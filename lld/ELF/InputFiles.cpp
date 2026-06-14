//===- InputFiles.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Config.h"
#include "DWARF.h"
#include "Driver.h"
#include "InputSection.h"
#include "LinkerScript.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "lld/Common/DWARF.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/IRObjectFile.h"
#include "llvm/Support/AArch64AttributeParser.h"
#include "llvm/Support/ARMAttributeParser.h"
#include "llvm/Support/ARMBuildAttributes.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace llvm::sys;
using namespace llvm::sys::fs;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::elf;

// This function is explicitly instantiated in ARM.cpp, don't do it here to
// avoid warnings with MSVC.
extern template void ObjFile<ELF32LE>::importCmseSymbols();
extern template void ObjFile<ELF32BE>::importCmseSymbols();
extern template void ObjFile<ELF64LE>::importCmseSymbols();
extern template void ObjFile<ELF64BE>::importCmseSymbols();

// Returns "<internal>", "foo.a(bar.o)" or "baz.o".
std::string elf::toStr(Ctx &ctx, const InputFile *f) {
  static std::mutex mu;
  if (!f)
    return "<internal>";

  {
    std::lock_guard<std::mutex> lock(mu);
    if (f->toStringCache.empty()) {
      if (f->archiveName.empty())
        f->toStringCache = f->getName();
      else
        (f->archiveName + "(" + f->getName() + ")").toVector(f->toStringCache);
    }
  }
  return std::string(f->toStringCache);
}

const ELFSyncStream &elf::operator<<(const ELFSyncStream &s,
                                     const InputFile *f) {
  return s << toStr(s.ctx, f);
}

static ELFKind getELFKind(Ctx &ctx, MemoryBufferRef mb, StringRef archiveName) {
  unsigned char size;
  unsigned char endian;
  std::tie(size, endian) = getElfArchType(mb.getBuffer());

  auto report = [&](StringRef msg) {
    StringRef filename = mb.getBufferIdentifier();
    if (archiveName.empty())
      Fatal(ctx) << filename << ": " << msg;
    else
      Fatal(ctx) << archiveName << "(" << filename << "): " << msg;
  };

  if (!mb.getBuffer().starts_with(ElfMagic))
    report("not an ELF file");
  if (endian != ELFDATA2LSB && endian != ELFDATA2MSB)
    report("corrupted ELF file: invalid data encoding");
  if (size != ELFCLASS32 && size != ELFCLASS64)
    report("corrupted ELF file: invalid file class");

  size_t bufSize = mb.getBuffer().size();
  if ((size == ELFCLASS32 && bufSize < sizeof(Elf32_Ehdr)) ||
      (size == ELFCLASS64 && bufSize < sizeof(Elf64_Ehdr)))
    report("corrupted ELF file: file is too short");

  if (size == ELFCLASS32)
    return (endian == ELFDATA2LSB) ? ELF32LEKind : ELF32BEKind;
  return (endian == ELFDATA2LSB) ? ELF64LEKind : ELF64BEKind;
}

// For ARM only, to set the EF_ARM_ABI_FLOAT_SOFT or EF_ARM_ABI_FLOAT_HARD
// flag in the ELF Header we need to look at Tag_ABI_VFP_args to find out how
// the input objects have been compiled.
static void updateARMVFPArgs(Ctx &ctx, const ARMAttributeParser &attributes,
                             const InputFile *f) {
  std::optional<unsigned> attr =
      attributes.getAttributeValue(ARMBuildAttrs::ABI_VFP_args);
  if (!attr)
    // If an ABI tag isn't present then it is implicitly given the value of 0
    // which maps to ARMBuildAttrs::BaseAAPCS. However many assembler files,
    // including some in glibc that don't use FP args (and should have value 3)
    // don't have the attribute so we do not consider an implicit value of 0
    // as a clash.
    return;

  unsigned vfpArgs = *attr;
  ARMVFPArgKind arg;
  switch (vfpArgs) {
  case ARMBuildAttrs::BaseAAPCS:
    arg = ARMVFPArgKind::Base;
    break;
  case ARMBuildAttrs::HardFPAAPCS:
    arg = ARMVFPArgKind::VFP;
    break;
  case ARMBuildAttrs::ToolChainFPPCS:
    // Tool chain specific convention that conforms to neither AAPCS variant.
    arg = ARMVFPArgKind::ToolChain;
    break;
  case ARMBuildAttrs::CompatibleFPAAPCS:
    // Object compatible with all conventions.
    return;
  default:
    ErrAlways(ctx) << f << ": unknown Tag_ABI_VFP_args value: " << vfpArgs;
    return;
  }
  // Follow ld.bfd and error if there is a mix of calling conventions.
  if (ctx.arg.armVFPArgs != arg && ctx.arg.armVFPArgs != ARMVFPArgKind::Default)
    ErrAlways(ctx) << f << ": incompatible Tag_ABI_VFP_args";
  else
    ctx.arg.armVFPArgs = arg;
}

// The ARM support in lld makes some use of instructions that are not available
// on all ARM architectures. Namely:
// - Use of BLX instruction for interworking between ARM and Thumb state.
// - Use of the extended Thumb branch encoding in relocation.
// - Use of the MOVT/MOVW instructions in Thumb Thunks.
// The ARM Attributes section contains information about the architecture chosen
// at compile time. We follow the convention that if at least one input object
// is compiled with an architecture that supports these features then lld is
// permitted to use them.
static void updateSupportedARMFeatures(Ctx &ctx,
                                       const ARMAttributeParser &attributes) {
  std::optional<unsigned> attr =
      attributes.getAttributeValue(ARMBuildAttrs::CPU_arch);
  if (!attr)
    return;
  auto arch = *attr;
  switch (arch) {
  case ARMBuildAttrs::Pre_v4:
  case ARMBuildAttrs::v4:
  case ARMBuildAttrs::v4T:
    // Architectures prior to v5 do not support BLX instruction
    break;
  case ARMBuildAttrs::v5T:
  case ARMBuildAttrs::v5TE:
  case ARMBuildAttrs::v5TEJ:
  case ARMBuildAttrs::v6:
  case ARMBuildAttrs::v6KZ:
  case ARMBuildAttrs::v6K:
    ctx.arg.armHasBlx = true;
    // Architectures used in pre-Cortex processors do not support
    // The J1 = 1 J2 = 1 Thumb branch range extension, with the exception
    // of Architecture v6T2 (arm1156t2-s and arm1156t2f-s) that do.
    break;
  default:
    // All other Architectures have BLX and extended branch encoding
    ctx.arg.armHasBlx = true;
    ctx.arg.armJ1J2BranchEncoding = true;
    if (arch != ARMBuildAttrs::v6_M && arch != ARMBuildAttrs::v6S_M)
      // All Architectures used in Cortex processors with the exception
      // of v6-M and v6S-M have the MOVT and MOVW instructions.
      ctx.arg.armHasMovtMovw = true;
    break;
  }

  // Only ARMv8-M or later architectures have CMSE support.
  std::optional<unsigned> profile =
      attributes.getAttributeValue(ARMBuildAttrs::CPU_arch_profile);
  if (!profile)
    return;
  if (arch >= ARMBuildAttrs::CPUArch::v8_M_Base &&
      profile == ARMBuildAttrs::MicroControllerProfile)
    ctx.arg.armCMSESupport = true;

  // The thumb PLT entries require Thumb2 which can be used on multiple archs.
  // For now, let's limit it to ones where ARM isn't available and we know have
  // Thumb2.
  std::optional<unsigned> armISA =
      attributes.getAttributeValue(ARMBuildAttrs::ARM_ISA_use);
  std::optional<unsigned> thumb =
      attributes.getAttributeValue(ARMBuildAttrs::THUMB_ISA_use);
  ctx.arg.armHasArmISA |= armISA && *armISA >= ARMBuildAttrs::Allowed;
  ctx.arg.armHasThumb2ISA |= thumb && *thumb >= ARMBuildAttrs::AllowThumb32;
}

InputFile::InputFile(Ctx &ctx, Kind k, MemoryBufferRef m)
    : ctx(ctx), mb(m), fileKind(k) {}

InputFile::~InputFile() {}

std::optional<MemoryBufferRef> elf::readFile(Ctx &ctx, StringRef path) {
  llvm::TimeTraceScope timeScope("Load input files", path);

  // The --chroot option changes our virtual root directory.
  // This is useful when you are dealing with files created by --reproduce.
  if (!ctx.arg.chroot.empty() && path.starts_with("/"))
    path = ctx.saver.save(ctx.arg.chroot + path);

  bool remapped = false;
  auto it = ctx.arg.remapInputs.find(path);
  if (it != ctx.arg.remapInputs.end()) {
    path = it->second;
    remapped = true;
  } else {
    for (const auto &[pat, toFile] : ctx.arg.remapInputsWildcards) {
      if (pat.match(path)) {
        path = toFile;
        remapped = true;
        break;
      }
    }
  }
  if (remapped) {
    // Use /dev/null to indicate an input file that should be ignored. Change
    // the path to NUL on Windows.
#ifdef _WIN32
    if (path == "/dev/null")
      path = "NUL";
#endif
  }

  Log(ctx) << path;
  ctx.arg.dependencyFiles.insert(llvm::CachedHashString(path));

  auto mbOrErr = MemoryBuffer::getFile(path, /*IsText=*/false,
                                       /*RequiresNullTerminator=*/false);
  if (auto ec = mbOrErr.getError()) {
    ErrAlways(ctx) << "cannot open " << path << ": " << ec.message();
    return std::nullopt;
  }

  MemoryBufferRef mbref = (*mbOrErr)->getMemBufferRef();
  ctx.memoryBuffers.push_back(std::move(*mbOrErr)); // take MB ownership

  if (ctx.tar)
    ctx.tar->append(relativeToRoot(path), mbref.getBuffer());
  return mbref;
}

// All input object files must be for the same architecture
// (e.g. it does not make sense to link x86 object files with
// MIPS object files.) This function checks for that error.
// firstObj/firstShared/firstBitcode name an already-accepted file for the
// fallback diagnostic; the serial path derives them from ctx, the parallel
// parse pipeline tracks them while scanning.
static bool isCompatible(Ctx &ctx, InputFile *file, InputFile *firstObj,
                         InputFile *firstShared, InputFile *firstBitcode) {
  if (!file->isElf() && !isa<BitcodeFile>(file))
    return true;

  if (file->ekind == ctx.arg.ekind && file->emachine == ctx.arg.emachine) {
    if (ctx.arg.emachine != EM_MIPS)
      return true;
    if (isMipsN32Abi(ctx, *file) == ctx.arg.mipsN32Abi)
      return true;
  }

  StringRef target =
      !ctx.arg.bfdname.empty() ? ctx.arg.bfdname : ctx.arg.emulation;
  if (!target.empty()) {
    Err(ctx) << file << " is incompatible with " << target;
    return false;
  }

  InputFile *existing = nullptr;
  if (firstObj)
    existing = firstObj;
  else if (firstShared)
    existing = firstShared;
  else if (firstBitcode)
    existing = firstBitcode;
  auto diag = Err(ctx);
  diag << file << " is incompatible";
  if (existing)
    diag << " with " << existing;
  return false;
}

static bool isCompatible(Ctx &ctx, InputFile *file) {
  return isCompatible(ctx, file,
                      ctx.objectFiles.empty() ? nullptr : ctx.objectFiles[0],
                      ctx.sharedFiles.empty() ? nullptr : ctx.sharedFiles[0],
                      ctx.bitcodeFiles.empty() ? nullptr : ctx.bitcodeFiles[0]);
}

// -t traces are printed by Symbol::extract and the parallel parse pipeline
// (commitFiles/epilogue), not here.
template <class ELFT> static void doParseFile(Ctx &ctx, InputFile *file) {
  if (!isCompatible(ctx, file))
    return;

  // Lazy object file
  if (file->lazy) {
    if (auto *f = dyn_cast<BitcodeFile>(file)) {
      ctx.lazyBitcodeFiles.push_back(f);
      f->parseLazy();
    } else {
      cast<ObjFile<ELFT>>(file)->parseLazy();
    }
    return;
  }

  if (file->kind() == InputFile::ObjKind) {
    ctx.objectFiles.push_back(cast<ELFFileBase>(file));
    cast<ObjFile<ELFT>>(file)->parse();
  } else if (auto *f = dyn_cast<SharedFile>(file)) {
    f->parse<ELFT>();
  } else if (auto *f = dyn_cast<BitcodeFile>(file)) {
    ctx.bitcodeFiles.push_back(f);
    f->parse();
  } else {
    ctx.binaryFiles.push_back(cast<BinaryFile>(file));
    cast<BinaryFile>(file)->parse();
  }
}

// Add symbols in File to the symbol table.
void elf::parseFile(Ctx &ctx, InputFile *file) {
  invokeELFT(doParseFile, ctx, file);
}

// Concatenates arguments to construct a string representing an error location.
StringRef InputFile::getNameForScript() const {
  if (archiveName.empty())
    return getName();

  if (nameForScriptCache.empty())
    nameForScriptCache = (archiveName + Twine(':') + getName()).str();

  return nameForScriptCache;
}

// An ELF object file may contain a `.deplibs` section. If it exists, the
// section contains a list of library specifiers such as `m` for libm. This
// function resolves a given name by finding the first matching library checking
// the various ways that a library can be specified to LLD. This ELF extension
// is a form of autolinking and is called `dependent libraries`. It is currently
// unique to LLVM and lld.
static void addDependentLibrary(Ctx &ctx, StringRef specifier,
                                const InputFile *f) {
  if (!ctx.arg.dependentLibraries)
    return;
  if (std::optional<std::string> s = searchLibraryBaseName(ctx, specifier))
    ctx.driver.addFile(ctx.saver.save(*s), /*withLOption=*/true);
  else if (std::optional<std::string> s = findFromSearchPaths(ctx, specifier))
    ctx.driver.addFile(ctx.saver.save(*s), /*withLOption=*/true);
  else if (fs::exists(specifier))
    ctx.driver.addFile(specifier, /*withLOption=*/false);
  else
    ErrAlways(ctx)
        << f << ": unable to find library from dependent library specifier: "
        << specifier;
}

// Record the membership of a section group so that in the garbage collection
// pass, section group members are kept or discarded as a unit.
template <class ELFT>
static void handleSectionGroup(ArrayRef<InputSectionBase *> sections,
                               ArrayRef<typename ELFT::Word> entries) {
  bool hasAlloc = false;
  for (uint32_t index : entries.slice(1)) {
    if (index >= sections.size())
      return;
    if (InputSectionBase *s = sections[index])
      if (s != &InputSection::discarded && s->flags & SHF_ALLOC)
        hasAlloc = true;
  }

  // If any member has the SHF_ALLOC flag, the whole group is subject to garbage
  // collection. See the comment in markLive(). This rule retains .debug_types
  // and .rela.debug_types.
  if (!hasAlloc)
    return;

  // Connect the members in a circular doubly-linked list via
  // nextInSectionGroup.
  InputSectionBase *head;
  InputSectionBase *prev = nullptr;
  for (uint32_t index : entries.slice(1)) {
    InputSectionBase *s = sections[index];
    if (!s || s == &InputSection::discarded)
      continue;
    if (prev)
      prev->nextInSectionGroup = s;
    else
      head = s;
    prev = s;
  }
  if (prev)
    prev->nextInSectionGroup = head;
}

template <class ELFT> void ObjFile<ELFT>::initDwarf() {
  dwarf = std::make_unique<DWARFCache>(std::make_unique<DWARFContext>(
      std::make_unique<LLDDwarfObj<ELFT>>(this), "",
      [&](Error err) { Warn(ctx) << getName() + ": " << std::move(err); },
      [&](Error warning) {
        Warn(ctx) << getName() << ": " << std::move(warning);
      }));
}

DWARFCache *ELFFileBase::getDwarf() {
  assert(fileKind == ObjKind);
  llvm::call_once(initDwarf, [this]() {
    switch (ekind) {
    default:
      llvm_unreachable("");
    case ELF32LEKind:
      return cast<ObjFile<ELF32LE>>(this)->initDwarf();
    case ELF32BEKind:
      return cast<ObjFile<ELF32BE>>(this)->initDwarf();
    case ELF64LEKind:
      return cast<ObjFile<ELF64LE>>(this)->initDwarf();
    case ELF64BEKind:
      return cast<ObjFile<ELF64BE>>(this)->initDwarf();
    }
  });
  return dwarf.get();
}

ELFFileBase::ELFFileBase(Ctx &ctx, Kind k, ELFKind ekind, MemoryBufferRef mb)
    : InputFile(ctx, k, mb) {
  this->ekind = ekind;
}

ELFFileBase::~ELFFileBase() {}

template <typename Elf_Shdr>
static const Elf_Shdr *findSection(ArrayRef<Elf_Shdr> sections, uint32_t type) {
  for (const Elf_Shdr &sec : sections)
    if (sec.sh_type == type)
      return &sec;
  return nullptr;
}

void ELFFileBase::init() {
  switch (ekind) {
  case ELF32LEKind:
    init<ELF32LE>(fileKind);
    break;
  case ELF32BEKind:
    init<ELF32BE>(fileKind);
    break;
  case ELF64LEKind:
    init<ELF64LE>(fileKind);
    break;
  case ELF64BEKind:
    init<ELF64BE>(fileKind);
    break;
  default:
    llvm_unreachable("getELFKind");
  }
}

template <class ELFT> void ELFFileBase::init(InputFile::Kind k) {
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;

  // Initialize trivial attributes.
  const ELFFile<ELFT> &obj = getObj<ELFT>();
  emachine = obj.getHeader().e_machine;
  osabi = obj.getHeader().e_ident[llvm::ELF::EI_OSABI];
  abiVersion = obj.getHeader().e_ident[llvm::ELF::EI_ABIVERSION];

  ArrayRef<Elf_Shdr> sections = CHECK2(obj.sections(), this);
  elfShdrs = sections.data();
  numELFShdrs = sections.size();

  // Find a symbol table.
  const Elf_Shdr *symtabSec =
      findSection(sections, k == SharedKind ? SHT_DYNSYM : SHT_SYMTAB);

  if (!symtabSec)
    return;

  // Initialize members corresponding to a symbol table.
  firstGlobal = symtabSec->sh_info;

  ArrayRef<Elf_Sym> eSyms = CHECK2(obj.symbols(symtabSec), this);
  if (firstGlobal == 0 || firstGlobal > eSyms.size())
    Fatal(ctx) << this << ": invalid sh_info in symbol table";

  elfSyms = reinterpret_cast<const void *>(eSyms.data());
  numSymbols = eSyms.size();
  stringTable = CHECK2(obj.getStringTableForSymtab(*symtabSec, sections), this);
}

template <class ELFT>
uint32_t ObjFile<ELFT>::getSectionIndex(const Elf_Sym &sym) const {
  return CHECK2(
      this->getObj().getSectionIndex(sym, getELFSyms<ELFT>(), shndxTable),
      this);
}

template <class ELFT>
static void
handleAArch64BAAndGnuProperties(ObjFile<ELFT> *file, Ctx &ctx,
                                const AArch64BuildAttrSubsections &baInfo) {
  if (file->aarch64PauthAbiCoreInfo) {
    // Check for data mismatch.
    if (file->aarch64PauthAbiCoreInfo) {
      if (baInfo.Pauth.TagPlatform != file->aarch64PauthAbiCoreInfo->platform ||
          baInfo.Pauth.TagSchema != file->aarch64PauthAbiCoreInfo->version)
        Err(ctx) << file
                 << " GNU properties and build attributes have conflicting "
                    "AArch64 PAuth data";
    }
    if (baInfo.AndFeatures != file->andFeatures)
      Err(ctx) << file
               << " GNU properties and build attributes have conflicting "
                  "AArch64 PAuth data";
  } else {
    // When BuildAttributes are missing, PauthABI value defaults to (TagPlatform
    // = 0, TagSchema = 0). GNU properties do not write PAuthAbiCoreInfo if GNU
    // property is not present. To match this behaviour, we only write
    // PAuthAbiCoreInfo when there is at least one non-zero value. The
    // specification reserves TagPlatform = 0, TagSchema = 1 values to match the
    // 'Invalid' GNU property section with platform = 0, version = 0.
    if (baInfo.Pauth.TagPlatform || baInfo.Pauth.TagSchema) {
      if (baInfo.Pauth.TagPlatform == 0 && baInfo.Pauth.TagSchema == 1)
        file->aarch64PauthAbiCoreInfo = {0, 0};
      else
        file->aarch64PauthAbiCoreInfo = {baInfo.Pauth.TagPlatform,
                                         baInfo.Pauth.TagSchema};
    }
    file->andFeatures = baInfo.AndFeatures;
  }
}

template <class ELFT> void ObjFile<ELFT>::parse(bool ignoreComdats) {
  object::ELFFile<ELFT> obj = this->getObj();
  // --just-symbols files have only their symbol table read here; the section
  // table is initialized in initSectionsAndLocalSyms.
  if (!this->justSymbols) {
    // Register comdat group ownership and process dependent libraries and
    // SHT_ARM_ATTRIBUTES, which are not done in parallel. Group selection,
    // diagnostics, and section creation happen in initializeSections.
    EarlySectionInfo early;
    scanEarlySections(early);
    if (!ignoreComdats)
      for (StringRef sig : early.comdats)
        ctx.symtab->addComdatGroup(CachedHashStringRef(sig), this);
    processEarlySections(early);
  }

  // Read a symbol table.
  initializeSymbols(obj);
}

template <class ELFT>
void ObjFile<ELFT>::scanEarlySections(EarlySectionInfo &out) {
  ArrayRef<Elf_Shdr> objSections = getELFShdrs<ELFT>();
  for (size_t i = 0, size = objSections.size(); i != size; ++i) {
    const Elf_Shdr &sec = objSections[i];
    if (LLVM_LIKELY(sec.sh_type == SHT_PROGBITS))
      continue;
    if (sec.sh_type == SHT_GROUP) {
      Expected<std::pair<StringRef, ArrayRef<Elf_Word>>> group = getGroup(sec);
      if (!group) {
        consumeError(group.takeError());
        continue;
      }
      if (group->second[0] == Elf_Word(GRP_COMDAT))
        out.comdats.push_back(group->first);
      continue;
    }
    if (sec.sh_type == SHT_LLVM_DEPENDENT_LIBRARIES && !ctx.arg.relocatable) {
      out.deplibSections.push_back(i);
      continue;
    }
    if (sec.sh_type == SHT_ARM_ATTRIBUTES && ctx.arg.emachine == EM_ARM)
      out.attributeSections.push_back(i);
  }
}

template <class ELFT>
void ObjFile<ELFT>::processEarlySections(const EarlySectionInfo &early) {
  if (early.deplibSections.empty() && early.attributeSections.empty())
    return;
  object::ELFFile<ELFT> obj = this->getObj();
  ArrayRef<Elf_Shdr> objSections = getELFShdrs<ELFT>();
  StringRef shstrtab = CHECK2(obj.getSectionStringTable(objSections), this);
  for (uint32_t i : early.deplibSections) {
    const Elf_Shdr &sec = objSections[i];
    StringRef name = check(obj.getSectionName(sec, shstrtab));
    ArrayRef<char> data =
        CHECK2(obj.template getSectionContentsAsArray<char>(sec), this);
    if (!data.empty() && data.back() != '\0') {
      Err(ctx)
          << this
          << ": corrupted dependent libraries section (unterminated string): "
          << name;
    } else {
      for (const char *d = data.begin(), *e = data.end(); d < e;) {
        StringRef s(d);
        addDependentLibrary(ctx, s, this);
        d += s.size() + 1;
      }
    }
  }
  for (uint32_t i : early.attributeSections) {
    const Elf_Shdr &sec = objSections[i];
    ARMAttributeParser attributes;
    ArrayRef<uint8_t> contents = check(obj.getSectionContents(sec));
    StringRef name = check(obj.getSectionName(sec, shstrtab));
    if (Error e = attributes.parse(contents, ekind == ELF32LEKind
                                                 ? llvm::endianness::little
                                                 : llvm::endianness::big)) {
      InputSection isec(*this, sec, name);
      Warn(ctx) << &isec << ": " << std::move(e);
    } else {
      updateSupportedARMFeatures(ctx, attributes);
      updateARMVFPArgs(ctx, attributes, this);

      // FIXME: Retain the first attribute section we see. The eglibc ARM
      // dynamic loaders require the presence of an attribute section for
      // dlopen to work. In a full implementation we would merge all
      // attribute sections.
      if (ctx.in.attributes == nullptr) {
        ctx.in.attributes = std::make_unique<InputSection>(*this, sec, name);
        armAttrSecIdx = i;
      }
    }
  }
}

// Sections with SHT_GROUP and comdat bits define comdat section groups.
// They are identified and deduplicated by group name. Decode a SHT_GROUP
// section's signature name and entries. scanEarlySections consumes errors;
// initializeSections diagnoses them.
template <class ELFT>
Expected<std::pair<StringRef, ArrayRef<typename ELFT::Word>>>
ObjFile<ELFT>::getGroup(const Elf_Shdr &sec) {
  typename ELFT::SymRange eSyms = this->getELFSyms<ELFT>();
  if (sec.sh_info >= eSyms.size())
    return createStringError("invalid symbol index");
  Expected<StringRef> signature = eSyms[sec.sh_info].getName(stringTable);
  if (!signature)
    return signature.takeError();
  Expected<ArrayRef<Elf_Word>> entries =
      getObj().template getSectionContentsAsArray<Elf_Word>(sec);
  if (!entries)
    return entries.takeError();
  if (entries->empty())
    return createStringError("empty SHT_GROUP");
  Elf_Word flag = (*entries)[0];
  if (flag && flag != Elf_Word(GRP_COMDAT))
    return createStringError("unsupported SHT_GROUP format");
  return std::make_pair(*signature, *entries);
}

template <class ELFT>
bool ObjFile<ELFT>::shouldMerge(const Elf_Shdr &sec, StringRef name) {
  // On a regular link we don't merge sections if -O0 (default is -O1). This
  // sometimes makes the linker significantly faster, although the output will
  // be bigger.
  //
  // Doing the same for -r would create a problem as it would combine sections
  // with different sh_entsize. One option would be to just copy every SHF_MERGE
  // section as is to the output. While this would produce a valid ELF file with
  // usable SHF_MERGE sections, tools like (llvm-)?dwarfdump get confused when
  // they see two .debug_str. We could have separate logic for combining
  // SHF_MERGE sections based both on their name and sh_entsize, but that seems
  // to be more trouble than it is worth. Instead, we just use the regular (-O1)
  // logic for -r.
  if (ctx.arg.optimize == 0 && !ctx.arg.relocatable)
    return false;

  // A mergeable section with size 0 is useless because they don't have
  // any data to merge. A mergeable string section with size 0 can be
  // argued as invalid because it doesn't end with a null character.
  // We'll avoid a mess by handling them as if they were non-mergeable.
  if (sec.sh_size == 0)
    return false;

  // Check for sh_entsize. The ELF spec is not clear about the zero
  // sh_entsize. It says that "the member [sh_entsize] contains 0 if
  // the section does not hold a table of fixed-size entries". We know
  // that Rust 1.13 produces a string mergeable section with a zero
  // sh_entsize. Here we just accept it rather than being picky about it.
  uint64_t entSize = sec.sh_entsize;
  if (entSize == 0)
    return false;
  if (sec.sh_size % entSize)
    ErrAlways(ctx) << this << ":(" << name << "): SHF_MERGE section size ("
                   << uint64_t(sec.sh_size)
                   << ") must be a multiple of sh_entsize (" << entSize << ")";
  if (sec.sh_flags & SHF_WRITE)
    Err(ctx) << this << ":(" << name
             << "): writable SHF_MERGE section is not supported";

  return true;
}

// This is for --just-symbols.
//
// --just-symbols is a very minor feature that allows you to link your
// output against other existing program, so that if you load both your
// program and the other program into memory, your output can refer the
// other program's symbols.
//
// When the option is given, we link "just symbols". The section table is
// initialized with null pointers.
template <class ELFT> void ObjFile<ELFT>::initializeJustSymbols() {
  sections.resize(numELFShdrs);
}

static bool isKnownSpecificSectionType(uint32_t t, uint32_t flags) {
  if (SHT_LOUSER <= t && t <= SHT_HIUSER && !(flags & SHF_ALLOC))
    return true;
  if (SHT_LOOS <= t && t <= SHT_HIOS && !(flags & SHF_OS_NONCONFORMING))
    return true;
  // Allow all processor-specific types. This is different from GNU ld.
  return SHT_LOPROC <= t && t <= SHT_HIPROC;
}

template <class ELFT>
void ObjFile<ELFT>::initializeSections(bool ignoreComdats,
                                       const llvm::object::ELFFile<ELFT> &obj) {
  ArrayRef<Elf_Shdr> objSections = getELFShdrs<ELFT>();
  StringRef shstrtab = CHECK2(obj.getSectionStringTable(objSections), this);
  uint64_t size = objSections.size();
  this->sections.resize(size);

  // First pass over SHT_GROUP sections: diagnose malformed groups and discard
  // members of non-prevailing comdat groups. Comdat group ownership was
  // registered by the parallel parse pipeline or the serial parse().
  // keptGroups memoizes the verdict for the main loop below.
  SmallVector<std::pair<uint32_t, ArrayRef<Elf_Word>>, 0> keptGroups;
  for (size_t i = 0; i != size; ++i) {
    const Elf_Shdr &sec = objSections[i];
    if (LLVM_LIKELY(sec.sh_type != SHT_GROUP))
      continue;
    Expected<std::pair<StringRef, ArrayRef<Elf_Word>>> group = getGroup(sec);
    if (!group) {
      Err(ctx) << this << ": " << group.takeError();
      this->sections[i] = &InputSection::discarded;
      continue;
    }
    auto [signature, entries] = *group;
    bool keepGroup =
        !entries[0] || ignoreComdats ||
        ctx.symtab->findComdatGroup(CachedHashStringRef(signature)) == this;
    if (keepGroup) {
      keptGroups.push_back({(uint32_t)i, entries});
      if (!ctx.arg.resolveGroups)
        this->sections[i] = createInputSection(
            i, sec, check(obj.getSectionName(sec, shstrtab)));
      continue;
    }
    // Otherwise, discard group members.
    for (uint32_t secIndex : entries.slice(1)) {
      if (secIndex >= size) {
        Err(ctx) << this << ": invalid section index in group: " << secIndex;
        continue;
      }
      this->sections[secIndex] = &InputSection::discarded;
    }
  }

  SmallVector<ArrayRef<Elf_Word>, 0> selectedGroups;
  size_t keptGroupIdx = 0;
  AArch64BuildAttrSubsections aarch64BAsubSections;
  bool hasAArch64BuildAttributes = false;
  for (size_t i = 0; i != size; ++i) {
    if (this->sections[i] == &InputSection::discarded)
      continue;
    const Elf_Shdr &sec = objSections[i];
    const uint32_t type = sec.sh_type;

    // SHF_EXCLUDE'ed sections are discarded by the linker. However,
    // if -r is given, we'll let the final link discard such sections.
    // This is compatible with GNU.
    if ((sec.sh_flags & SHF_EXCLUDE) && !ctx.arg.relocatable) {
      if (type == SHT_LLVM_CALL_GRAPH_PROFILE)
        cgProfileSectionIndex = i;
      if (type == SHT_LLVM_ADDRSIG) {
        // We ignore the address-significance table if we know that the object
        // file was created by objcopy or ld -r. This is because these tools
        // will reorder the symbols in the symbol table, invalidating the data
        // in the address-significance table, which refers to symbols by index.
        if (sec.sh_link != 0)
          this->addrsigSec = &sec;
        else if (ctx.arg.icf == ICFLevel::Safe)
          Warn(ctx) << this
                    << ": --icf=safe conservatively ignores "
                       "SHT_LLVM_ADDRSIG [index "
                    << i
                    << "] with sh_link=0 "
                       "(likely created using objcopy or ld -r)";
      }
      this->sections[i] = &InputSection::discarded;
      continue;
    }

    // Processor-specific types that do not use the following switch statement.
    //
    // Extract Build Attributes section contents into aarch64BAsubSections.
    // Input objects may contain both build Build Attributes and GNU
    // properties. We delay processing Build Attributes until we have finished
    // reading all sections so that we can check that these are consistent.
    if (type == SHT_AARCH64_ATTRIBUTES && ctx.arg.emachine == EM_AARCH64) {
      ArrayRef<uint8_t> contents = check(obj.getSectionContents(sec));
      AArch64AttributeParser attributes;
      if (Error e = attributes.parse(contents, ELFT::Endianness)) {
        StringRef name = check(obj.getSectionName(sec, shstrtab));
        InputSection isec(*this, sec, name);
        Warn(ctx) << &isec << ": " << std::move(e);
      } else {
        aarch64BAsubSections = extractBuildAttributesSubsections(attributes);
        hasAArch64BuildAttributes = true;
      }
      this->sections[i] = &InputSection::discarded;
      continue;
    }
    if (type == SHT_ARM_ATTRIBUTES && ctx.arg.emachine == EM_ARM) {
      // The retained attribute section (if this file provides it) was created
      // by processEarlySections.
      this->sections[i] = i == armAttrSecIdx
                              ? ctx.in.attributes.get()
                              : (InputSectionBase *)&InputSection::discarded;
      continue;
    }
    // Producing a static binary with MTE globals is not currently supported,
    // remove all SHT_AARCH64_MEMTAG_GLOBALS_STATIC sections as they're unused
    // medatada, and we don't want them to end up in the output file for
    // static executables.
    if (type == SHT_AARCH64_MEMTAG_GLOBALS_STATIC &&
        ctx.arg.emachine == EM_AARCH64 && !canHaveMemtagGlobals(ctx)) {
      this->sections[i] = &InputSection::discarded;
      continue;
    }
    if (type == SHT_LLVM_DEPENDENT_LIBRARIES && !ctx.arg.relocatable) {
      // The contents were processed by processEarlySections.
      this->sections[i] = &InputSection::discarded;
      continue;
    }
    switch (type) {
    case SHT_GROUP: {
      if (!ctx.arg.relocatable)
        sections[i] = &InputSection::discarded;
      // The verdict was computed by the first pass above. Kept groups may
      // have been discarded since (e.g. as a member of another group).
      while (keptGroupIdx != keptGroups.size() &&
             keptGroups[keptGroupIdx].first < i)
        ++keptGroupIdx;
      if (keptGroupIdx != keptGroups.size() &&
          keptGroups[keptGroupIdx].first == i)
        selectedGroups.push_back(keptGroups[keptGroupIdx++].second);
      break;
    }
    case SHT_SYMTAB_SHNDX:
      shndxTable = CHECK2(obj.getSHNDXTable(sec, objSections), this);
      break;
    case SHT_SYMTAB:
    case SHT_STRTAB:
    case SHT_REL:
    case SHT_RELA:
    case SHT_CREL:
    case SHT_NULL:
      break;
    case SHT_PROGBITS:
    case SHT_NOTE:
    case SHT_NOBITS:
    case SHT_INIT_ARRAY:
    case SHT_FINI_ARRAY:
    case SHT_PREINIT_ARRAY:
      this->sections[i] =
          createInputSection(i, sec, check(obj.getSectionName(sec, shstrtab)));
      break;
    case SHT_LLVM_LTO:
      // Discard .llvm.lto in a relocatable link that does not use the bitcode.
      // The concatenated output does not properly reflect the linking
      // semantics. In addition, since we do not use the bitcode wrapper format,
      // the concatenated raw bitcode would be invalid.
      if (ctx.arg.relocatable && !ctx.arg.fatLTOObjects) {
        sections[i] = &InputSection::discarded;
        break;
      }
      [[fallthrough]];
    default:
      this->sections[i] =
          createInputSection(i, sec, check(obj.getSectionName(sec, shstrtab)));
      if (ctx.arg.rejectMismatch &&
          !isKnownSpecificSectionType(type, sec.sh_flags))
        Err(ctx) << this->sections[i] << ": unknown section type 0x"
                 << Twine::utohexstr(type);
      break;
    }
  }

  // We have a second loop. It is used to:
  // 1) handle SHF_LINK_ORDER sections.
  // 2) create relocation sections. In some cases the section header index of a
  //    relocation section may be smaller than that of the relocated section. In
  //    such cases, the relocation section would attempt to reference a target
  //    section that has not yet been created. For simplicity, delay creation of
  //    relocation sections until now.
  for (size_t i = 0; i != size; ++i) {
    if (this->sections[i] == &InputSection::discarded)
      continue;
    const Elf_Shdr &sec = objSections[i];

    if (isStaticRelSecType(sec.sh_type)) {
      // Find a relocation target section and associate this section with that.
      // Target may have been discarded if it is in a different section group
      // and the group is discarded, even though it's a violation of the spec.
      // We handle that situation gracefully by discarding dangling relocation
      // sections.
      const uint32_t info = sec.sh_info;
      InputSectionBase *s = getRelocTarget(i, info);
      if (!s)
        continue;

      // ELF spec allows mergeable sections with relocations, but they are rare,
      // and it is in practice hard to merge such sections by contents, because
      // applying relocations at end of linking changes section contents. So, we
      // simply handle such sections as non-mergeable ones. Degrading like this
      // is acceptable because section merging is optional.
      if (auto *ms = dyn_cast<MergeInputSection>(s)) {
        s = makeThreadLocal<InputSection>(ms->file, ms->name, ms->type,
                                          ms->flags, ms->addralign, ms->entsize,
                                          ms->contentMaybeDecompress());
        sections[info] = s;
      }

      if (s->relSecIdx != 0)
        ErrAlways(ctx) << s
                       << ": multiple relocation sections to one section are "
                          "not supported";
      s->relSecIdx = i;

      // Relocation sections are usually removed from the output, so return
      // `nullptr` for the normal case. However, if -r or --emit-relocs is
      // specified, we need to copy them to the output. (Some post link analysis
      // tools specify --emit-relocs to obtain the information.)
      if (ctx.arg.copyRelocs) {
        auto *isec = makeThreadLocal<InputSection>(
            *this, sec, check(obj.getSectionName(sec, shstrtab)));
        // If the relocated section is discarded (due to /DISCARD/ or
        // --gc-sections), the relocation section should be discarded as well.
        s->dependentSections.push_back(isec);
        sections[i] = isec;
      }
      continue;
    }

    // A SHF_LINK_ORDER section with sh_link=0 is handled as if it did not have
    // the flag.
    if (!sec.sh_link || !(sec.sh_flags & SHF_LINK_ORDER))
      continue;

    InputSectionBase *linkSec = nullptr;
    if (sec.sh_link < size)
      linkSec = this->sections[sec.sh_link];
    if (!linkSec) {
      ErrAlways(ctx) << this
                     << ": invalid sh_link index: " << uint32_t(sec.sh_link);
      continue;
    }

    // A SHF_LINK_ORDER section is discarded if its linked-to section is
    // discarded.
    InputSection *isec = cast<InputSection>(this->sections[i]);
    linkSec->dependentSections.push_back(isec);
    if (!isa<InputSection>(linkSec))
      ErrAlways(ctx)
          << "a section " << isec->name
          << " with SHF_LINK_ORDER should not refer a non-regular section: "
          << linkSec;
  }

  // Handle AArch64 Build Attributes and GNU properties:
  // - Err on mismatched values.
  // - Store missing values as GNU properties.
  if (hasAArch64BuildAttributes)
    handleAArch64BAAndGnuProperties<ELFT>(this, ctx, aarch64BAsubSections);

  for (ArrayRef<Elf_Word> entries : selectedGroups)
    handleSectionGroup<ELFT>(this->sections, entries);
}

template <typename ELFT>
static void parseGnuPropertyNote(Ctx &ctx, ELFFileBase &f,
                                 uint32_t featureAndType,
                                 ArrayRef<uint8_t> &desc, const uint8_t *base,
                                 ArrayRef<uint8_t> *data = nullptr) {
  auto err = [&](const uint8_t *place) -> ELFSyncStream {
    auto diag = Err(ctx);
    diag << &f << ":(" << ".note.gnu.property+0x"
         << Twine::utohexstr(place - base) << "): ";
    return diag;
  };

  while (!desc.empty()) {
    const uint8_t *place = desc.data();
    if (desc.size() < 8)
      return void(err(place) << "program property is too short");
    uint32_t type = read32<ELFT::Endianness>(desc.data());
    uint32_t size = read32<ELFT::Endianness>(desc.data() + 4);
    desc = desc.slice(8);
    if (desc.size() < size)
      return void(err(place) << "program property is too short");

    if (type == featureAndType) {
      // We found a FEATURE_1_AND field. There may be more than one of these
      // in a .note.gnu.property section, for a relocatable object we
      // accumulate the bits set.
      if (size < 4)
        return void(err(place) << "FEATURE_1_AND entry is too short");
      f.andFeatures |= read32<ELFT::Endianness>(desc.data());
    } else if (ctx.arg.emachine == EM_AARCH64 &&
               type == GNU_PROPERTY_AARCH64_FEATURE_PAUTH) {
      ArrayRef<uint8_t> contents = data ? *data : desc;
      if (f.aarch64PauthAbiCoreInfo) {
        return void(
            err(contents.data())
            << "multiple GNU_PROPERTY_AARCH64_FEATURE_PAUTH entries are "
               "not supported");
      } else if (size != 16) {
        return void(err(contents.data())
                    << "GNU_PROPERTY_AARCH64_FEATURE_PAUTH entry "
                       "is invalid: expected 16 bytes, but got "
                    << size);
      }
      f.aarch64PauthAbiCoreInfo = {
          support::endian::read64<ELFT::Endianness>(&desc[0]),
          support::endian::read64<ELFT::Endianness>(&desc[8])};
    }

    // Padding is present in the note descriptor, if necessary.
    desc = desc.slice(alignTo<(ELFT::Is64Bits ? 8 : 4)>(size));
  }
}
// Read the following info from the .note.gnu.property section and write it to
// the corresponding fields in `ObjFile`:
// - Feature flags (32 bits) representing x86, AArch64 or RISC-V features for
//   hardware-assisted call flow control;
// - AArch64 PAuth ABI core info (16 bytes).
template <class ELFT>
static void readGnuProperty(Ctx &ctx, const InputSection &sec,
                            ObjFile<ELFT> &f) {
  using Elf_Nhdr = typename ELFT::Nhdr;
  using Elf_Note = typename ELFT::Note;

  uint32_t featureAndType;
  switch (ctx.arg.emachine) {
  case EM_386:
  case EM_X86_64:
    featureAndType = GNU_PROPERTY_X86_FEATURE_1_AND;
    break;
  case EM_AARCH64:
    featureAndType = GNU_PROPERTY_AARCH64_FEATURE_1_AND;
    break;
  case EM_RISCV:
    featureAndType = GNU_PROPERTY_RISCV_FEATURE_1_AND;
    break;
  default:
    return;
  }

  ArrayRef<uint8_t> data = sec.content();
  auto err = [&](const uint8_t *place) -> ELFSyncStream {
    auto diag = Err(ctx);
    diag << sec.file << ":(" << sec.name << "+0x"
         << Twine::utohexstr(place - sec.content().data()) << "): ";
    return diag;
  };
  while (!data.empty()) {
    // Read one NOTE record.
    auto *nhdr = reinterpret_cast<const Elf_Nhdr *>(data.data());
    if (data.size() < sizeof(Elf_Nhdr) ||
        data.size() < nhdr->getSize(sec.addralign))
      return void(err(data.data()) << "data is too short");

    Elf_Note note(*nhdr);
    if (nhdr->n_type != NT_GNU_PROPERTY_TYPE_0 || note.getName() != "GNU") {
      data = data.slice(nhdr->getSize(sec.addralign));
      continue;
    }

    // Read a body of a NOTE record, which consists of type-length-value fields.
    ArrayRef<uint8_t> desc = note.getDesc(sec.addralign);
    const uint8_t *base = sec.content().data();
    parseGnuPropertyNote<ELFT>(ctx, f, featureAndType, desc, base, &data);

    // Go to next NOTE record to look for more FEATURE_1_AND descriptions.
    data = data.slice(nhdr->getSize(sec.addralign));
  }
}

template <class ELFT>
InputSectionBase *ObjFile<ELFT>::getRelocTarget(uint32_t idx, uint32_t info) {
  if (info < this->sections.size()) {
    InputSectionBase *target = this->sections[info];

    // Strictly speaking, a relocation section must be included in the
    // group of the section it relocates. However, LLVM 3.3 and earlier
    // would fail to do so, so we gracefully handle that case.
    if (target == &InputSection::discarded)
      return nullptr;

    if (target != nullptr)
      return target;
  }

  Err(ctx) << this << ": relocation section (index " << idx
           << ") has invalid sh_info (" << info << ')';
  return nullptr;
}

// The function may be called concurrently for different input files. For
// allocation, prefer makeThreadLocal which does not require holding a lock.
template <class ELFT>
InputSectionBase *ObjFile<ELFT>::createInputSection(uint32_t idx,
                                                    const Elf_Shdr &sec,
                                                    StringRef name) {
  if (name.starts_with(".n")) {
    // The GNU linker uses .note.GNU-stack section as a marker indicating
    // that the code in the object file does not expect that the stack is
    // executable (in terms of NX bit). If all input files have the marker,
    // the GNU linker adds a PT_GNU_STACK segment to tells the loader to
    // make the stack non-executable. Most object files have this section as
    // of 2017.
    //
    // But making the stack non-executable is a norm today for security
    // reasons. Failure to do so may result in a serious security issue.
    // Therefore, we make LLD always add PT_GNU_STACK unless it is
    // explicitly told to do otherwise (by -z execstack). Because the stack
    // executable-ness is controlled solely by command line options,
    // .note.GNU-stack sections are, with one exception, ignored. Report
    // an error if we encounter an executable .note.GNU-stack to force the
    // user to explicitly request an executable stack.
    if (name == ".note.GNU-stack") {
      if ((sec.sh_flags & SHF_EXECINSTR) && !ctx.arg.relocatable &&
          ctx.arg.zGnustack != GnuStackKind::Exec) {
        Err(ctx) << this
                 << ": requires an executable stack, but -z execstack is not "
                    "specified";
      }
      return &InputSection::discarded;
    }

    // Object files that use processor features such as Intel Control-Flow
    // Enforcement (CET), AArch64 Branch Target Identification BTI or RISC-V
    // Zicfilp/Zicfiss extensions, use a .note.gnu.property section containing
    // a bitfield of feature bits like the GNU_PROPERTY_X86_FEATURE_1_IBT flag.
    //
    // Since we merge bitmaps from multiple object files to create a new
    // .note.gnu.property containing a single AND'ed bitmap, we discard an input
    // file's .note.gnu.property section.
    if (name == ".note.gnu.property") {
      readGnuProperty<ELFT>(ctx, InputSection(*this, sec, name), *this);
      return &InputSection::discarded;
    }

    // Split stacks is a feature to support a discontiguous stack,
    // commonly used in the programming language Go. For the details,
    // see https://gcc.gnu.org/wiki/SplitStacks. An object file compiled
    // for split stack will include a .note.GNU-split-stack section.
    if (name == ".note.GNU-split-stack") {
      if (ctx.arg.relocatable) {
        ErrAlways(ctx) << "cannot mix split-stack and non-split-stack in a "
                          "relocatable link";
        return &InputSection::discarded;
      }
      this->splitStack = true;
      return &InputSection::discarded;
    }

    // An object file compiled for split stack, but where some of the
    // functions were compiled with the no_split_stack_attribute will
    // include a .note.GNU-no-split-stack section.
    if (name == ".note.GNU-no-split-stack") {
      this->someNoSplitStack = true;
      return &InputSection::discarded;
    }

    // Strip existing .note.gnu.build-id sections so that the output won't have
    // more than one build-id. This is not usually a problem because input
    // object files normally don't have .build-id sections, but you can create
    // such files by "ld.{bfd,gold,lld} -r --build-id", and we want to guard
    // against it.
    if (name == ".note.gnu.build-id")
      return &InputSection::discarded;
  }

  // The linker merges EH (exception handling) frames and creates a
  // .eh_frame_hdr section for runtime. So we handle them with a special
  // class. For relocatable outputs, they are just passed through.
  if (name == ".eh_frame" && !ctx.arg.relocatable)
    return makeThreadLocal<EhInputSection>(*this, sec, name);

  if ((sec.sh_flags & SHF_MERGE) && shouldMerge(sec, name))
    return makeThreadLocal<MergeInputSection>(*this, sec, name);
  return makeThreadLocal<InputSection>(*this, sec, name);
}

// Resolve a global symbol: issue the resolve() call for its definition,
// COMMON, or undefined reference, with the side effects of serial symbol
// resolution. Shared by initializeSymbols and the parallel parse pipeline.
template <class ELFT>
static void resolveSymbol(Ctx &ctx, ObjFile<ELFT> *f,
                          const typename ELFT::Sym &eSym, Symbol &sym) {
  if (eSym.st_shndx == SHN_UNDEF) {
    sym.resolve(ctx, Undefined{f, StringRef(), eSym.getBinding(), eSym.st_other,
                               eSym.getType()});
    sym.isUsedInRegularObj = true;
    sym.referenced = true;
    return;
  }
  sym.isUsedInRegularObj = true;
  if (LLVM_UNLIKELY(eSym.st_shndx == SHN_COMMON)) {
    uint64_t value = eSym.st_value;
    if (value == 0 || value >= UINT32_MAX)
      Err(ctx) << f << ": common symbol '" << sym.getName()
               << "' has invalid alignment: " << value;
    sym.resolve(ctx, CommonSymbol{ctx, f, StringRef(), eSym.getBinding(),
                                  eSym.st_other, eSym.getType(), value,
                                  eSym.st_size});
    return;
  }
  // Defined::section will be set in postParse.
  sym.resolve(ctx,
              Defined{ctx, f, StringRef(), eSym.getBinding(), eSym.st_other,
                      eSym.getType(), eSym.st_value, eSym.st_size, nullptr});
}

// Initialize symbols. symbols is a parallel array to the corresponding ELF
// symbol table.
template <class ELFT>
void ObjFile<ELFT>::initializeSymbols(const object::ELFFile<ELFT> &obj) {
  ArrayRef<Elf_Sym> eSyms = this->getELFSyms<ELFT>();
  allocateSymbols();

  // Some entries have been filled by LazyObjFile.
  auto *symtab = ctx.symtab.get();
  for (size_t i = firstGlobal, end = eSyms.size(); i != end; ++i)
    if (!symbols[i])
      symbols[i] = symtab->insert(CHECK2(eSyms[i].getName(stringTable), this));

  // Perform symbol resolution on non-local symbols.
  SmallVector<unsigned, 32> undefineds;
  for (size_t i = firstGlobal, end = eSyms.size(); i != end; ++i) {
    const Elf_Sym &eSym = eSyms[i];
    if (eSym.st_shndx == SHN_UNDEF) {
      undefineds.push_back(i);
      continue;
    }
    if (LLVM_UNLIKELY(eSym.st_shndx == SHN_COMMON))
      hasCommonSyms = true;
    resolveSymbol(ctx, this, eSym, *symbols[i]);
  }

  // Undefined symbols (excluding those defined relative to non-prevailing
  // sections) can trigger recursive extract. Process defined symbols first so
  // that the relative order between a defined symbol and an undefined symbol
  // does not change the symbol resolution behavior. In addition, a set of
  // interconnected symbols will all be resolved to the same file, instead of
  // being resolved to different files.
  for (unsigned i : undefineds)
    resolveSymbol(ctx, this, eSyms[i], *symbols[i]);
}

template <class ELFT>
void ObjFile<ELFT>::initSectionsAndLocalSyms(bool ignoreComdats) {
  if (!justSymbols)
    initializeSections(ignoreComdats, getObj());
  else
    initializeJustSymbols();

  if (!firstGlobal)
    return;
  SymbolUnion *locals = makeThreadLocalN<SymbolUnion>(firstGlobal);

  ArrayRef<Elf_Sym> eSyms = this->getELFSyms<ELFT>();
  for (size_t i = 0, end = firstGlobal; i != end; ++i) {
    const Elf_Sym &eSym = eSyms[i];
    uint32_t secIdx = eSym.st_shndx;
    if (LLVM_UNLIKELY(secIdx == SHN_XINDEX))
      secIdx = check(getExtendedSymbolTableIndex<ELFT>(eSym, i, shndxTable));
    else if (secIdx >= SHN_LORESERVE)
      secIdx = 0;
    if (LLVM_UNLIKELY(secIdx >= sections.size())) {
      Err(ctx) << this << ": invalid section index: " << secIdx;
      secIdx = 0;
    }
    if (LLVM_UNLIKELY(eSym.getBinding() != STB_LOCAL))
      ErrAlways(ctx) << this << ": non-local symbol (" << i
                     << ") found at index < .symtab's sh_info (" << end << ")";

    InputSectionBase *sec = sections[secIdx];
    uint8_t type = eSym.getType();
    if (type == STT_FILE)
      sourceFile = CHECK2(eSym.getName(stringTable), this);
    unsigned stName = eSym.st_name;
    if (LLVM_UNLIKELY(stringTable.size() <= stName)) {
      Err(ctx) << this << ": invalid symbol name offset";
      stName = 0;
    }
    StringRef name(stringTable.data() + stName);

    symbols[i] = reinterpret_cast<Symbol *>(locals + i);
    if (eSym.st_shndx == SHN_UNDEF || sec == &InputSection::discarded)
      new (symbols[i]) Undefined(this, name, STB_LOCAL, eSym.st_other, type,
                                 /*discardedSecIdx=*/secIdx);
    else
      new (symbols[i]) Defined(ctx, this, name, STB_LOCAL, eSym.st_other, type,
                               eSym.st_value, eSym.st_size, sec);
    symbols[i]->isUsedInRegularObj = true;
  }
}

// Called after all ObjFile::parse is called for all ObjFiles. This checks
// duplicate symbols and may do symbol property merge in the future.
template <class ELFT> void ObjFile<ELFT>::postParse() {
  static std::mutex mu;
  ArrayRef<Elf_Sym> eSyms = this->getELFSyms<ELFT>();
  for (size_t i = firstGlobal, end = eSyms.size(); i != end; ++i) {
    const Elf_Sym &eSym = eSyms[i];
    Symbol &sym = *symbols[i];
    uint32_t secIdx = eSym.st_shndx;
    uint8_t binding = eSym.getBinding();
    if (LLVM_UNLIKELY(binding != STB_GLOBAL && binding != STB_WEAK &&
                      binding != STB_GNU_UNIQUE))
      Err(ctx) << this << ": symbol (" << i
               << ") has invalid binding: " << (int)binding;

    // st_value of STT_TLS represents the assigned offset, not the actual
    // address which is used by STT_FUNC and STT_OBJECT. STT_TLS symbols can
    // only be referenced by special TLS relocations. It is usually an error if
    // a STT_TLS symbol is replaced by a non-STT_TLS symbol, vice versa.
    if (LLVM_UNLIKELY(sym.isTls()) && eSym.getType() != STT_TLS &&
        eSym.getType() != STT_NOTYPE)
      Err(ctx) << "TLS attribute mismatch: " << &sym << "\n>>> in " << sym.file
               << "\n>>> in " << this;

    // Handle non-COMMON defined symbol below. !sym.file allows a symbol
    // assignment to redefine a symbol without an error.
    if (!sym.isDefined() || secIdx == SHN_UNDEF)
      continue;
    if (LLVM_UNLIKELY(secIdx >= SHN_LORESERVE)) {
      if (secIdx == SHN_COMMON)
        continue;
      if (secIdx == SHN_XINDEX)
        secIdx = check(getExtendedSymbolTableIndex<ELFT>(eSym, i, shndxTable));
      else
        secIdx = 0;
    }

    if (LLVM_UNLIKELY(secIdx >= sections.size())) {
      Err(ctx) << this << ": invalid section index: " << secIdx;
      continue;
    }
    InputSectionBase *sec = sections[secIdx];
    if (sec == &InputSection::discarded) {
      if (sym.traced) {
        printTraceSymbol(Undefined{this, sym.getName(), sym.binding,
                                   sym.stOther, sym.type, secIdx},
                         sym.getName());
      }
      if (sym.file == this) {
        std::lock_guard<std::mutex> lock(mu);
        ctx.nonPrevailingSyms.emplace_back(&sym, secIdx);
      }
      continue;
    }

    if (sym.file == this) {
      cast<Defined>(sym).section = sec;
      continue;
    }

    if (sym.binding == STB_WEAK || binding == STB_WEAK)
      continue;
    std::lock_guard<std::mutex> lock(mu);
    ctx.duplicates.push_back({&sym, this, sec, eSym.st_value});
  }
}

// The handling of tentative definitions (COMMON symbols) in archives is murky.
// A tentative definition will be promoted to a global definition if there are
// no non-tentative definitions to dominate it. When we hold a tentative
// definition to a symbol and are inspecting archive members for inclusion
// there are 2 ways we can proceed:
//
// 1) Consider the tentative definition a 'real' definition (ie promotion from
//    tentative to real definition has already happened) and not inspect
//    archive members for Global/Weak definitions to replace the tentative
//    definition. An archive member would only be included if it satisfies some
//    other undefined symbol. This is the behavior Gold uses.
//
// 2) Consider the tentative definition as still undefined (ie the promotion to
//    a real definition happens only after all symbol resolution is done).
//    The linker searches archive members for STB_GLOBAL definitions to
//    replace the tentative definition with. This is the behavior used by
//    GNU ld.
//
//  The second behavior is inherited from SysVR4, which based it on the FORTRAN
//  COMMON BLOCK model. This behavior is needed for proper initialization in old
//  (pre F90) FORTRAN code that is packaged into an archive.
//
//  The following functions search archive members for definitions to replace
//  tentative definitions (implementing behavior 2).
static bool isBitcodeNonCommonDef(MemoryBufferRef mb, StringRef symName,
                                  StringRef archiveName) {
  IRSymtabFile symtabFile = check(readIRSymtab(mb));
  for (const irsymtab::Reader::SymbolRef &sym :
       symtabFile.TheReader.symbols()) {
    if (sym.isGlobal() && sym.getName() == symName)
      return !sym.isUndefined() && !sym.isWeak() && !sym.isCommon();
  }
  return false;
}

template <class ELFT>
static bool isNonCommonDef(Ctx &ctx, ELFKind ekind, MemoryBufferRef mb,
                           StringRef symName, StringRef archiveName) {
  ObjFile<ELFT> *obj = make<ObjFile<ELFT>>(ctx, ekind, mb, archiveName);
  obj->init();
  StringRef stringtable = obj->getStringTable();

  for (auto sym : obj->template getGlobalELFSyms<ELFT>()) {
    Expected<StringRef> name = sym.getName(stringtable);
    if (name && name.get() == symName)
      return sym.isDefined() && sym.getBinding() == STB_GLOBAL &&
             !sym.isCommon();
  }
  return false;
}

static bool isNonCommonDef(Ctx &ctx, MemoryBufferRef mb, StringRef symName,
                           StringRef archiveName) {
  switch (getELFKind(ctx, mb, archiveName)) {
  case ELF32LEKind:
    return isNonCommonDef<ELF32LE>(ctx, ELF32LEKind, mb, symName, archiveName);
  case ELF32BEKind:
    return isNonCommonDef<ELF32BE>(ctx, ELF32BEKind, mb, symName, archiveName);
  case ELF64LEKind:
    return isNonCommonDef<ELF64LE>(ctx, ELF64LEKind, mb, symName, archiveName);
  case ELF64BEKind:
    return isNonCommonDef<ELF64BE>(ctx, ELF64BEKind, mb, symName, archiveName);
  default:
    llvm_unreachable("getELFKind");
  }
}

SharedFile::SharedFile(Ctx &ctx, MemoryBufferRef m, StringRef defaultSoName)
    : ELFFileBase(ctx, SharedKind, getELFKind(ctx, m, ""), m),
      soName(defaultSoName), isNeeded(!ctx.arg.asNeeded) {}

// Parse the version definitions in the object file if present, and return a
// vector whose nth element contains a pointer to the Elf_Verdef for version
// identifier n. Version identifiers that are not definitions map to nullptr.
template <typename ELFT>
static SmallVector<const void *, 0>
parseVerdefs(const uint8_t *base, const typename ELFT::Shdr *sec) {
  if (!sec)
    return {};

  // Build the Verdefs array by following the chain of Elf_Verdef objects
  // from the start of the .gnu.version_d section.
  SmallVector<const void *, 0> verdefs;
  const uint8_t *verdef = base + sec->sh_offset;
  for (unsigned i = 0, e = sec->sh_info; i != e; ++i) {
    auto *curVerdef = reinterpret_cast<const typename ELFT::Verdef *>(verdef);
    verdef += curVerdef->vd_next;
    unsigned verdefIndex = curVerdef->vd_ndx;
    if (verdefIndex >= verdefs.size())
      verdefs.resize(verdefIndex + 1);
    verdefs[verdefIndex] = curVerdef;
  }
  return verdefs;
}

// Whether a dynsym entry is registered under its unversioned name: an
// unversioned or default-versioned undefined symbol, or a plain or
// default-versioned definition with an in-bounds version index. Versioned
// undefined symbols and hidden (VERSYM_HIDDEN) or VER_NDX_LOCAL definitions
// are registered under name@version instead, if at all. Shared by
// SharedFile::parse and the pipeline's readShared.
//
// Index 0 (VER_NDX_LOCAL) is used for unversioned undefined symbols. GNU ld
// versions between 2.35 and 2.45 also generate VER_NDX_GLOBAL for this case
// (https://sourceware.org/PR33577).
static bool addsUnversionedName(bool isUndef, uint16_t ver,
                                size_t verdefCount) {
  uint16_t idx = ver & ~VERSYM_HIDDEN;
  if (isUndef)
    return ver == VER_NDX_LOCAL || ver == VER_NDX_GLOBAL;
  return ver == idx && ver != VER_NDX_LOCAL &&
         (ver == VER_NDX_GLOBAL || idx < verdefCount);
}

// Parse SHT_GNU_verneed to properly set the name of a versioned undefined
// symbol. We detect fatal issues which would cause vulnerabilities, but do not
// implement sophisticated error checking like in llvm-readobj because the value
// of such diagnostics is low.
template <typename ELFT>
std::vector<uint32_t> SharedFile::parseVerneed(const ELFFile<ELFT> &obj,
                                               const typename ELFT::Shdr *sec) {
  if (!sec)
    return {};
  std::vector<uint32_t> verneeds;
  ArrayRef<uint8_t> data = CHECK2(obj.getSectionContents(*sec), this);
  const uint8_t *verneedBuf = data.begin();
  for (unsigned i = 0; i != sec->sh_info; ++i) {
    if (verneedBuf + sizeof(typename ELFT::Verneed) > data.end()) {
      Err(ctx) << this << " has an invalid Verneed";
      break;
    }
    auto *vn = reinterpret_cast<const typename ELFT::Verneed *>(verneedBuf);
    const uint8_t *vernauxBuf = verneedBuf + vn->vn_aux;
    for (unsigned j = 0; j != vn->vn_cnt; ++j) {
      if (vernauxBuf + sizeof(typename ELFT::Vernaux) > data.end()) {
        Err(ctx) << this << " has an invalid Vernaux";
        break;
      }
      auto *aux = reinterpret_cast<const typename ELFT::Vernaux *>(vernauxBuf);
      if (aux->vna_name >= this->stringTable.size()) {
        Err(ctx) << this << " has a Vernaux with an invalid vna_name";
        break;
      }
      uint16_t version = aux->vna_other & VERSYM_VERSION;
      if (version >= verneeds.size())
        verneeds.resize(version + 1);
      verneeds[version] = aux->vna_name;
      vernauxBuf += aux->vna_next;
    }
    verneedBuf += vn->vn_next;
  }
  return verneeds;
}

// Parse PT_GNU_PROPERTY segments in DSO. The process is similar to
// readGnuProperty, but we don't have the InputSection information.
template <typename ELFT>
void SharedFile::parseGnuAndFeatures(const ELFFile<ELFT> &obj) {
  if (ctx.arg.emachine != EM_AARCH64)
    return;
  const uint8_t *base = obj.base();
  auto phdrs = CHECK2(obj.program_headers(), this);
  for (auto phdr : phdrs) {
    if (phdr.p_type != PT_GNU_PROPERTY)
      continue;
    typename ELFT::Note note(
        *reinterpret_cast<const typename ELFT::Nhdr *>(base + phdr.p_offset));
    if (note.getType() != NT_GNU_PROPERTY_TYPE_0 || note.getName() != "GNU")
      continue;

    ArrayRef<uint8_t> desc = note.getDesc(phdr.p_align);
    parseGnuPropertyNote<ELFT>(ctx, *this, GNU_PROPERTY_AARCH64_FEATURE_1_AND,
                               desc, base);
  }
}

// We do not usually care about alignments of data in shared object
// files because the loader takes care of it. However, if we promote a
// DSO symbol to point to .bss due to copy relocation, we need to keep
// the original alignment requirements. We infer it in this function.
template <typename ELFT>
static uint64_t getAlignment(ArrayRef<typename ELFT::Shdr> sections,
                             const typename ELFT::Sym &sym) {
  uint64_t ret = UINT64_MAX;
  if (sym.st_value)
    ret = 1ULL << llvm::countr_zero((uint64_t)sym.st_value);
  if (0 < sym.st_shndx && sym.st_shndx < sections.size())
    ret = std::min<uint64_t>(ret, sections[sym.st_shndx].sh_addralign);
  return (ret > UINT32_MAX) ? 0 : ret;
}

// Fully parse the shared object file.
//
// This function parses symbol versions. If a DSO has version information,
// the file has a ".gnu.version_d" section which contains symbol version
// definitions. Each symbol is associated to one version through a table in
// ".gnu.version" section. That table is a parallel array for the symbol
// table, and each table entry contains an index in ".gnu.version_d".
//
// The special index 0 is reserved for VERF_NDX_LOCAL and 1 is for
// VER_NDX_GLOBAL. There's no table entry for these special versions in
// ".gnu.version_d".
//
// The file format for symbol versioning is perhaps a bit more complicated
// than necessary, but you can easily understand the code if you wrap your
// head around the data structure described above.
template <class ELFT> void SharedFile::parse() {
  using Elf_Dyn = typename ELFT::Dyn;
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;
  using Elf_Verdef = typename ELFT::Verdef;
  using Elf_Versym = typename ELFT::Versym;

  ArrayRef<Elf_Dyn> dynamicTags;
  const ELFFile<ELFT> obj = this->getObj<ELFT>();
  ArrayRef<Elf_Shdr> sections = getELFShdrs<ELFT>();

  const Elf_Shdr *versymSec = nullptr;
  const Elf_Shdr *verdefSec = nullptr;
  const Elf_Shdr *verneedSec = nullptr;
  symbols = std::make_unique<Symbol *[]>(numSymbols);

  // Search for .dynsym, .dynamic, .symtab, .gnu.version and .gnu.version_d.
  for (const Elf_Shdr &sec : sections) {
    switch (sec.sh_type) {
    default:
      continue;
    case SHT_DYNAMIC:
      dynamicTags =
          CHECK2(obj.template getSectionContentsAsArray<Elf_Dyn>(sec), this);
      break;
    case SHT_GNU_versym:
      versymSec = &sec;
      break;
    case SHT_GNU_verdef:
      verdefSec = &sec;
      break;
    case SHT_GNU_verneed:
      verneedSec = &sec;
      break;
    }
  }

  if (versymSec && numSymbols == 0) {
    ErrAlways(ctx) << "SHT_GNU_versym should be associated with symbol table";
    return;
  }

  // Search for a DT_SONAME tag to initialize this->soName.
  for (const Elf_Dyn &dyn : dynamicTags) {
    if (dyn.d_tag == DT_NEEDED) {
      uint64_t val = dyn.getVal();
      if (val >= this->stringTable.size()) {
        Err(ctx) << this << ": invalid DT_NEEDED entry";
        return;
      }
      dtNeeded.push_back(this->stringTable.data() + val);
    } else if (dyn.d_tag == DT_SONAME) {
      uint64_t val = dyn.getVal();
      if (val >= this->stringTable.size()) {
        Err(ctx) << this << ": invalid DT_SONAME entry";
        return;
      }
      soName = this->stringTable.data() + val;
    }
  }

  // DSOs are uniquified not by filename but by soname.
  StringSaver &ss = ctx.saver;
  DenseMap<CachedHashStringRef, SharedFile *>::iterator it;
  bool wasInserted;
  std::tie(it, wasInserted) =
      ctx.symtab->soNames.try_emplace(CachedHashStringRef(soName), this);

  // If a DSO appears more than once on the command line with and without
  // --as-needed, --no-as-needed takes precedence over --as-needed because a
  // user can add an extra DSO with --no-as-needed to force it to be added to
  // the dependency list.
  if (isNeeded)
    it->second->isNeeded.store(true, std::memory_order_relaxed);
  if (!wasInserted)
    return;

  ctx.sharedFiles.push_back(this);

  verdefs = parseVerdefs<ELFT>(obj.base(), verdefSec);
  std::vector<uint32_t> verneeds = parseVerneed<ELFT>(obj, verneedSec);
  parseGnuAndFeatures<ELFT>(obj);

  // Parse ".gnu.version" section which is a parallel array for the symbol
  // table. If a given file doesn't have a ".gnu.version" section, we use
  // VER_NDX_GLOBAL.
  size_t size = numSymbols - firstGlobal;
  std::vector<uint16_t> versyms(size, VER_NDX_GLOBAL);
  if (versymSec) {
    ArrayRef<Elf_Versym> versym =
        CHECK2(obj.template getSectionContentsAsArray<Elf_Versym>(*versymSec),
               this)
            .slice(firstGlobal);
    for (size_t i = 0; i < size; ++i)
      versyms[i] = versym[i].vs_index;
  }

  // System libraries can have a lot of symbols with versions. Using a
  // fixed buffer for computing the versions name (foo@ver) can save a
  // lot of allocations.
  SmallString<0> versionedNameBuffer;

  // Add symbols to the symbol table.
  ArrayRef<Elf_Sym> syms = this->getGlobalELFSyms<ELFT>();
  for (size_t i = 0, e = syms.size(); i != e; ++i) {
    const Elf_Sym &sym = syms[i];

    // ELF spec requires that all local symbols precede weak or global
    // symbols in each symbol table, and the index of first non-local symbol
    // is stored to sh_info. If a local symbol appears after some non-local
    // symbol, that's a violation of the spec.
    StringRef name = CHECK2(sym.getName(stringTable), this);
    if (sym.getBinding() == STB_LOCAL) {
      Err(ctx) << this << ": invalid local symbol '" << name
               << "' in global part of symbol table";
      continue;
    }

    const uint16_t ver = versyms[i], idx = ver & ~VERSYM_HIDDEN;
    if (sym.isUndefined()) {
      if (!addsUnversionedName(true, ver, verdefs.size())) {
        if (idx >= verneeds.size()) {
          ErrAlways(ctx) << "corrupt input file: version need index " << idx
                         << " for symbol " << name
                         << " is out of bounds\n>>> defined in " << this;
          continue;
        }
        StringRef verName = stringTable.data() + verneeds[idx];
        versionedNameBuffer.clear();
        name = ss.save((name + "@" + verName).toStringRef(versionedNameBuffer));
      }
      Symbol *s = ctx.symtab->addSymbol(
          Undefined{this, name, sym.getBinding(), sym.st_other, sym.getType()});
      s->isExported = true;
      if (sym.getBinding() != STB_WEAK &&
          ctx.arg.unresolvedSymbolsInShlib != UnresolvedPolicy::Ignore)
        requiredSymbols.push_back(s);
      continue;
    }

    if (ver == VER_NDX_LOCAL ||
        (ver != VER_NDX_GLOBAL && idx >= verdefs.size())) {
      // In GNU ld < 2.31 (before 3be08ea4728b56d35e136af4e6fd3086ade17764), the
      // MIPS port puts _gp_disp symbol into DSO files and incorrectly assigns
      // VER_NDX_LOCAL. Workaround this bug.
      if (ctx.arg.emachine == EM_MIPS && name == "_gp_disp")
        continue;
      ErrAlways(ctx) << "corrupt input file: version definition index " << idx
                     << " for symbol " << name
                     << " is out of bounds\n>>> defined in " << this;
      continue;
    }

    uint32_t alignment = getAlignment<ELFT>(sections, sym);
    if (addsUnversionedName(false, ver, verdefs.size())) {
      auto *s = ctx.symtab->addSymbol(
          SharedSymbol{*this, name, sym.getBinding(), sym.st_other,
                       sym.getType(), sym.st_value, sym.st_size, alignment});
      s->dsoDefined = true;
      if (s->file == this)
        s->versionId = ver;
    }

    // Also add the symbol with the versioned name to handle undefined symbols
    // with explicit versions.
    if (ver == VER_NDX_GLOBAL)
      continue;

    StringRef verName =
        stringTable.data() +
        reinterpret_cast<const Elf_Verdef *>(verdefs[idx])->getAux()->vda_name;
    versionedNameBuffer.clear();
    name = (name + "@" + verName).toStringRef(versionedNameBuffer);
    auto *s = ctx.symtab->addSymbol(
        SharedSymbol{*this, ss.save(name), sym.getBinding(), sym.st_other,
                     sym.getType(), sym.st_value, sym.st_size, alignment});
    s->dsoDefined = true;
    if (s->file == this)
      s->versionId = idx;
  }
}

static ELFKind getBitcodeELFKind(const Triple &t) {
  if (t.isLittleEndian())
    return t.isArch64Bit() ? ELF64LEKind : ELF32LEKind;
  return t.isArch64Bit() ? ELF64BEKind : ELF32BEKind;
}

static uint16_t getBitcodeMachineKind(Ctx &ctx, StringRef path,
                                      const Triple &t) {
  switch (t.getArch()) {
  case Triple::aarch64:
  case Triple::aarch64_be:
    return EM_AARCH64;
  case Triple::amdgcn:
  case Triple::r600:
    return EM_AMDGPU;
  case Triple::arm:
  case Triple::armeb:
  case Triple::thumb:
  case Triple::thumbeb:
    return EM_ARM;
  case Triple::avr:
    return EM_AVR;
  case Triple::hexagon:
    return EM_HEXAGON;
  case Triple::loongarch32:
  case Triple::loongarch64:
    return EM_LOONGARCH;
  case Triple::mips:
  case Triple::mipsel:
  case Triple::mips64:
  case Triple::mips64el:
    return EM_MIPS;
  case Triple::msp430:
    return EM_MSP430;
  case Triple::ppc:
  case Triple::ppcle:
    return EM_PPC;
  case Triple::ppc64:
  case Triple::ppc64le:
    return EM_PPC64;
  case Triple::riscv32:
  case Triple::riscv64:
    return EM_RISCV;
  case Triple::sparcv9:
    return EM_SPARCV9;
  case Triple::systemz:
    return EM_S390;
  case Triple::x86:
    return t.isOSIAMCU() ? EM_IAMCU : EM_386;
  case Triple::x86_64:
    return EM_X86_64;
  default:
    ErrAlways(ctx) << path
                   << ": could not infer e_machine from bitcode target triple "
                   << t.str();
    return EM_NONE;
  }
}

static uint8_t getOsAbi(const Triple &t) {
  switch (t.getOS()) {
  case Triple::AMDHSA:
    return ELF::ELFOSABI_AMDGPU_HSA;
  case Triple::AMDPAL:
    return ELF::ELFOSABI_AMDGPU_PAL;
  case Triple::Mesa3D:
    return ELF::ELFOSABI_AMDGPU_MESA3D;
  default:
    return ELF::ELFOSABI_NONE;
  }
}

BitcodeFile::BitcodeFile(Ctx &ctx, MemoryBufferRef mb, StringRef archiveName,
                         uint64_t offsetInArchive, bool lazy)
    : InputFile(ctx, BitcodeKind, mb) {
  this->archiveName = archiveName;
  this->lazy = lazy;

  std::string path = mb.getBufferIdentifier().str();
  if (ctx.arg.thinLTOIndexOnly)
    path = replaceThinLTOSuffix(ctx, mb.getBufferIdentifier());

  // ThinLTO assumes that all MemoryBufferRefs given to it have a unique
  // name. If two archives define two members with the same name, this
  // causes a collision which result in only one of the objects being taken
  // into consideration at LTO time (which very likely causes undefined
  // symbols later in the link stage). So we append file offset to make
  // filename unique.
  StringSaver &ss = ctx.saver;
  StringRef name = archiveName.empty()
                       ? ss.save(path)
                       : ss.save(archiveName + "(" + path::filename(path) +
                                 " at " + utostr(offsetInArchive) + ")");

  MemoryBufferRef mbref(mb.getBuffer(), name);

  obj = CHECK2(lto::InputFile::create(mbref), this);
  obj->setArchivePathAndName(archiveName, mb.getBufferIdentifier());

  Triple t(obj->getTargetTriple());
  ekind = getBitcodeELFKind(t);
  emachine = getBitcodeMachineKind(ctx, mb.getBufferIdentifier(), t);
  osabi = getOsAbi(t);
}

static uint8_t mapVisibility(GlobalValue::VisibilityTypes gvVisibility) {
  switch (gvVisibility) {
  case GlobalValue::DefaultVisibility:
    return STV_DEFAULT;
  case GlobalValue::HiddenVisibility:
    return STV_HIDDEN;
  case GlobalValue::ProtectedVisibility:
    return STV_PROTECTED;
  }
  llvm_unreachable("unknown visibility");
}

static void createBitcodeSymbol(Ctx &ctx, Symbol *&sym,
                                const lto::InputFile::Symbol &objSym,
                                BitcodeFile &f) {
  uint8_t binding = objSym.isWeak() ? STB_WEAK : STB_GLOBAL;
  uint8_t type = objSym.isTLS() ? STT_TLS : STT_NOTYPE;
  uint8_t visibility = mapVisibility(objSym.getVisibility());

  if (!sym) {
    // Symbols can be duplicated in bitcode files because of '#include' and
    // linkonce_odr. Use uniqueSaver to save symbol names for de-duplication.
    // Update objSym.Name to reference (via StringRef) the string saver's copy;
    // this way LTO can reference the same string saver's copy rather than
    // keeping copies of its own.
    objSym.Name = ctx.uniqueSaver.save(objSym.getName());
    sym = ctx.symtab->insert(objSym.getName());
  }

  if (objSym.isUndefined()) {
    Undefined newSym(&f, StringRef(), binding, visibility, type);
    sym->resolve(ctx, newSym);
    sym->referenced = true;
    return;
  }

  if (objSym.isCommon()) {
    sym->resolve(ctx, CommonSymbol{ctx, &f, StringRef(), binding, visibility,
                                   STT_OBJECT, objSym.getCommonAlignment(),
                                   objSym.getCommonSize()});
  } else {
    Defined newSym(ctx, &f, StringRef(), binding, visibility, type, 0, 0,
                   nullptr);
    // The definition can be omitted if all bitcode definitions satisfy
    // `canBeOmittedFromSymbolTable()` and isUsedInRegularObj is false.
    // The latter condition is tested in parseVersionAndComputeIsPreemptible.
    sym->ltoCanOmit = objSym.canBeOmittedFromSymbolTable() &&
                      (!sym->isDefined() || sym->ltoCanOmit);
    sym->resolve(ctx, newSym);
  }
}

void BitcodeFile::parse() {
  // addComdatGroup is owner-idempotent: the parallel parse pipeline may have
  // already registered this file as the owner.
  for (std::pair<StringRef, Comdat::SelectionKind> s : obj->getComdatTable()) {
    keptComdats.push_back(
        s.second == Comdat::NoDeduplicate ||
        ctx.symtab->addComdatGroup(CachedHashStringRef(s.first), this) == this);
  }

  if (numSymbols == 0) {
    numSymbols = obj->symbols().size();
    symbols = std::make_unique<Symbol *[]>(numSymbols);
  }
  // Process defined symbols first. See the comment in
  // ObjFile<ELFT>::initializeSymbols.
  for (auto [i, irSym] : llvm::enumerate(obj->symbols()))
    if (!irSym.isUndefined())
      createBitcodeSymbol(ctx, symbols[i], irSym, *this);
  for (auto [i, irSym] : llvm::enumerate(obj->symbols()))
    if (irSym.isUndefined())
      createBitcodeSymbol(ctx, symbols[i], irSym, *this);

  for (auto l : obj->getDependentLibraries())
    addDependentLibrary(ctx, l, this);
}

void BitcodeFile::parseLazy() {
  numSymbols = obj->symbols().size();
  symbols = std::make_unique<Symbol *[]>(numSymbols);
  for (auto [i, irSym] : llvm::enumerate(obj->symbols())) {
    // Symbols can be duplicated in bitcode files because of '#include' and
    // linkonce_odr. Use uniqueSaver to save symbol names for de-duplication.
    // Update objSym.Name to reference (via StringRef) the string saver's copy;
    // this way LTO can reference the same string saver's copy rather than
    // keeping copies of its own.
    irSym.Name = ctx.uniqueSaver.save(irSym.getName());
    if (!irSym.isUndefined()) {
      auto *sym = ctx.symtab->insert(irSym.getName());
      sym->resolve(ctx, LazySymbol{*this});
      symbols[i] = sym;
    }
  }
}

void BitcodeFile::postParse() {
  for (auto [i, irSym] : llvm::enumerate(obj->symbols())) {
    const Symbol &sym = *symbols[i];
    if (sym.file == this || !sym.isDefined() || irSym.isUndefined() ||
        irSym.isCommon() || irSym.isWeak())
      continue;
    int c = irSym.getComdatIndex();
    if (c != -1 && !keptComdats[c])
      continue;
    reportDuplicate(ctx, sym, this, nullptr, 0);
  }
}

void BinaryFile::parse() {
  ArrayRef<uint8_t> data = arrayRefFromStringRef(mb.getBuffer());
  auto *section =
      make<InputSection>(this, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
                         /*addralign=*/8, /*entsize=*/0, data);
  sections.push_back(section);

  // For each input file foo that is embedded to a result as a binary
  // blob, we define _binary_foo_{start,end,size} symbols, so that
  // user programs can access blobs by name. Non-alphanumeric
  // characters in a filename are replaced with underscore.
  std::string s = "_binary_" + mb.getBufferIdentifier().str();
  for (char &c : s)
    if (!isAlnum(c))
      c = '_';

  llvm::StringSaver &ss = ctx.saver;
  ctx.symtab->addAndCheckDuplicate(
      ctx, Defined{ctx, this, ss.save(s + "_start"), STB_GLOBAL, STV_DEFAULT,
                   STT_OBJECT, 0, 0, section});
  ctx.symtab->addAndCheckDuplicate(
      ctx, Defined{ctx, this, ss.save(s + "_end"), STB_GLOBAL, STV_DEFAULT,
                   STT_OBJECT, data.size(), 0, section});
  ctx.symtab->addAndCheckDuplicate(
      ctx, Defined{ctx, this, ss.save(s + "_size"), STB_GLOBAL, STV_DEFAULT,
                   STT_OBJECT, data.size(), 0, nullptr});
}

InputFile *elf::createInternalFile(Ctx &ctx, StringRef name) {
  auto *file =
      make<InputFile>(ctx, InputFile::InternalKind, MemoryBufferRef("", name));
  // References from an internal file do not lead to --warn-backrefs
  // diagnostics.
  file->groupId = 0;
  return file;
}

std::unique_ptr<ELFFileBase> elf::createObjFile(Ctx &ctx, MemoryBufferRef mb,
                                                StringRef archiveName,
                                                bool lazy) {
  std::unique_ptr<ELFFileBase> f;
  switch (getELFKind(ctx, mb, archiveName)) {
  case ELF32LEKind:
    f = std::make_unique<ObjFile<ELF32LE>>(ctx, ELF32LEKind, mb, archiveName);
    break;
  case ELF32BEKind:
    f = std::make_unique<ObjFile<ELF32BE>>(ctx, ELF32BEKind, mb, archiveName);
    break;
  case ELF64LEKind:
    f = std::make_unique<ObjFile<ELF64LE>>(ctx, ELF64LEKind, mb, archiveName);
    break;
  case ELF64BEKind:
    f = std::make_unique<ObjFile<ELF64BE>>(ctx, ELF64BEKind, mb, archiveName);
    break;
  default:
    llvm_unreachable("getELFKind");
  }
  f->init();
  f->lazy = lazy;
  return f;
}

template <class ELFT> void ObjFile<ELFT>::parseLazy() {
  const ArrayRef<typename ELFT::Sym> eSyms = this->getELFSyms<ELFT>();
  numSymbols = eSyms.size();
  symbols = std::make_unique<Symbol *[]>(numSymbols);

  // resolve() may trigger this->extract() if an existing symbol is an undefined
  // symbol. If that happens, this function has served its purpose, and we can
  // exit from the loop early.
  auto *symtab = ctx.symtab.get();
  for (size_t i = firstGlobal, end = eSyms.size(); i != end; ++i) {
    if (eSyms[i].st_shndx == SHN_UNDEF)
      continue;
    symbols[i] = symtab->insert(CHECK2(eSyms[i].getName(stringTable), this));
    symbols[i]->resolve(ctx, LazySymbol{*this});
    if (!lazy)
      break;
  }
}

bool InputFile::shouldExtractForCommon(StringRef name) const {
  if (isa<BitcodeFile>(this))
    return isBitcodeNonCommonDef(mb, name, archiveName);

  return isNonCommonDef(ctx, mb, name, archiveName);
}

std::string elf::replaceThinLTOSuffix(Ctx &ctx, StringRef path) {
  auto [suffix, repl] = ctx.arg.thinLTOObjectSuffixReplace;
  if (path.consume_back(suffix))
    return (path + repl).str();
  return std::string(path);
}

//===----------------------------------------------------------------------===//
// Parallel input file parsing and symbol resolution pipeline.
//
// elf::parseFiles is implemented as a parallel pipeline that replaces the
// serial per-file parse loop while replicating its exact semantics:
//
// 1. Read symbols (parallel per file): collect per-file POD records for every
//    compatible object, shared and bitcode file (including lazy archive
//    members), stably counting-sorted into hash buckets.
// 2. Build per-bucket symbol database (parallel per bucket): a name -> id map
//    plus two chains per name in command-line order (all refs, defs only).
// 3. Activation engine: replay serial archive member extraction with a
//    positional fixpoint. A strong undefined reference acting at serial point
//    P pairs with the first inactive lazy definition L and extracts it at
//    time T = max(P, L+1) unless a non-lazy definition precedes T.
// 4. Create and resolve symbols (parallel per bucket): issue the real
//    Symbol::resolve calls for object file references in serial parse order,
//    then order all symbols by their serial insertion point so the output
//    .symtab matches the serial linker. The bucket maps become the sharded
//    SymbolTable maps.
// 5. Wire global symbols into the per-file symbol arrays (parallel).
//
// Non-object files (shared, bitcode, binary) are then parsed serially through
// the regular parseFile dispatcher in serial order.
//
//===----------------------------------------------------------------------===//

namespace {
constexpr uint32_t numShards = SymbolTable::numShards;

// SymRecord flags.
enum : uint8_t {
  FDef = 1,     // defined, including COMMON
  FWeak = 2,    // STB_WEAK
  FBitcode = 4, // from a bitcode file
  FShared = 8,  // from a shared file
  FCommon = 16, // COMMON definition
  FHasAt = 32,  // the name contains '@'
};

struct SymRecord {
  const char *name;
  uint32_t stemLen; // bucket key length (name minus a @@ suffix)
  uint32_t nameLen; // full name length
  uint32_t hash;    // DenseMap hash of the stem
  uint32_t nameId;  // index into Bucket::names; written in phase 2
  uint32_t elfIdx;  // symbol index within the file's symbol/IR table
  uint8_t flags;

  StringRef stem() const { return StringRef(name, stemLen); }
};

// POD mirror of CachedHashStringRef (default-constructible for
// resize_for_overwrite).
struct CachedName {
  const char *data;
  uint32_t size;
  uint32_t hash;
  CachedHashStringRef ref() const {
    return CachedHashStringRef(StringRef(data, size), hash);
  }
};

struct FileData {
  SmallVector<SymRecord, 0> records; // stably bucketed by hash % numShards
  // Record indices in symbol table order, for the serial extraction replay.
  SmallVector<uint32_t, 0> symOrder;
  uint32_t bucketStart[numShards + 1] = {};
  // GRP_COMDAT signatures, stably bucketed by hash % numShards; section order
  // within a bucket.
  SmallVector<CachedName, 0> comdats;
  uint32_t comdatStart[numShards + 1] = {};
  EarlySectionInfo early;
  bool eligible = false;   // participates in the pipeline
  bool compatible = false; // passed the compatibility check
};

struct RefNode {
  uint32_t fileIdx;
  uint32_t recIdx;
  uint32_t next = UINT32_MAX;
  uint32_t nextDef = UINT32_MAX;
};

struct NameInfo {
  // The last @@-versioned spelling: serial insert() renames the symbol on
  // every versioned insertion.
  const char *verName = nullptr;
  Symbol *sym = nullptr;
  // Output order key: the visit rank and sub-index of the earliest resolution
  // event; seeds use (0, seedIdx). UINT32_MAX rank means no file ended up
  // inserting the name, so it is dropped.
  uint64_t anchorSub = UINT64_MAX;
  uint32_t anchorRank = UINT32_MAX;
  uint32_t firstRef = UINT32_MAX, lastRef = UINT32_MAX;
  uint32_t firstDef = UINT32_MAX, lastDef = UINT32_MAX;
  uint32_t verNameLen = 0;
  uint32_t seedIdx = UINT32_MAX;      // pre-parseFiles symVector index
  uint32_t firstLazyDef = UINT32_MAX; // first definition in a lazy file
  uint32_t outIdx = UINT32_MAX;       // final symVector index
};

struct Bucket {
  DenseMap<CachedHashStringRef, int> map;
  SmallVector<NameInfo, 0> names;
  SmallVector<RefNode, 0> refs;
};

struct SerialEvent {
  uint32_t fileIdx;
  bool full; // full parse event (vs lazy symbol insertion)
};

// A per-name resolution event, ordered by key = (visit rank << 33) |
// (undefined-phase << 32) | symbol index. r indexes Bucket::refs.
struct REvent {
  uint64_t key;
  uint32_t r;
};

template <class ELFT> struct Pipeline {
  Ctx &ctx;
  const SmallVector<std::unique_ptr<InputFile>, 0> &driverFiles;
  SmallVector<InputFile *, 0> files; // snapshot of driverFiles
  SmallVector<FileData, 0> fd;
  std::array<Bucket, numShards> buckets;
  SmallVector<Symbol *, 0> seeds;      // pre-parseFiles symbols
  SmallVector<CachedName, 0> seedKeys; // their stems
  // For a member extracted at its own lazy visit, the symtab index of the
  // triggering definition: parseLazy stops inserting there, so later
  // definitions are inserted by the full parse instead.
  SmallVector<uint32_t, 0> actDefIdx;
  SmallVector<SerialEvent, 0> events; // serial visit/extraction order
  SmallVector<uint32_t, 0> fullRank, lazyRank;
  // Extractions in completion order, for --why-extract/--warn-backrefs.
  struct Extraction {
    uint32_t member, trigFile, trigSym, bucket, nameId;
    bool fortran;
  };
  SmallVector<Extraction, 0> extractions;
  uint32_t bucketBase[numShards + 1]; // global name id = base[bucket] + nameId
  bool hasTraced = false;

  Pipeline(Ctx &ctx, const SmallVector<std::unique_ptr<InputFile>, 0> &f)
      : ctx(ctx), driverFiles(f) {}

  void run();
  void readSymbols();
  void readObj(uint32_t i);
  void readShared(uint32_t i);
  void readBitcode(uint32_t i);
  void buildNameDB();
  void activate();
  void commitFiles();
  void resolveSymbols();
  void resolveName(Bucket &b, uint32_t nameId, SmallVectorImpl<REvent> &order);

  // The file's visit rank: extracted members rank at their extraction point,
  // not their command-line position.
  uint32_t effRank(uint32_t f) const {
    return fullRank[f] != UINT32_MAX ? fullRank[f] : lazyRank[f];
  }
  void wireSymbols();
  void recordExtractions();
  void epilogue();

  void addRecord(SmallVectorImpl<SymRecord> &tmp, StringRef name,
                 uint32_t elfIdx, uint8_t flags) {
    SymRecord r;
    r.name = name.data();
    r.nameLen = name.size();
    auto [stemLen, hasAt] = getSymbolStem(name);
    r.stemLen = stemLen;
    if (hasAt)
      flags |= FHasAt;
    r.hash = CachedHashStringRef(StringRef(r.name, r.stemLen)).hash();
    r.nameId = UINT32_MAX;
    r.elfIdx = elfIdx;
    r.flags = flags;
    tmp.push_back(r);
  }
};

// Stable counting sort into numShards buckets by hash, recording the bucket
// boundaries in start[]. If order is given, order[i] receives the destination
// position of tmp[i].
template <class T, class HashFn>
static void bucketSort(SmallVectorImpl<T> &dst, ArrayRef<T> tmp,
                       uint32_t (&start)[numShards + 1], HashFn hash,
                       uint32_t *order = nullptr) {
  uint32_t count[numShards] = {};
  for (const T &r : tmp)
    ++count[hash(r) % numShards];
  uint32_t sum = 0;
  for (uint32_t i = 0; i != numShards; ++i) {
    start[i] = sum;
    sum += count[i];
  }
  start[numShards] = sum;
  uint32_t cursor[numShards];
  memcpy(cursor, start, sizeof(cursor));
  dst.resize_for_overwrite(tmp.size());
  for (auto [i, r] : llvm::enumerate(tmp)) {
    uint32_t pos = cursor[hash(r) % numShards]++;
    dst[pos] = r;
    if (order)
      order[i] = pos;
  }
}

// Stably group the records by hash bucket for the parallel phases; symOrder
// retains the symbol table order for the serial extraction replay.
static void bucketize(FileData &d, SmallVectorImpl<SymRecord> &tmp) {
  d.symOrder.resize_for_overwrite(tmp.size());
  bucketSort(
      d.records, ArrayRef<SymRecord>(tmp), d.bucketStart,
      [](const SymRecord &r) { return r.hash; }, d.symOrder.data());
}

static void bucketizeComdats(FileData &d, ArrayRef<StringRef> sigs) {
  SmallVector<CachedName, 0> tmp;
  tmp.reserve(sigs.size());
  for (StringRef s : sigs)
    tmp.push_back(
        {s.data(), (uint32_t)s.size(), CachedHashStringRef(s).hash()});
  bucketSort(d.comdats, ArrayRef<CachedName>(tmp), d.comdatStart,
             [](const CachedName &s) { return s.hash; });
}

} // namespace

template <class ELFT> void Pipeline<ELFT>::readSymbols() {
  size_t n = driverFiles.size();
  files.reserve(n);
  for (auto &f : driverFiles)
    files.push_back(f.get());
  fd.resize(files.size());

  // Diagnose incompatible files in command-line order; they are skipped
  // entirely, as in the serial linker.
  InputFile *firstObj = nullptr, *firstShared = nullptr, *firstBc = nullptr;
  for (auto [i, f] : llvm::enumerate(files)) {
    fd[i].compatible = isCompatible(ctx, f, firstObj, firstShared, firstBc);
    if (!fd[i].compatible || f->lazy)
      continue;
    if (f->kind() == InputFile::ObjKind) {
      if (!firstObj)
        firstObj = f;
    } else if (f->kind() == InputFile::SharedKind) {
      if (!firstShared)
        firstShared = f;
    } else if (f->kind() == InputFile::BitcodeKind) {
      if (!firstBc)
        firstBc = f;
    }
  }

  // Save bitcode symbol names through the string savers serially (they are
  // not thread-safe). This also gives LTO NUL-terminated persistent names.
  for (auto [i, f] : llvm::enumerate(files)) {
    if (!fd[i].compatible)
      continue;
    if (auto *bf = dyn_cast<BitcodeFile>(f)) {
      for (const lto::InputFile::Symbol &irSym : bf->obj->symbols())
        irSym.Name = ctx.uniqueSaver.save(irSym.getName());
      fd[i].eligible = true;
    } else if (isa<SharedFile>(f) || f->kind() == InputFile::ObjKind) {
      fd[i].eligible = true;
    }
  }

  parallelFor(0, files.size(), [&](size_t i) {
    if (!fd[i].eligible)
      return;
    switch (files[i]->kind()) {
    case InputFile::ObjKind:
      readObj(i);
      break;
    case InputFile::SharedKind:
      readShared(i);
      break;
    case InputFile::BitcodeKind:
      readBitcode(i);
      break;
    default:
      llvm_unreachable("unexpected file kind");
    }
  });

  // DSOs are uniquified by soname: a duplicate contributes no symbols. The
  // epilogue's SharedFile::parse performs the real deduplication.
  DenseSet<CachedHashStringRef> sonames;
  for (auto [i, f] : llvm::enumerate(files))
    if (fd[i].eligible && isa<SharedFile>(f))
      if (!sonames.insert(CachedHashStringRef(cast<SharedFile>(f)->soName))
               .second) {
        fd[i].records.clear();
        fd[i].symOrder.clear();
        memset(fd[i].bucketStart, 0, sizeof(fd[i].bucketStart));
      }
}

template <class ELFT> void Pipeline<ELFT>::readObj(uint32_t i) {
  auto *f = cast<ObjFile<ELFT>>(files[i]);
  ArrayRef<typename ELFT::Sym> eSyms = f->template getELFSyms<ELFT>();
  uint32_t firstGlobal = f->getFirstGlobal();
  StringRef strtab = f->getStringTable();
  SmallVector<SymRecord, 0> tmp;
  tmp.reserve(eSyms.size() - firstGlobal);
  for (size_t j = firstGlobal, e = eSyms.size(); j != e; ++j) {
    const typename ELFT::Sym &eSym = eSyms[j];
    Expected<StringRef> name = eSym.getName(strtab);
    if (!name) {
      Err(ctx) << f << ": " << name.takeError();
      break;
    }
    uint8_t flags = 0;
    if (eSym.st_shndx != SHN_UNDEF)
      flags |= FDef;
    if (eSym.st_shndx == SHN_COMMON) {
      flags |= FCommon;
      f->hasCommonSyms = true;
    }
    if (eSym.getBinding() == STB_WEAK)
      flags |= FWeak;
    addRecord(tmp, *name, j, flags);
  }
  bucketize(fd[i], tmp);
  if (!f->justSymbols)
    f->scanEarlySections(fd[i].early);
  bucketizeComdats(fd[i], fd[i].early.comdats);
  // Only deplibSections/attributeSections are consumed later.
  fd[i].early.comdats = {};
}

template <class ELFT> void Pipeline<ELFT>::readShared(uint32_t i) {
  auto *f = cast<SharedFile>(files[i]);
  const ELFFile<ELFT> obj = f->template getObj<ELFT>();
  ArrayRef<typename ELFT::Shdr> sections = f->template getELFShdrs<ELFT>();
  const typename ELFT::Shdr *versymSec = nullptr, *verdefSec = nullptr;
  ArrayRef<typename ELFT::Dyn> dynamicTags;
  for (const typename ELFT::Shdr &sec : sections) {
    switch (sec.sh_type) {
    case SHT_DYNAMIC:
      if (Expected<ArrayRef<typename ELFT::Dyn>> tags =
              obj.template getSectionContentsAsArray<typename ELFT::Dyn>(sec))
        dynamicTags = *tags;
      else
        consumeError(tags.takeError());
      break;
    case SHT_GNU_versym:
      versymSec = &sec;
      break;
    case SHT_GNU_verdef:
      verdefSec = &sec;
      break;
    }
  }
  // DT_SONAME determines the deduplication key.
  for (const typename ELFT::Dyn &dyn : dynamicTags)
    if (dyn.d_tag == DT_SONAME)
      if (uint64_t val = dyn.getVal(); val < f->getStringTable().size())
        f->soName = f->getStringTable().data() + val;

  // The number of version definitions, for bounds checking below.
  size_t verdefCount = parseVerdefs<ELFT>(obj.base(), verdefSec).size();

  uint32_t firstGlobal = f->getFirstGlobal();
  size_t size = f->template getELFSyms<ELFT>().size() - firstGlobal;
  ArrayRef<typename ELFT::Versym> versyms;
  if (versymSec && size) {
    if (Expected<ArrayRef<typename ELFT::Versym>> vs =
            obj.template getSectionContentsAsArray<typename ELFT::Versym>(
                *versymSec))
      versyms = vs->slice(firstGlobal);
    else
      consumeError(vs.takeError());
  }

  ArrayRef<typename ELFT::Sym> syms = f->template getGlobalELFSyms<ELFT>();
  StringRef strtab = f->getStringTable();
  SmallVector<SymRecord, 0> tmp;
  tmp.reserve(syms.size());
  for (size_t j = 0, e = syms.size(); j != e; ++j) {
    const typename ELFT::Sym &sym = syms[j];
    if (sym.getBinding() == STB_LOCAL)
      continue;
    Expected<StringRef> name = sym.getName(strtab);
    if (!name) {
      consumeError(name.takeError());
      continue;
    }
    uint16_t ver = j < versyms.size() ? uint16_t(versyms[j].vs_index)
                                      : uint16_t(VER_NDX_GLOBAL);
    // Versioned undefined symbols and hidden or local-versioned definitions
    // are registered under name@version (if at all) by the epilogue's
    // SharedFile::parse, not under the unversioned name.
    if (!addsUnversionedName(sym.isUndefined(), ver, verdefCount))
      continue;
    uint8_t flags = FShared;
    if (!sym.isUndefined())
      flags |= FDef;
    if (sym.getBinding() == STB_WEAK)
      flags |= FWeak;
    addRecord(tmp, *name, firstGlobal + j, flags);
  }
  bucketize(fd[i], tmp);
}

template <class ELFT> void Pipeline<ELFT>::readBitcode(uint32_t i) {
  auto *f = cast<BitcodeFile>(files[i]);
  SmallVector<SymRecord, 0> tmp;
  for (auto [j, irSym] : llvm::enumerate(f->obj->symbols())) {
    uint8_t flags = FBitcode;
    if (!irSym.isUndefined())
      flags |= FDef;
    if (irSym.isWeak())
      flags |= FWeak;
    if (irSym.isCommon())
      flags |= FCommon;
    addRecord(tmp, irSym.getName(), j, flags);
  }
  bucketize(fd[i], tmp);
  SmallVector<StringRef, 0> sigs;
  for (auto s : f->obj->getComdatTable())
    if (s.second != Comdat::NoDeduplicate)
      sigs.push_back(s.first);
  bucketizeComdats(fd[i], sigs);
}

template <class ELFT> void Pipeline<ELFT>::buildNameDB() {
  // Seed the buckets with pre-parseFiles symbols (-u, --trace-symbol) so they
  // come first in the serial insertion order.
  seeds.assign(ctx.symtab->getSymbols().begin(),
               ctx.symtab->getSymbols().end());
  for (Symbol *s : seeds)
    if (s->traced)
      hasTraced = true;

  size_t total = 0;
  for (FileData &d : fd)
    total += d.records.size();

  // Hash seed stems once rather than in every bucket.
  seedKeys.reserve(seeds.size());
  for (Symbol *s : seeds) {
    StringRef stem = s->getName().take_front(getSymbolStem(s->getName()).first);
    seedKeys.push_back(
        {stem.data(), (uint32_t)stem.size(), CachedHashStringRef(stem).hash()});
  }

  parallelFor(0, numShards, [&](size_t b) {
    Bucket &bu = buckets[b];
    bu.names.reserve(total / numShards / 4 + seeds.size());
    bu.refs.reserve(total / numShards + 1);
    bu.map.reserve(total / numShards / 4 + seeds.size());
    for (auto [i, k] : llvm::enumerate(seedKeys)) {
      if (k.hash % numShards != b)
        continue;
      auto [it, inserted] = bu.map.try_emplace(k.ref(), bu.names.size());
      if (inserted) {
        NameInfo &ni = bu.names.emplace_back();
        ni.seedIdx = i;
        ni.sym = seeds[i];
      }
    }
    auto append = [&bu](uint32_t &first, uint32_t &last, uint32_t idx,
                        uint32_t RefNode::*next) {
      if (first == UINT32_MAX)
        first = idx;
      else
        bu.refs[last].*next = idx;
      last = idx;
    };
    for (auto [i, d] : llvm::enumerate(fd)) {
      bool lazy = files[i]->lazy;
      for (uint32_t r = d.bucketStart[b], e = d.bucketStart[b + 1]; r != e;
           ++r) {
        SymRecord &rec = d.records[r];
        CachedHashStringRef key(rec.stem(), rec.hash);
        auto [it, inserted] = bu.map.try_emplace(key, bu.names.size());
        if (inserted)
          bu.names.emplace_back();
        uint32_t nameId = it->second;
        rec.nameId = nameId;
        NameInfo &ni = bu.names[nameId];
        uint32_t refIdx = bu.refs.size();
        RefNode &node = bu.refs.emplace_back();
        node.fileIdx = i;
        node.recIdx = r;
        append(ni.firstRef, ni.lastRef, refIdx, &RefNode::next);
        if (rec.flags & FDef) {
          append(ni.firstDef, ni.lastDef, refIdx, &RefNode::nextDef);
          if (lazy && ni.firstLazyDef == UINT32_MAX)
            ni.firstLazyDef = refIdx;
        }
        if (rec.stemLen != rec.nameLen) {
          ni.verName = rec.name;
          ni.verNameLen = rec.nameLen;
        }
      }
    }
  });

  bucketBase[0] = 0;
  for (uint32_t b = 0; b != numShards; ++b)
    bucketBase[b + 1] = bucketBase[b] + buckets[b].names.size();
}

template <class ELFT> void Pipeline<ELFT>::activate() {
  // Serial replay of archive member extraction over the pre-digested
  // records. Extraction is the only order-sensitive part of symbol
  // resolution: a strong undefined reference extracts the first lazy
  // definition of the name, synchronously and recursively, nesting like the
  // serial linker; definitions satisfy later references. The replay tracks
  // one state byte per name and assigns every file visit and extraction a
  // sequence number that the parallel resolution phase orders by.
  actDefIdx.assign(files.size(), UINT32_MAX);
  fullRank.assign(files.size(), UINT32_MAX);
  lazyRank.assign(files.size(), UINT32_MAX);

  // A definition is sticky against weaker kinds: a COMMON definition is
  // overridden only by a strong regular definition, and itself overrides
  // weak/shared definitions (commons merge; see Symbol::resolve).
  enum : uint8_t { SNone, SPending, SWeakish, SCommon, SStrong };
  uint32_t numNames = bucketBase[numShards];
  SmallVector<uint8_t, 0> state(numNames);
  struct Attr {
    uint32_t file, sym;
  };
  SmallVector<Attr, 0> attr;
  attr.resize_for_overwrite(numNames);
  // File index of the first lazy definition per name, or UINT32_MAX.
  SmallVector<uint32_t, 0> lazyDef;
  lazyDef.resize_for_overwrite(numNames);
  parallelFor(0, numShards, [&](size_t b) {
    Bucket &bu = buckets[b];
    for (auto [id, ni] : llvm::enumerate(bu.names))
      lazyDef[bucketBase[b] + id] = ni.firstLazyDef == UINT32_MAX
                                        ? UINT32_MAX
                                        : bu.refs[ni.firstLazyDef].fileIdx;
  });
  auto globalId = [&](const SymRecord &rec) {
    return bucketBase[rec.hash % numShards] + rec.nameId;
  };

  // Seeded -u references act before all files. The sentinel trigger file
  // index files.size() denotes ctx.internalFile.
  for (auto [i, s] : llvm::enumerate(seeds))
    if (s->isUndefined() && !s->isWeak()) {
      auto it =
          buckets[seedKeys[i].hash % numShards].map.find(seedKeys[i].ref());
      if (it != buckets[seedKeys[i].hash % numShards].map.end()) {
        uint32_t id = bucketBase[seedKeys[i].hash % numShards] + it->second;
        if (state[id] == SNone) {
          state[id] = SPending;
          attr[id] = {(uint32_t)files.size(), (uint32_t)i};
        }
      }
    }

  uint32_t seq = 1;
  auto applyDef = [&](const SymRecord &rec, uint32_t f) {
    uint8_t &s = state[globalId(rec)];
    if (rec.flags & FCommon) {
      if (s != SStrong && s != SCommon) {
        s = SCommon;
        // Attribution for --fortran-common extractions (approximating the
        // serial choice, the largest common's file, with the first).
        attr[globalId(rec)] = {f, rec.elfIdx};
      }
    } else if (rec.flags & (FWeak | FShared)) {
      if (s == SNone || s == SPending)
        s = SWeakish;
    } else {
      s = SStrong;
    }
  };

  // Process a full parse: definitions first, then strong undefined
  // references (the serial initializeSymbols order), possibly extracting
  // members recursively. One stack-disciplined undef buffer is shared across
  // the recursion: nested visits push past e and truncate back.
  SmallVector<uint32_t, 0> undefStack;
  auto visitFull = [&](uint32_t f, auto &applyUndefRef) {
    const FileData &d = fd[f];
    size_t start = undefStack.size();
    for (uint32_t j = 0, e = d.symOrder.size(); j != e; ++j) {
      if (j + 8 < e)
        LLVM_PREFETCH(&d.records[d.symOrder[j + 8]], 0, 3);
      uint32_t ri = d.symOrder[j];
      const SymRecord &rec = d.records[ri];
      if (rec.flags & FDef)
        applyDef(rec, f);
      else if (!(rec.flags & FWeak))
        undefStack.push_back(ri);
    }
    for (size_t k = start, e = undefStack.size(); k != e; ++k)
      applyUndefRef(d.records[undefStack[k]], f, applyUndefRef);
    undefStack.truncate(start);
  };

  // extractMember and applyUndef recurse into each other, replaying the
  // synchronous extraction of the serial linker.
  auto extractMember = [&](uint32_t m, uint32_t trigF, uint32_t trigSym,
                           uint32_t bucket, uint32_t nameId, uint32_t cutoff,
                           bool fortran, auto &applyUndefRef) -> void {
    files[m]->lazy = false;
    fullRank[m] = seq++;
    events.push_back({m, true});
    actDefIdx[m] = cutoff;
    visitFull(m, applyUndefRef);
    // Completion order, as the serial linker records after the synchronous
    // extract() returns.
    extractions.push_back({m, trigF, trigSym, bucket, nameId, fortran});
  };

  auto applyUndef = [&](const SymRecord &rec, uint32_t f, auto &self) -> void {
    if (rec.flags & FWeak)
      return;
    uint32_t id = globalId(rec);
    uint8_t &s = state[id];
    if (s == SWeakish || s == SCommon || s == SStrong)
      return;
    if (s == SNone) {
      s = SPending;
      attr[id] = {f, rec.elfIdx};
    }
    // Extract the first lazy definition if its member was already visited
    // (the serial symbol is a LazySymbol at this point).
    uint32_t m = lazyDef[id];
    if (m == UINT32_MAX || lazyRank[m] == UINT32_MAX || !files[m]->lazy)
      return;
    extractMember(m, attr[id].file, attr[id].sym, rec.hash % numShards,
                  rec.nameId, UINT32_MAX, false, self);
  };

  // Visit the files in command-line order, mirroring the serial loop.
  for (auto [i, f] : llvm::enumerate(files)) {
    if (!fd[i].compatible)
      continue;
    if (!f->lazy) {
      if (fullRank[i] != UINT32_MAX)
        continue; // an extracted member
      fullRank[i] = seq++;
      events.push_back({(uint32_t)i, true});
      visitFull(i, applyUndef);
      continue;
    }
    // Lazy file visit: serial parseLazy inserts LazySymbols for the
    // definitions in order, extracting the file at the first definition
    // whose name has a pending strong reference (or a COMMON with
    // --fortran-common). Object files stop there (the nested full parse
    // takes over); for bitcode the loop has no early exit, but continuing
    // is a no-op once the definitions resolve.
    lazyRank[i] = seq++;
    events.push_back({(uint32_t)i, false});
    if (!fd[i].eligible)
      continue;
    bool bc = files[i]->kind() == InputFile::BitcodeKind;
    for (uint32_t ri : fd[i].symOrder) {
      const SymRecord &rec = fd[i].records[ri];
      if (!(rec.flags & FDef))
        continue;
      uint32_t id = globalId(rec);
      bool fortran = false;
      if (state[id] != SPending) {
        if (!(state[id] == SCommon && ctx.arg.fortranCommon &&
              !(rec.flags & (FWeak | FCommon | FShared))))
          continue;
        fortran = true;
      }
      extractMember(i, attr[id].file, attr[id].sym, rec.hash % numShards,
                    rec.nameId, bc ? UINT32_MAX : rec.elfIdx, fortran,
                    applyUndef);
      break;
    }
  }
}

template <class ELFT> void Pipeline<ELFT>::commitFiles() {
  // Register object files in serial parse order (this determines output
  // section order), pre-populate the comdat group shards, and print -t
  // traces for the initial batch.
  for (const SerialEvent &ev : events) {
    if (!ev.full)
      continue;
    InputFile *f = files[ev.fileIdx];
    if (ctx.arg.trace)
      Msg(ctx) << f;
    if (fd[ev.fileIdx].eligible && f->kind() == InputFile::ObjKind)
      ctx.objectFiles.push_back(cast<ELFFileBase>(f));
  }
  // First-parsed file in serial order owns each comdat group.
  llvm::TimeTraceScope comdatScope("Pre-populate comdat groups");
  size_t numComdats = 0;
  for (const FileData &d : fd)
    numComdats += d.comdats.size();
  parallelFor(0, numShards, [&](size_t s) {
    ctx.symtab->comdatGroups[s].reserve(numComdats / numShards / 2);
    for (const SerialEvent &ev : events) {
      if (!ev.full)
        continue;
      const FileData &d = fd[ev.fileIdx];
      for (uint32_t i = d.comdatStart[s], e = d.comdatStart[s + 1]; i != e; ++i)
        ctx.symtab->addComdatGroup(d.comdats[i].ref(), files[ev.fileIdx]);
    }
  });
}

template <class ELFT>
void Pipeline<ELFT>::resolveName(Bucket &bu, uint32_t nameId,
                                 SmallVectorImpl<REvent> &order) {
  NameInfo &ni = bu.names[nameId];
  Symbol *sym = ni.sym;

  // Apply the @@ rename and default flags. Serial insert() renames the symbol
  // on every versioned insertion and flags any name containing '@'.
  if (ni.verName) {
    sym->setName(StringRef(ni.verName, ni.verNameLen));
    sym->hasVersionSuffix = true;
  } else if (ni.seedIdx == UINT32_MAX &&
             (fd[bu.refs[ni.firstRef].fileIdx]
                  .records[bu.refs[ni.firstRef].recIdx]
                  .flags &
              FHasAt)) {
    sym->hasVersionSuffix = true;
  }

  // Order the references by their file's visit rank, then definitions before
  // undefined references, then symbol index, so resolution matches the serial
  // linker (extracted members resolve at their extraction point). The output
  // order anchor is the earliest (rank, symbol index), ignoring the phase.
  order.clear();
  uint32_t anchorRank = ni.seedIdx == UINT32_MAX ? UINT32_MAX : 0;
  uint64_t anchorSub = ni.seedIdx == UINT32_MAX ? UINT64_MAX : ni.seedIdx;
  for (uint32_t r = ni.firstRef; r != UINT32_MAX; r = bu.refs[r].next) {
    const RefNode &node = bu.refs[r];
    const SymRecord &rec = fd[node.fileIdx].records[node.recIdx];
    bool isDef = rec.flags & FDef;
    // An unextracted lazy file contributes only its definitions (as
    // LazySymbols); its undefined references are never inserted.
    if (!isDef && files[node.fileIdx]->lazy)
      continue;
    uint32_t rank = effRank(node.fileIdx);
    uint64_t undefPhase = isDef ? 0 : 1;
    order.push_back(
        {(uint64_t(rank) << 33) | (undefPhase << 32) | rec.elfIdx, r});
    // The output anchor is the insertion point: the file that first inserts the
    // symbol, definitions before undefined references (as the serial parse
    // processes them), then symbol index. parseLazy inserts a lazy file's
    // definitions as LazySymbols at the lazy-visit rank, but only up to the
    // definition that triggered extraction (actDefIdx); later definitions are
    // inserted by the full parse at fullRank.
    uint32_t arank = isDef && lazyRank[node.fileIdx] != UINT32_MAX &&
                             rec.elfIdx <= actDefIdx[node.fileIdx]
                         ? lazyRank[node.fileIdx]
                         : rank;
    // Object/shared symbols are inserted in symbol-table order; bitcode files
    // insert definitions before undefined references (BitcodeFile::parse).
    uint64_t asub =
        (rec.flags & FBitcode) ? (undefPhase << 32) | rec.elfIdx : rec.elfIdx;
    if (arank < anchorRank || (arank == anchorRank && asub < anchorSub)) {
      anchorRank = arank;
      anchorSub = asub;
    }
  }
  llvm::sort(order,
             [](const REvent &a, const REvent &b) { return a.key < b.key; });
  ni.anchorRank = anchorRank;
  ni.anchorSub = anchorSub;

  // -y defers traced names to the serial replay in the epilogue.
  if (sym->traced)
    return;

  // Bitcode and shared symbols are resolved later by the epilogue's
  // BitcodeFile/SharedFile::parse; here we only track an earlier bitcode
  // definition, which suppresses a later object definition.
  uint8_t bcDef = 0;      // 1: earlier weak bitcode def, 2: earlier strong
  bool nonObjDef = false; // a shared or bitcode definition precedes here
  bool inserted = ni.seedIdx != UINT32_MAX;
  for (const REvent &e : order) {
    const RefNode &node = bu.refs[e.r];
    uint32_t f = node.fileIdx;
    const SymRecord &rec = fd[f].records[node.recIdx];
    bool isDef = rec.flags & FDef;
    if (rec.flags & FShared) {
      inserted = true; // SharedFile::parse inserts the symbol in the epilogue
      nonObjDef |= isDef;
      continue;
    }
    if (rec.flags & FBitcode) {
      if (files[f]->lazy)
        continue;      // unextracted lazy bitcode contributes nothing
      inserted = true; // BitcodeFile::parse resolves the symbol in the epilogue
      if (isDef) {
        nonObjDef = true;
        if (!(rec.flags & FCommon))
          bcDef = std::max<uint8_t>(bcDef, rec.flags & FWeak ? 1 : 2);
      }
      continue;
    }
    auto *obj = cast<ObjFile<ELFT>>(files[f]);
    if (files[f]->lazy) {
      // Unextracted lazy object: only its definitions are visible, as
      // LazySymbols (ObjFile::parseLazy).
      if (isDef) {
        inserted = true;
        if (!nonObjDef &&
            (sym->isPlaceholder() || (sym->isUndefined() && sym->isWeak())))
          sym->resolve(ctx, LazySymbol{*obj});
      }
      continue;
    }
    inserted = true;
    const typename ELFT::Sym &eSym =
        obj->template getELFSyms<ELFT>()[rec.elfIdx];
    if (!isDef) {
      // resolveSymbol would extract a lazy member; activate already made the
      // extraction decision, so for a strong reference to a member it chose not
      // to extract, only merge visibility (as resolve(Undefined) would).
      if (sym->isLazy() && eSym.getBinding() != STB_WEAK) {
        sym->mergeVisibility(eSym.st_other & 3);
        sym->isUsedInRegularObj = true;
        sym->referenced = true;
      } else {
        resolveSymbol(ctx, obj, eSym, *sym);
      }
      continue;
    }
    if (!(rec.flags & FCommon)) {
      uint8_t binding = eSym.getBinding();
      if (bcDef == 2 || (bcDef == 1 && binding == STB_WEAK)) {
        sym->isUsedInRegularObj = true;
        sym->mergeVisibility(eSym.st_other & 3);
        continue;
      }
      if (bcDef == 1 && binding != STB_WEAK)
        bcDef = 0;
    }
    resolveSymbol(ctx, obj, eSym, *sym);
  }
  if (!inserted)
    ni.anchorRank = UINT32_MAX;
}

template <class ELFT> void Pipeline<ELFT>::resolveSymbols() {
  // Create and resolve symbols per bucket.
  {
    llvm::TimeTraceScope scope1("Resolve buckets");
    parallelFor(0, numShards, [&](size_t b) {
      Bucket &bu = buckets[b];
      size_t n = bu.names.size();
      if (!n)
        return;
      SymbolUnion *storage = makeThreadLocalN<SymbolUnion>(n);
      SmallVector<REvent, 0> order;
      for (size_t id = 0; id != n; ++id) {
        NameInfo &ni = bu.names[id];
        SymbolUnion *su = &storage[id];
        if (ni.seedIdx != UINT32_MAX) {
          memcpy(static_cast<void *>(su), ni.sym, sizeof(SymbolUnion));
        } else {
          memset(static_cast<void *>(su), 0, sizeof(SymbolUnion));
          auto *s = reinterpret_cast<Symbol *>(su);
          // Find the key for this name to set the initial name.
          // The first reference's stem is the map key.
          s->versionId = VER_NDX_GLOBAL;
          const RefNode &node = bu.refs[ni.firstRef];
          const SymRecord &rec = fd[node.fileIdx].records[node.recIdx];
          s->setName(rec.stem());
        }
        ni.sym = reinterpret_cast<Symbol *>(su);
        resolveName(bu, id, order);
      }
    });
  }

  llvm::TimeTraceScope scope2("Order symbols");
  // Order all symbols by their serial insertion point and move them into the
  // final storage so the output .symtab matches the serial linker.
  struct Item {
    uint64_t ord, sub;
    uint32_t bucket, nameId;
  };
  SmallVector<Item, 0> items;
  size_t total = bucketBase[numShards];
  items.resize_for_overwrite(total);
  parallelFor(0, numShards, [&](size_t b) {
    Bucket &bu = buckets[b];
    Item *out = items.begin() + bucketBase[b];
    for (auto [id, ni] : llvm::enumerate(bu.names))
      out[id] = {ni.anchorRank, ni.anchorSub, (uint32_t)b, (uint32_t)id};
  });
  parallelSort(items.begin(), items.end(), [](const Item &a, const Item &b) {
    if (a.ord != b.ord)
      return a.ord < b.ord;
    if (a.sub != b.sub)
      return a.sub < b.sub;
    if (a.bucket != b.bucket)
      return a.bucket < b.bucket;
    return a.nameId < b.nameId;
  });

  // Names that no serial insertion event would have created (e.g. names only
  // referenced by undefined symbols of unextracted lazy members) are dropped:
  // they must not appear in symVector or the installed maps.
  size_t live = total;
  while (live && items[live - 1].ord == UINT32_MAX)
    --live;

  SymbolUnion *out = getSpecificAllocSingleton<SymbolUnion>().Allocate(live);
  SmallVector<Symbol *, 0> symVector(live);
  parallelFor(0, live, [&](size_t i) {
    const Item &it = items[i];
    NameInfo &ni = buckets[it.bucket].names[it.nameId];
    memcpy(static_cast<void *>(&out[i]), ni.sym, sizeof(SymbolUnion));
    ni.sym = reinterpret_cast<Symbol *>(&out[i]);
    ni.outIdx = i;
    symVector[i] = ni.sym;
  });

  // Rewrite the bucket map values to symVector indices and install.
  parallelFor(0, numShards, [&](size_t b) {
    Bucket &bu = buckets[b];
    for (const NameInfo &ni : bu.names)
      if (ni.outIdx == UINT32_MAX) {
        const RefNode &node = bu.refs[ni.firstRef];
        const SymRecord &rec = fd[node.fileIdx].records[node.recIdx];
        bu.map.erase(CachedHashStringRef(rec.stem(), rec.hash));
      }
    for (auto &kv : bu.map)
      kv.second = bu.names[kv.second].outIdx;
  });
  std::array<DenseMap<CachedHashStringRef, int>, numShards> maps;
  for (size_t b = 0; b != numShards; ++b)
    maps[b] = std::move(buckets[b].map);
  ctx.symtab->installShardedSymbols(maps, std::move(symVector));

  recordExtractions();
}

template <class ELFT> void Pipeline<ELFT>::recordExtractions() {
  // The extraction replay collected these in completion order, as the serial
  // linker records after the synchronous extract() returns.
  bool why = !ctx.arg.whyExtract.empty();
  if (!why && !ctx.arg.warnBackrefs)
    return;
  for (const Extraction &ex : extractions) {
    Bucket &bu = buckets[ex.bucket];
    NameInfo &ni = bu.names[ex.nameId];
    Symbol *sym = ni.sym;
    if (sym->traced)
      continue; // the -y serial replay issues the real resolve() calls
    InputFile *member = files[ex.member];
    InputFile *trigger =
        ex.trigFile == files.size() ? ctx.internalFile : files[ex.trigFile];
    if (why)
      ctx.whyExtractRecords.emplace_back(toStr(ctx, trigger), member, *sym);
    if (ex.fortran || !ctx.arg.warnBackrefs ||
        member->groupId >= trigger->groupId)
      continue;
    // Not reported if the member's own definition is weak (serial checks
    // isWeak() right after the synchronous extraction). A later lazy
    // definition dismisses the backref (linking sandwich), but only if the
    // symbol is a real Defined.
    bool dismissed = false;
    for (uint32_t r = ni.firstDef; r != UINT32_MAX; r = bu.refs[r].nextDef) {
      const RefNode &node = bu.refs[r];
      uint32_t f = node.fileIdx;
      if (f == ex.member)
        dismissed = fd[f].records[node.recIdx].flags & FWeak;
      else
        dismissed = sym->isDefined() && lazyRank[f] != UINT32_MAX &&
                    lazyRank[f] > fullRank[ex.member];
      if (dismissed)
        break;
    }
    if (!dismissed)
      ctx.backwardReferences.try_emplace(
          sym, std::make_pair((const InputFile *)trigger,
                              (const InputFile *)member));
  }
}

template <class ELFT> void Pipeline<ELFT>::wireSymbols() {
  parallelFor(0, files.size(), [&](size_t i) {
    if (!fd[i].eligible || files[i]->kind() != InputFile::ObjKind)
      return;
    InputFile *f = files[i];
    bool inactiveLazy = f->lazy;
    f->allocateSymbols();
    MutableArrayRef<Symbol *> syms = f->getMutableSymbols();
    for (const SymRecord &rec : fd[i].records) {
      // An unextracted lazy file has only its definitions wired, mirroring
      // ObjFile::parseLazy.
      if (inactiveLazy && !(rec.flags & FDef))
        continue;
      Bucket &bu = buckets[rec.hash % numShards];
      syms[rec.elfIdx] = bu.names[rec.nameId].sym;
    }
  });
}

template <class ELFT> void Pipeline<ELFT>::epilogue() {
  // Parse non-object files serially in serial parse order, process dependent
  // libraries and ARM attributes of object files, and replay resolution
  // events for -y traced symbols.
  for (const SerialEvent &ev : events) {
    InputFile *f = files[ev.fileIdx];
    FileData &d = fd[ev.fileIdx];
    if (!ev.full) {
      if (!d.eligible)
        continue;
      if (auto *bf = dyn_cast<BitcodeFile>(f)) {
        if (bf->lazy) {
          parseFile(ctx, bf); // parseLazy
        } else if (fullRank[ev.fileIdx] != UINT32_MAX) {
          // The serial linker ran parseLazy at this point before the member
          // was extracted; keep the lazyBitcodeFiles registration order.
          ctx.lazyBitcodeFiles.push_back(bf);
        }
        continue;
      }
      // Lazy object: replay LazySymbol insertions for traced names. symOrder
      // yields the records in ascending elfIdx order.
      if (hasTraced && f->kind() == InputFile::ObjKind) {
        for (uint32_t ri : d.symOrder) {
          const SymRecord &rec = d.records[ri];
          if (!(rec.flags & FDef) || rec.elfIdx > actDefIdx[ev.fileIdx])
            continue;
          Symbol *sym = buckets[rec.hash % numShards].names[rec.nameId].sym;
          if (sym->traced)
            sym->resolve(ctx, LazySymbol{*f});
        }
      }
      continue;
    }
    // Full parse events.
    if (!d.eligible || f->kind() != InputFile::ObjKind) {
      parseFile(ctx, f); // SharedFile, BitcodeFile, BinaryFile
      continue;
    }
    auto *obj = cast<ObjFile<ELFT>>(f);
    if (hasTraced) {
      // Replay defs-then-undefs for traced names with real resolve calls.
      ArrayRef<typename ELFT::Sym> eSyms = obj->template getELFSyms<ELFT>();
      for (int phase = 0; phase != 2; ++phase)
        for (uint32_t ri : d.symOrder) {
          const SymRecord &rec = d.records[ri];
          if ((phase == 0) != bool(rec.flags & FDef))
            continue;
          Symbol *sym = buckets[rec.hash % numShards].names[rec.nameId].sym;
          if (sym->traced)
            resolveSymbol(ctx, obj, eSyms[rec.elfIdx], *sym);
        }
    }
    obj->processEarlySections(d.early);
  }

  // Files can be appended during the epilogue (dependent libraries). Parse
  // them with the serial machinery.
  for (size_t i = files.size(); i < driverFiles.size(); ++i) {
    InputFile *f = driverFiles[i].get();
    if (ctx.arg.trace && !f->lazy)
      Msg(ctx) << f;
    parseFile(ctx, f);
  }
}

template <class ELFT> void Pipeline<ELFT>::run() {
  {
    llvm::TimeTraceScope timeScope("Read symbols");
    readSymbols();
  }
  {
    llvm::TimeTraceScope timeScope("Build symbol database");
    buildNameDB();
  }
  {
    llvm::TimeTraceScope timeScope("Activate archive members");
    activate();
  }
  commitFiles();
  {
    llvm::TimeTraceScope timeScope("Resolve symbols");
    resolveSymbols();
  }
  {
    llvm::TimeTraceScope timeScope("Wire symbols");
    wireSymbols();
  }
  {
    llvm::TimeTraceScope timeScope("Parse non-object files");
    epilogue();
  }
  if (ctx.driver.armCmseImpLib)
    cast<ObjFile<ELFT>>(*ctx.driver.armCmseImpLib).importCmseSymbols();
  // Free phase-local data in parallel; serial destruction measurably
  // serializes the frees.
  parallelFor(0, fd.size() + numShards, [&](size_t i) {
    if (i < fd.size()) {
      fd[i] = FileData();
    } else {
      Bucket &bu = buckets[i - fd.size()];
      bu.names = {};
      bu.refs = {};
    }
  });
}

template <class ELFT>
static void
doParseFiles(Ctx &ctx,
             const SmallVector<std::unique_ptr<InputFile>, 0> &files) {
  // With one thread the pipeline's record/replay overhead has no parallelism
  // to pay for it; use the serial parse loop. Both paths produce identical
  // output. Parsing may append files due to addDependentLibrary.
  if (ctx.arg.threadCount == 1) {
    for (size_t i = 0; i != files.size(); ++i) {
      InputFile *f = files[i].get();
      llvm::TimeTraceScope timeScope("Parse input files", f->getName());
      if (ctx.arg.trace && !f->lazy)
        Msg(ctx) << f;
      parseFile(ctx, f);
    }
    if (ctx.driver.armCmseImpLib)
      cast<ObjFile<ELFT>>(*ctx.driver.armCmseImpLib).importCmseSymbols();
    return;
  }
  Pipeline<ELFT> p(ctx, files);
  p.run();
}

void elf::parseFiles(Ctx &ctx,
                     const SmallVector<std::unique_ptr<InputFile>, 0> &files) {
  llvm::TimeTraceScope timeScope("Parse input files");
  invokeELFT(doParseFiles, ctx, files);
}

template class elf::ObjFile<ELF32LE>;
template class elf::ObjFile<ELF32BE>;
template class elf::ObjFile<ELF64LE>;
template class elf::ObjFile<ELF64BE>;

template void SharedFile::parse<ELF32LE>();
template void SharedFile::parse<ELF32BE>();
template void SharedFile::parse<ELF64LE>();
template void SharedFile::parse<ELF64BE>();
