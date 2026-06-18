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
#include <numeric>
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
// MIPS object files.) This function checks for that error. existing is an
// already-accepted file named by the fallback diagnostic.
static bool isCompatible(Ctx &ctx, InputFile *file, InputFile *existing) {
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

  auto diag = Err(ctx);
  diag << file << " is incompatible";
  if (existing)
    diag << " with " << existing;
  return false;
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
  // Missing subsections have zero-initialized data fields, so we must check
  // presence before comparing against GNU properties.
  bool baPauthInfoPresent = baInfo.Pauth.TagPlatform || baInfo.Pauth.TagSchema;

  if (file->aarch64PauthAbiCoreInfo) {
    // Check for data mismatch.
    if (baPauthInfoPresent &&
        (baInfo.Pauth.TagPlatform != file->aarch64PauthAbiCoreInfo->platform ||
         baInfo.Pauth.TagSchema != file->aarch64PauthAbiCoreInfo->version))
      Err(ctx) << file
               << " GNU properties and build attributes have conflicting "
                  "AArch64 PAuth data";
    if (baInfo.AndFeatures && baInfo.AndFeatures != file->andFeatures)
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
    if (baPauthInfoPresent) {
      if (baInfo.Pauth.TagPlatform == 0 && baInfo.Pauth.TagSchema == 1)
        file->aarch64PauthAbiCoreInfo = {0, 0};
      else
        file->aarch64PauthAbiCoreInfo = {baInfo.Pauth.TagPlatform,
                                         baInfo.Pauth.TagSchema};
    }
    file->andFeatures |= baInfo.AndFeatures;
  }
}

template <class ELFT>
void ObjFile<ELFT>::scanEarlySections() {
  ArrayRef<Elf_Shdr> objSections = getELFShdrs<ELFT>();
  const llvm::object::ELFFile<ELFT> obj = getObj();
  typename ELFT::SymRange eSyms = this->getELFSyms<ELFT>();
  for (size_t i = 0, size = objSections.size(); i != size; ++i) {
    const Elf_Shdr &sec = objSections[i];
    if (LLVM_LIKELY(sec.sh_type == SHT_PROGBITS))
      continue;
    if (sec.sh_type == SHT_GROUP) {
      // Tolerantly decode the group; initializeSections diagnoses. The
      // signature name (a strlen and a hash) is deferred to the caller, which
      // reuses the symbol records; validate st_name so it cannot fail there.
      Expected<ArrayRef<Elf_Word>> entries =
          obj.template getSectionContentsAsArray<Elf_Word>(sec);
      uint32_t sigSym = UINT32_MAX;
      if (!entries)
        consumeError(entries.takeError());
      else if (!entries->empty() && (*entries)[0] == Elf_Word(GRP_COMDAT) &&
               sec.sh_info < eSyms.size() &&
               eSyms[sec.sh_info].st_name < stringTable.size())
        sigSym = sec.sh_info;
      comdatSecs.push_back({(uint32_t)i, sigSym});
      continue;
    }
    if ((sec.sh_type == SHT_LLVM_DEPENDENT_LIBRARIES && !ctx.arg.relocatable) ||
        sec.sh_type == SHT_LLVM_DYNDBG_ELF ||
        (sec.sh_type == SHT_ARM_ATTRIBUTES && ctx.arg.emachine == EM_ARM))
      needsSerialScan = true;
  }
}

template <class ELFT>
void ObjFile<ELFT>::processEarlySections() {
  if (!needsSerialScan)
    return;
  object::ELFFile<ELFT> obj = this->getObj();
  ArrayRef<Elf_Shdr> objSections = getELFShdrs<ELFT>();
  StringRef shstrtab = CHECK2(obj.getSectionStringTable(objSections), this);
  for (auto [i, sec] : llvm::enumerate(objSections)) {
    if (sec.sh_type == SHT_LLVM_DEPENDENT_LIBRARIES && !ctx.arg.relocatable) {
      StringRef name = check(obj.getSectionName(sec, shstrtab));
      ArrayRef<char> data =
          CHECK2(obj.template getSectionContentsAsArray<char>(sec), this);
      if (!data.empty() && data.back() != '\0') {
        Err(ctx)
            << this
            << ": corrupted dependent libraries section (unterminated string): "
            << name;
        continue;
      }
      for (const char *d = data.begin(), *e = data.end(); d < e;) {
        StringRef s(d);
        addDependentLibrary(ctx, s, this);
        d += s.size() + 1;
      }
      continue;
    }
    if (sec.sh_type == SHT_LLVM_DYNDBG_ELF) {
      if (check(obj.getSectionName(sec, shstrtab)) == dynDbgSecName) {
        dynDbgSecIdx = i;
        dynDbgSec = std::make_unique<InputSection>(*this, sec, dynDbgSecName);
        ctx.hasDynDbg = true;
      }
      continue;
    }
    if (sec.sh_type != SHT_ARM_ATTRIBUTES || ctx.arg.emachine != EM_ARM)
      continue;
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

  // First pass over the SHT_GROUP sections scanned by scanEarlySections:
  // diagnose malformed groups and discard members of non-prevailing comdat
  // groups. Comdat group ownership was registered by the parse pipeline
  // (Pipeline::registerComdats). keptGroups memoizes the verdict for the main
  // loop below (recomputing it there would re-decode and re-hash every
  // signature). A decodable GRP_COMDAT group reuses the cached signature hash
  // and re-reads only the entries.
  SmallVector<std::pair<uint32_t, ArrayRef<Elf_Word>>, 0> keptGroups;
  for (const ComdatSec &cs : comdatSecs) {
    uint32_t i = cs.secIdx;
    const Elf_Shdr &sec = objSections[i];
    bool keepGroup;
    ArrayRef<Elf_Word> entries;
    if (cs.sigSym != UINT32_MAX) {
      entries = cantFail(
          this->getObj().template getSectionContentsAsArray<Elf_Word>(sec));
      keepGroup = ignoreComdats || cs.prevailing;
    } else {
      // Malformed or non-COMDAT group: take the diagnosing path.
      Expected<std::pair<StringRef, ArrayRef<Elf_Word>>> group = getGroup(sec);
      if (!group) {
        Err(ctx) << this << ": " << group.takeError();
        this->sections[i] = &InputSection::discarded;
        continue;
      }
      entries = group->second;
      keepGroup = !entries[0] || ignoreComdats ||
                  ctx.symtab->findComdatGroup(
                      CachedHashStringRef(group->first)) == this;
    }
    if (keepGroup) {
      keptGroups.push_back({i, entries});
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
  comdatSecs = {};

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
    if (i == dynDbgSecIdx) {
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
// COMMON, or undefined reference, with the per-symbol side effects of symbol
// resolution. Called by the parallel parse pipeline.
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

// Add the undefined symbols of the embedded unoptimized dynamic debugging
// object so that the outer link resolves the inner link's dependencies. Tag
// those reached by an inner relocation against a SHF_ALLOC section with
// `isDynDbgRef`; the rest are only needed by debug sections.
template <class ELFT>
void ObjFile<ELFT>::initDynDbgSymbols(SmallVectorImpl<Symbol *> &triggers) {
  dynDbgSymbolsAdded = true;
  MemoryBufferRef dbgMb(toStringRef(dynDbgSec->contentMaybeDecompress()),
                        mb.getBufferIdentifier());
  std::unique_ptr<ELFFileBase> efb = createObjFile(ctx, dbgMb);
  // Compare ekind (note ObjFile<ELFT>::classof only tests InputFile::kind()).
  if (efb->ekind != ekind) {
    Err(ctx) << this << ": " << dynDbgSecName
             << " contains an incompatible ELF type";
    return;
  }
  auto &dbgObj = cast<ObjFile<ELFT>>(*efb);
  const object::ELFFile<ELFT> obj = dbgObj.getObj();

  ArrayRef<Elf_Sym> dbgSyms = dbgObj.template getGlobalELFSyms<ELFT>();
  SmallVector<bool, 0> globalUsed(dbgSyms.size());
  auto setSymUsed = [&, firstGlobal = dbgObj.firstGlobal](uint32_t symIdx) {
    if (symIdx >= firstGlobal)
      globalUsed[symIdx - firstGlobal] = true;
  };

  for (const Elf_Shdr &sh : dbgObj.template getELFShdrs<ELFT>()) {
    if (!isStaticRelSecType(sh.sh_type))
      continue;
    const Elf_Shdr &target = *CHECK2(obj.getSection(sh.sh_info), &dbgObj);
    if (!(target.sh_flags & SHF_ALLOC))
      continue;
    if (sh.sh_type == SHT_CREL) {
      auto [rels, relas] = CHECK2(obj.crels(sh), &dbgObj);
      for (const Elf_Rel &r : rels)
        setSymUsed(r.getSymbol(false));
      for (const Elf_Rela &r : relas)
        setSymUsed(r.getSymbol(false));
    } else if (sh.sh_type == SHT_RELA) {
      for (const Elf_Rela &r : CHECK2(obj.relas(sh), &dbgObj))
        setSymUsed(r.getSymbol(ctx.arg.isMips64EL));
    } else {
      for (const Elf_Rel &r : CHECK2(obj.rels(sh), &dbgObj))
        setSymUsed(r.getSymbol(ctx.arg.isMips64EL));
    }
  }

  for (size_t i = 0, end = dbgSyms.size(); i != end; ++i) {
    const Elf_Sym &s = dbgSyms[i];
    if (s.st_shndx != SHN_UNDEF)
      continue;
    StringRef name = CHECK2(s.getName(dbgObj.stringTable), this);
    Symbol *sym = ctx.symtab->addSymbol(
        Undefined{this, name, s.getBinding(), s.st_other, s.getType()});
    sym->isUsedInRegularObj = true;
    sym->referenced = true;
    if (sym->isLazy() && !sym->isWeak())
      triggers.push_back(sym);
    // Undefined symbols are reported against their file's dynamic debugging
    // section. The pipeline resolves references in an order-independent way,
    // so attribute a still-undefined symbol to its first dynamic debugging
    // referrer here rather than to whichever object happened to create it.
    if (auto *und = dyn_cast<Undefined>(sym); und && !und->discardedSecIdx) {
      auto *f = dyn_cast<ObjFile<ELFT>>(sym->file);
      if (!f || !f->dynDbgSec)
        sym->file = this;
    }
    if (globalUsed[i]) {
      sym->isDynDbgRef = true;
      if (sym->traced)
        Msg(ctx) << this << ": dynamic debugging reference to " << name;
    }
  }
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

// Resolve one dynsym entry of a shared file into sym, mirroring the per-symbol
// body of SharedFile::parse. Shared by the parallel resolution and the -y
// traced replay. sym already holds the (possibly versioned) name.
template <class ELFT>
static void resolveSharedSymbol(Ctx &ctx, Symbol &sym, SharedFile &sf,
                                uint32_t elfIdx, bool isDef,
                                uint16_t versionId) {
  const typename ELFT::Sym &eSym = sf.template getELFSyms<ELFT>()[elfIdx];
  if (!isDef) {
    sym.resolve(ctx, Undefined{&sf, sym.getName(), eSym.getBinding(),
                               eSym.st_other, eSym.getType()});
    sym.isExported = true;
  } else {
    uint32_t alignment =
        getAlignment<ELFT>(sf.template getELFShdrs<ELFT>(), eSym);
    sym.resolve(ctx, SharedSymbol{sf, sym.getName(), eSym.getBinding(),
                                  eSym.st_other, eSym.getType(), eSym.st_value,
                                  eSym.st_size, alignment});
    sym.dsoDefined = true;
    if (sym.file == &sf)
      sym.versionId = versionId;
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
  case Triple::amdgpu:
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

// addComdatGroup is owner-idempotent: the parallel parse pipeline may have
// already registered this file as the owner.
void BitcodeFile::parseComdats() {
  for (std::pair<StringRef, Comdat::SelectionKind> s : obj->getComdatTable())
    keptComdats.push_back(
        s.second == Comdat::NoDeduplicate ||
        ctx.symtab->addComdatGroup(CachedHashStringRef(s.first), this) == this);
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

std::string elf::replaceThinLTOSuffix(Ctx &ctx, StringRef path) {
  auto [suffix, repl] = ctx.arg.thinLTOObjectSuffixReplace;
  if (path.consume_back(suffix))
    return (path + repl).str();
  return std::string(path);
}

//===----------------------------------------------------------------------===//
// Parallel input file parsing and symbol resolution pipeline.
//
// Pipeline::run drives one batch of input files: POD symbol records are read
// per file in parallel and counting-sorted into hash buckets, archive members
// are extracted to a fixpoint (order-independent, mold/wild-style), and
// symbols are created and resolved per bucket, then ordered by their serial
// insertion point so that the output .symtab matches the serial linker.
// Pipeline::epilogue runs the order-sensitive side effects serially.
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
  uint32_t stemLen;   // bucket key length (name minus a @@ suffix)
  uint32_t nameLen;   // full name length
  uint32_t hash;      // DenseMap hash of the stem
  uint32_t nameId;    // index into Bucket::names; written in phase 2
  uint32_t elfIdx;    // symbol index within the file's symbol/IR table
  uint16_t versionId; // shared symbol version (FShared records only)
  uint8_t flags;

  StringRef stem() const { return StringRef(name, stemLen); }
};

// POD mirror of CachedHashStringRef (default-constructible).
struct CachedName {
  const char *data;
  uint32_t size;
  uint32_t hash;
  // Index into the source file's comdatSecs (comdat signatures only).
  uint32_t srcIdx = 0;
  CachedHashStringRef ref() const {
    return CachedHashStringRef(StringRef(data, size), hash);
  }
};

struct FileData {
  SmallVector<SymRecord, 0> records; // stably bucketed by hash % numShards
  uint32_t bucketStart[numShards + 1] = {};
  // GRP_COMDAT signatures, bucketed by hash; section order within a bucket.
  SmallVector<CachedName, 0> comdats;
  uint32_t comdatStart[numShards + 1] = {};
  bool eligible = false;   // participates in the pipeline
  bool compatible = false; // passed the compatibility check
  bool dupSoname = false;  // DSO whose soname is already registered
};

// A node of a per-name singly-linked chain. Each name has two disjoint
// chains, definitions and undefined references, so one link field suffices.
struct RefNode {
  uint32_t fileIdx;
  uint32_t recIdx;
  uint32_t next = UINT32_MAX;
};

struct NameInfo {
  // The last @@-versioned spelling; serial insert() renames on each such
  // insertion.
  const char *verName = nullptr;
  Symbol *sym = nullptr;
  // Output order key: the earliest resolution event; seeds use (0, seedIdx).
  // UINT32_MAX rank means no file inserted the name, so it is dropped.
  uint64_t anchorSub = UINT64_MAX;
  uint32_t anchorRank = UINT32_MAX;
  uint32_t firstUndef = UINT32_MAX, lastUndef = UINT32_MAX;
  uint32_t firstDef = UINT32_MAX, lastDef = UINT32_MAX;
  uint32_t verNameLen = 0;
  uint32_t seedIdx = UINT32_MAX; // pre-parseFiles symVector index
  uint32_t outIdx = UINT32_MAX;  // final symVector index
};

struct Bucket {
  DenseMap<CachedHashStringRef, int> map;
  SmallVector<NameInfo, 0> names;
  SmallVector<RefNode, 0> refs;
};

// A symbol's serial insertion point, ordering the installed symbol vector.
struct OrderItem {
  uint32_t ord;
  uint64_t sub;
  uint32_t bucket, nameId;
  bool operator<(const OrderItem &o) const {
    return std::tie(ord, sub, bucket, nameId) <
           std::tie(o.ord, o.sub, o.bucket, o.nameId);
  }
};

template <class ELFT> struct Pipeline {
  Ctx &ctx;
  SmallVector<InputFile *, 0> files; // the batch to process
  SmallVector<FileData, 0> fd;
  std::array<Bucket, numShards> buckets;
  // Extractions in command-line order, for --why-extract.
  struct Extraction {
    uint32_t member, trigFile, bucket, nameId;
  };
  SmallVector<Extraction, 0> extractions;
  uint32_t bucketBase[numShards + 1]; // global name id = base[bucket] + nameId
  size_t firstObjFile = 0;            // this batch's start in ctx.objectFiles
  // A late batch (dependent libraries, LTO outputs, reactivate) extends the
  // installed symbol table in place.
  bool incremental;
  // LTO outputs are parsed with ignoreComdats: their comdat groups were already
  // resolved before LTO and must not be re-registered.
  bool ignoreComdats;
  // Reactivate: lazy symbols whose members should be extracted. activate seeds
  // these (only) as pending references, so their members are pulled in.
  ArrayRef<Symbol *> triggers;

  Pipeline(Ctx &ctx, SmallVector<InputFile *, 0> files, bool incremental,
           ArrayRef<Symbol *> triggers = {}, bool ignoreComdats = false)
      : ctx(ctx), files(std::move(files)), incremental(incremental),
        ignoreComdats(ignoreComdats), triggers(triggers) {}

  void run();
  void readSymbols();
  void readObj(uint32_t i);
  void readShared(uint32_t i);
  void readBitcode(uint32_t i);
  void buildNameDB();
  void activate();
  void registerComdats();
  void resolveSymbols();
  void resolveName(Bucket &b, uint32_t nameId);
  void applyRecord(Symbol *sym, InputFile *file, const SymRecord &rec,
                   bool isDef);
  void replayTraced(InputFile *file, const FileData &d, bool phaseSplit,
                    bool defsOnly);

  // The resolved symbol for a record, located via its hash bucket and name id.
  Symbol *symOf(const SymRecord &rec) {
    return buckets[rec.hash % numShards].names[rec.nameId].sym;
  }

  // Any record of the name (all records of a name share the stem and hash).
  const SymRecord &recOf(const Bucket &bu, const NameInfo &ni) const {
    const RefNode &node =
        bu.refs[ni.firstDef != UINT32_MAX ? ni.firstDef : ni.firstUndef];
    return fd[node.fileIdx].records[node.recIdx];
  }

  void wireSymbols();
  void recordExtractions();
  void epilogue();
  void initSections();

  void addRecord(SmallVectorImpl<SymRecord> &tmp, StringRef name,
                 uint32_t elfIdx, uint8_t flags, uint16_t versionId = 0) {
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
    r.versionId = versionId;
    r.flags = flags;
    tmp.push_back(r);
  }
};

// Stable counting sort into numShards buckets by hash, recording the bucket
// boundaries in start[].
template <class T, class HashFn>
static void bucketSort(SmallVectorImpl<T> &dst, ArrayRef<T> tmp,
                       uint32_t (&start)[numShards + 1], HashFn hash) {
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
  for (const T &r : tmp)
    dst[cursor[hash(r) % numShards]++] = r;
}

// Group the records by hash bucket for the parallel phases.
static void bucketize(FileData &d, ArrayRef<SymRecord> tmp) {
  bucketSort(d.records, tmp, d.bucketStart,
             [](const SymRecord &r) { return r.hash; });
}

static void bucketizeComdats(FileData &d, ArrayRef<CachedName> tmp) {
  bucketSort(d.comdats, tmp, d.comdatStart,
             [](const CachedName &s) { return s.hash; });
}

// Record indices in symbol table order, which the bucketing loses. Only the
// serial order-sensitive passes need it.
static SmallVector<uint32_t, 0> symbolOrder(const FileData &d) {
  SmallVector<uint32_t, 0> order;
  order.resize_for_overwrite(d.records.size());
  std::iota(order.begin(), order.end(), 0u);
  llvm::stable_sort(order, [&d](uint32_t a, uint32_t b) {
    const SymRecord &x = d.records[a], &y = d.records[b];
    // A shared file's default-versioned definition emits two records at one
    // symbol index; the unversioned name is inserted first.
    return std::make_pair(x.elfIdx, x.flags & FHasAt) <
           std::make_pair(y.elfIdx, y.flags & FHasAt);
  });
  return order;
}

} // namespace

void elf::parallelForLPT(size_t numItems,
                         llvm::function_ref<uint64_t(uint32_t)> cost,
                         llvm::function_ref<void(uint32_t)> fn) {
  SmallVector<std::pair<uint64_t, uint32_t>, 0> order;
  order.resize_for_overwrite(numItems);
  for (uint32_t i = 0; i != numItems; ++i)
    order[i] = {cost(i), i};
  llvm::stable_sort(
      order, [](const auto &a, const auto &b) { return a.first > b.first; });
  std::atomic<size_t> next{0};
  auto worker = [&]() {
    for (size_t i;
         (i = next.fetch_add(1, std::memory_order_relaxed)) < numItems;)
      fn(order[i].second);
  };
  parallel::TaskGroup tg;
  for (size_t i = 0, e = std::min<size_t>(numItems, parallel::getThreadCount());
       i != e; ++i)
    tg.spawn(worker);
}

template <class ELFT> void Pipeline<ELFT>::readSymbols() {
  fd.resize(files.size());

  // Diagnose incompatible files in command-line order. Bitcode symbol names
  // are saved here because the string savers are not thread-safe.
  InputFile *firstObj = nullptr, *firstShared = nullptr, *firstBc = nullptr;
  for (auto [i, f] : llvm::enumerate(files)) {
    InputFile *first = firstObj ? firstObj : firstShared;
    fd[i].compatible = isCompatible(ctx, f, first ? first : firstBc);
    if (!fd[i].compatible)
      continue;
    if (auto *bf = dyn_cast<BitcodeFile>(f)) {
      for (const lto::InputFile::Symbol &irSym : bf->obj->symbols())
        irSym.Name = ctx.uniqueSaver.save(irSym.getName());
      fd[i].eligible = true;
    } else if (isa<SharedFile>(f) || f->kind() == InputFile::ObjKind) {
      fd[i].eligible = true;
    }
    if (f->lazy)
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

  // Largest symbol tables first: reading a big file last leaves the other
  // workers idle for its whole duration.
  auto cost = [&](uint32_t i) -> uint64_t {
    if (!fd[i].eligible)
      return 0;
    if (auto *bf = dyn_cast<BitcodeFile>(files[i]))
      return bf->obj->symbols().size();
    return cast<ELFFileBase>(files[i])->template getELFSyms<ELFT>().size();
  };
  parallelForLPT(files.size(), cost, [&](uint32_t i) {
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

  // DSOs are uniquified by soname; a duplicate only merges isNeeded into the
  // canonical file. Registering here lets the later phases skip its records.
  for (auto [i, f] : llvm::enumerate(files)) {
    auto *sf = dyn_cast<SharedFile>(f);
    if (!sf || !fd[i].eligible)
      continue;
    auto [it, inserted] =
        ctx.symtab->soNames.try_emplace(CachedHashStringRef(sf->soName), sf);
    if (sf->isNeeded)
      it->second->isNeeded.store(true, std::memory_order_relaxed);
    if (inserted)
      continue;
    fd[i].dupSoname = true;
    fd[i].records.clear();
    memset(fd[i].bucketStart, 0, sizeof(fd[i].bucketStart));
  }
}

template <class ELFT> void Pipeline<ELFT>::readObj(uint32_t i) {
  auto *f = cast<ObjFile<ELFT>>(files[i]);
  ArrayRef<typename ELFT::Sym> eSyms = f->template getELFSyms<ELFT>();
  uint32_t firstGlobal = f->firstGlobal;
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
  if (!f->justSymbols)
    f->scanEarlySections();
  // Derive the comdat signatures. A global signature symbol reuses its record's
  // name and hash; the hash is of the stem, so a '@'-containing name rehashes.
  auto sigOf = [&](uint32_t symIdx) {
    if (uint32_t k = symIdx - firstGlobal; k < tmp.size()) {
      const SymRecord &rec = tmp[k];
      StringRef name(rec.name, rec.nameLen);
      return rec.flags & FHasAt ? CachedHashStringRef(name)
                                : CachedHashStringRef(name, rec.hash);
    }
    return CachedHashStringRef(
        StringRef(strtab.data() + eSyms[symIdx].st_name));
  };
  SmallVector<CachedName, 0> sigs;
  sigs.reserve(f->comdatSecs.size());
  for (auto [j, cs] : llvm::enumerate(f->comdatSecs)) {
    if (cs.sigSym == UINT32_MAX)
      continue;
    CachedHashStringRef sig = sigOf(cs.sigSym);
    sigs.push_back({sig.val().data(), (uint32_t)sig.val().size(), sig.hash(),
                    (uint32_t)j});
  }
  bucketizeComdats(fd[i], sigs);
  bucketize(fd[i], tmp);
}

// Read a shared file's dynamic tags and version sections and record its dynsym
// entries. Resolution runs in resolveName, registration in the epilogue.
template <class ELFT> void Pipeline<ELFT>::readShared(uint32_t i) {
  using Elf_Dyn = typename ELFT::Dyn;
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;
  using Elf_Verdef = typename ELFT::Verdef;
  using Elf_Versym = typename ELFT::Versym;
  auto *f = cast<SharedFile>(files[i]);
  const ELFFile<ELFT> obj = f->template getObj<ELFT>();
  ArrayRef<Elf_Shdr> sections = f->template getELFShdrs<ELFT>();
  const Elf_Shdr *versymSec = nullptr, *verdefSec = nullptr,
                 *verneedSec = nullptr;
  ArrayRef<Elf_Dyn> dynamicTags;
  for (const Elf_Shdr &sec : sections) {
    switch (sec.sh_type) {
    case SHT_DYNAMIC:
      dynamicTags =
          CHECK2(obj.template getSectionContentsAsArray<Elf_Dyn>(sec), f);
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

  if (versymSec && f->template getELFSyms<ELFT>().empty()) {
    ErrAlways(ctx) << "SHT_GNU_versym should be associated with symbol table";
    return;
  }

  StringRef strtab = f->getStringTable();
  // DT_SONAME (the deduplication key) and DT_NEEDED.
  for (const Elf_Dyn &dyn : dynamicTags) {
    if (dyn.d_tag == DT_NEEDED) {
      uint64_t val = dyn.getVal();
      if (val >= strtab.size()) {
        Err(ctx) << f << ": invalid DT_NEEDED entry";
        return;
      }
      f->dtNeeded.push_back(strtab.data() + val);
    } else if (dyn.d_tag == DT_SONAME) {
      uint64_t val = dyn.getVal();
      if (val >= strtab.size()) {
        Err(ctx) << f << ": invalid DT_SONAME entry";
        return;
      }
      f->soName = strtab.data() + val;
    }
  }

  f->verdefs = parseVerdefs<ELFT>(obj.base(), verdefSec);
  std::vector<uint32_t> verneeds =
      f->template parseVerneed<ELFT>(obj, verneedSec);

  uint32_t firstGlobal = f->firstGlobal;
  size_t size = f->template getELFSyms<ELFT>().size() - firstGlobal;
  std::vector<uint16_t> versyms(size, VER_NDX_GLOBAL);
  if (versymSec && size) {
    ArrayRef<Elf_Versym> v =
        CHECK2(obj.template getSectionContentsAsArray<Elf_Versym>(*versymSec),
               f)
            .slice(firstGlobal);
    for (size_t j = 0; j < size; ++j)
      versyms[j] = v[j].vs_index;
  }

  // Versioned names (foo@ver) are built in the thread-local arena so they
  // outlive this parallel phase without touching the shared string saver.
  auto saveVersioned = [](StringRef name, StringRef ver) {
    size_t n = name.size() + 1 + ver.size();
    char *buf = makeThreadLocalN<char>(n);
    memcpy(buf, name.data(), name.size());
    buf[name.size()] = '@';
    memcpy(buf + name.size() + 1, ver.data(), ver.size());
    return StringRef(buf, n);
  };

  ArrayRef<Elf_Sym> syms = f->template getGlobalELFSyms<ELFT>();
  SmallVector<SymRecord, 0> tmp;
  tmp.reserve(syms.size());
  for (size_t j = 0, e = syms.size(); j != e; ++j) {
    const Elf_Sym &sym = syms[j];
    StringRef name = CHECK2(sym.getName(strtab), f);
    if (sym.getBinding() == STB_LOCAL) {
      Err(ctx) << f << ": invalid local symbol '" << name
               << "' in global part of symbol table";
      continue;
    }
    uint32_t elfIdx = firstGlobal + j;
    const uint16_t ver = versyms[j], idx = ver & ~VERSYM_HIDDEN;
    uint8_t base = FShared | (sym.getBinding() == STB_WEAK ? FWeak : 0);

    if (sym.isUndefined()) {
      // Index 0 (VER_NDX_LOCAL) is used for unversioned undefined symbols. GNU
      // ld versions between 2.35 and 2.45 also generate VER_NDX_GLOBAL for
      // this case (https://sourceware.org/PR33577).
      if (ver != VER_NDX_LOCAL && ver != VER_NDX_GLOBAL) {
        if (idx >= verneeds.size()) {
          ErrAlways(ctx) << "corrupt input file: version need index " << idx
                         << " for symbol " << name
                         << " is out of bounds\n>>> defined in " << f;
          continue;
        }
        name = saveVersioned(name, strtab.data() + verneeds[idx]);
      }
      addRecord(tmp, name, elfIdx, base);
      continue;
    }

    if (ver == VER_NDX_LOCAL ||
        (ver != VER_NDX_GLOBAL && idx >= f->verdefs.size())) {
      // In GNU ld < 2.31 the MIPS port put _gp_disp with VER_NDX_LOCAL.
      if (ctx.arg.emachine == EM_MIPS && name == "_gp_disp")
        continue;
      ErrAlways(ctx) << "corrupt input file: version definition index " << idx
                     << " for symbol " << name
                     << " is out of bounds\n>>> defined in " << f;
      continue;
    }

    if (ver == idx)
      addRecord(tmp, name, elfIdx, base | FDef, ver);

    // Also register the versioned name to satisfy explicitly versioned refs.
    if (ver == VER_NDX_GLOBAL)
      continue;
    StringRef verName =
        strtab.data() + reinterpret_cast<const Elf_Verdef *>(f->verdefs[idx])
                            ->getAux()
                            ->vda_name;
    addRecord(tmp, saveVersioned(name, verName), elfIdx, base | FDef, idx);
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
  SmallVector<CachedName, 0> sigs;
  for (auto s : f->obj->getComdatTable())
    if (s.second != Comdat::NoDeduplicate)
      sigs.push_back({s.first.data(), (uint32_t)s.first.size(),
                      CachedHashStringRef(s.first).hash()});
  bucketizeComdats(fd[i], sigs);
}

template <class ELFT> void Pipeline<ELFT>::buildNameDB() {
  // Each bucket draws its seeds from the shard of the same index: shard routing
  // and bucket routing share the name hash, and both key by the stem. A first
  // batch replaces the symbol table, so it copies the whole shard; a late batch
  // only extends it, so it looks up the names it actually sees.
  ArrayRef<Symbol *> symVec = ctx.symtab->getSymbols();
  size_t total = 0;
  uint32_t bucketTotals[numShards] = {};
  for (const FileData &d : fd) {
    total += d.records.size();
    for (uint32_t b = 0; b != numShards; ++b)
      bucketTotals[b] += d.bucketStart[b + 1] - d.bucketStart[b];
  }

  parallelFor(0, numShards, [&](size_t b) {
    Bucket &bu = buckets[b];
    const auto &shard = ctx.symtab->getShards()[b];
    // Shard loads deviate several percent from the mean, so size refs exactly
    // rather than paying a mid-build reallocation.
    size_t seeds = incremental ? 0 : shard.size();
    bu.names.reserve(total / numShards / 4 + seeds);
    bu.refs.reserve(bucketTotals[b]);
    bu.map.reserve(total / numShards / 4 + seeds);
    if (!incremental)
      for (const auto &kv : shard) {
        bu.map.try_emplace(kv.first, bu.names.size());
        NameInfo &ni = bu.names.emplace_back();
        ni.seedIdx = kv.second;
        ni.sym = symVec[kv.second];
      }
    auto append = [&bu](uint32_t &first, uint32_t &last, uint32_t idx) {
      if (first == UINT32_MAX)
        first = idx;
      else
        bu.refs[last].next = idx;
      last = idx;
    };
    for (auto [i, d] : llvm::enumerate(fd)) {
      for (uint32_t r = d.bucketStart[b], e = d.bucketStart[b + 1]; r != e;
           ++r) {
        SymRecord &rec = d.records[r];
        CachedHashStringRef key(rec.stem(), rec.hash);
        auto [it, inserted] = bu.map.try_emplace(key, bu.names.size());
        if (inserted) {
          NameInfo &n = bu.names.emplace_back();
          if (incremental) {
            if (auto sit = shard.find(key); sit != shard.end()) {
              n.seedIdx = sit->second;
              n.sym = symVec[sit->second];
            }
          }
        }
        uint32_t nameId = it->second;
        rec.nameId = nameId;
        NameInfo &ni = bu.names[nameId];
        uint32_t refIdx = bu.refs.size();
        RefNode &node = bu.refs.emplace_back();
        node.fileIdx = i;
        node.recIdx = r;
        if (rec.flags & FDef)
          append(ni.firstDef, ni.lastDef, refIdx);
        else
          append(ni.firstUndef, ni.lastUndef, refIdx);
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
  // A lazy member is pulled in if a non-weak undefined reference anywhere names
  // a symbol whose first definition is that member, to a fixpoint. Determinism
  // comes from file index, not scan order.
  uint32_t numNames = bucketBase[numShards];
  const bool fortranCommon = ctx.arg.fortranCommon;
  auto globalId = [&](const SymRecord &rec) {
    return bucketBase[rec.hash % numShards] + rec.nameId;
  };
  // Snapshot the lazy flags: extraction clears files[i]->lazy, but the
  // decisions must stay a function of the pre-activation state.
  SmallVector<uint8_t, 0> wasLazy(files.size());
  for (auto [i, f] : llvm::enumerate(files))
    wasLazy[i] = f->lazy;

  // Walk a name's definition chain, summarizing each tier (0 = strong/regular,
  // 1 = weak/tentative) and the first lazy non-tentative definition (the
  // --fortran-common override target).
  struct DefInfo {
    uint32_t firstFile[2] = {UINT32_MAX, UINT32_MAX};
    uint32_t lazyStrongDef = UINT32_MAX;
    bool firstLazy[2] = {false, false};
    bool eagerReg[2] = {false, false};
    bool eagerCommon = false; // an eager or extracted file defines it as COMMON
  };
  auto summarize = [&](uint32_t b, uint32_t nameId) {
    DefInfo di;
    const Bucket &bu = buckets[b];
    // A late batch does not contain the files parsed before it; the already
    // resolved symbol stands in for their definitions.
    const Symbol *seed = bu.names[nameId].seedIdx == UINT32_MAX
                             ? nullptr
                             : bu.names[nameId].sym;
    if (seed && (seed->isDefined() || seed->isCommon() || seed->isShared())) {
      unsigned t = seed->isWeak() || seed->isCommon() ? 1 : 0;
      di.firstFile[t] = files.size();
      di.eagerReg[t] = !seed->isShared();
      di.eagerCommon = seed->isCommon();
    }
    for (uint32_t r = bu.names[nameId].firstDef; r != UINT32_MAX;
         r = bu.refs[r].next) {
      const RefNode &node = bu.refs[r];
      const SymRecord &rec = fd[node.fileIdx].records[node.recIdx];
      bool lazy = wasLazy[node.fileIdx];
      unsigned t = (rec.flags & (FWeak | FCommon)) ? 1 : 0;
      if (di.firstFile[t] == UINT32_MAX) {
        di.firstFile[t] = node.fileIdx;
        di.firstLazy[t] = lazy;
      }
      if (!lazy && !(rec.flags & FShared))
        di.eagerReg[t] = true;
      // --fortran-common: a COMMON is active once its file is eager or
      // extracted; only a STB_GLOBAL non-tentative definition overrides it.
      if (rec.flags & FCommon)
        di.eagerCommon |= !files[node.fileIdx]->lazy;
      else if (lazy && di.lazyStrongDef == UINT32_MAX && !(rec.flags & FWeak))
        di.lazyStrongDef = node.fileIdx;
    }
    return di;
  };
  // The lazy member to pull in when the name is referenced. A reference
  // resolves in its strongest tier; within that tier an eager regular
  // definition wins and suppresses extraction, otherwise the first-seen
  // definition wins. This matches mold/ld.bfd.
  auto extractTarget = [](const DefInfo &di) -> uint32_t {
    unsigned t = di.firstFile[0] != UINT32_MAX ? 0 : 1;
    if (di.firstFile[t] != UINT32_MAX && !di.eagerReg[t] && di.firstLazy[t])
      return di.firstFile[t];
    return UINT32_MAX;
  };

  // Worklist of names referenced by a live non-weak undefined symbol. Each name
  // is resolved once; if unsatisfied it extracts its first-seen lazy member,
  // whose own references join the worklist. trig records the first referrer:
  // UINT32_MAX until the name is queued, and files.size() for ctx.internalFile.
  SmallVector<uint32_t, 0> trig(numNames, UINT32_MAX);
  struct WorkItem {
    uint32_t bucket, nameId;
  };
  SmallVector<WorkItem, 0> work;
  auto pushName = [&](uint32_t id, uint32_t bucket, uint32_t nameId,
                      uint32_t file) {
    if (trig[id] == UINT32_MAX) {
      trig[id] = file;
      work.push_back({bucket, nameId});
    }
  };
  auto seedKey = [&](CachedHashStringRef key) {
    uint32_t b = key.hash() % numShards;
    auto it = buckets[b].map.find(key);
    if (it != buckets[b].map.end())
      pushName(bucketBase[b] + it->second, b, it->second,
               (uint32_t)files.size());
  };
  if (!triggers.empty()) {
    // A reactivate batch pulls in only the members defining the triggers.
    for (Symbol *t : triggers) {
      StringRef stem =
          t->getName().take_front(getSymbolStem(t->getName()).first);
      seedKey(CachedHashStringRef(stem));
    }
  } else {
    // Already resolved non-weak undefined references (e.g. -u) and every
    // non-weak undefined reference of an eager (non-lazy) file.
    for (uint32_t b = 0; b != numShards; ++b)
      for (auto [id, ni] : llvm::enumerate(buckets[b].names))
        if (ni.seedIdx != UINT32_MAX && ni.sym->isUndefined() &&
            !ni.sym->isWeak())
          pushName(bucketBase[b] + id, b, id, (uint32_t)files.size());
    for (auto [i, f] : llvm::enumerate(files)) {
      if (!fd[i].compatible || f->lazy)
        continue;
      for (const SymRecord &rec : fd[i].records)
        if (!(rec.flags & (FDef | FWeak)) ||
            (fortranCommon && (rec.flags & FCommon)))
          pushName(globalId(rec), rec.hash % numShards, rec.nameId, i);
    }
  }

  while (!work.empty()) {
    auto [b, nameId] = work.pop_back_val();
    uint32_t id = bucketBase[b] + nameId;
    DefInfo di = summarize(b, nameId);
    uint32_t m = extractTarget(di);
    // --fortran-common: a lazy non-tentative definition overrides an active
    // COMMON that nothing else displaces.
    if (fortranCommon && m == UINT32_MAX && di.eagerCommon &&
        di.lazyStrongDef != UINT32_MAX)
      m = di.lazyStrongDef;
    if (m == UINT32_MAX || !files[m]->lazy)
      continue; // satisfied, no lazy definition, or already extracted
    files[m]->lazy = false;
    extractions.push_back({m, trig[id], b, nameId});
    for (const SymRecord &rec : fd[m].records)
      if (!(rec.flags & (FDef | FWeak)) ||
          (fortranCommon && (rec.flags & FCommon)))
        pushName(globalId(rec), rec.hash % numShards, rec.nameId, m);
  }
}

template <class ELFT> void Pipeline<ELFT>::registerComdats() {
  if (ignoreComdats)
    return;
  // First-parsed file in serial order owns each comdat group.
  llvm::TimeTraceScope comdatScope("Pre-populate comdat groups");
  size_t numComdats = 0;
  for (const FileData &d : fd)
    numComdats += d.comdats.size();
  parallelFor(0, numShards, [&](size_t s) {
    ctx.symtab->comdatGroups[s].reserve(numComdats / numShards / 2);
    for (auto [fi, f] : llvm::enumerate(files)) {
      if (!fd[fi].compatible || f->lazy)
        continue;
      const FileData &d = fd[fi];
      // Record the verdict so that initializeSections needs no lookup. Comdat
      // entries are sharded by hash, so writes to comdatSecs are disjoint.
      auto *obj =
          f->kind() == InputFile::ObjKind ? cast<ObjFile<ELFT>>(f) : nullptr;
      for (uint32_t i = d.comdatStart[s], e = d.comdatStart[s + 1]; i != e;
           ++i) {
        const CachedName &cn = d.comdats[i];
        if (ctx.symtab->addComdatGroup(cn.ref(), f) != f && obj)
          obj->comdatSecs[cn.srcIdx].prevailing = 0;
      }
    }
  });
}

// The sole symbol-resolution dispatch, shared by the parallel resolveName and
// the serial -y traced replay, so the two cannot diverge.
template <class ELFT>
void Pipeline<ELFT>::applyRecord(Symbol *sym, InputFile *file,
                                 const SymRecord &rec, bool isDef) {
  if (rec.flags & FShared) {
    resolveSharedSymbol<ELFT>(ctx, *sym, *cast<SharedFile>(file), rec.elfIdx,
                              isDef, rec.versionId);
  } else if (file->lazy) {
    // An unextracted lazy file contributes only LazySymbol definitions; an
    // earlier shared or bitcode definition suppresses them.
    sym->resolve(ctx, LazySymbol{*file});
  } else if (rec.flags & FBitcode) {
    auto *bf = cast<BitcodeFile>(file);
    createBitcodeSymbol(ctx, sym, bf->obj->symbols()[rec.elfIdx], *bf);
  } else {
    auto *obj = cast<ObjFile<ELFT>>(file);
    resolveSymbol(ctx, obj, obj->template getELFSyms<ELFT>()[rec.elfIdx], *sym);
  }
}

// Replay resolution events for -y traced names in symbol-table order, deferred
// by resolveName so that the trace output is deterministic. phaseSplit visits
// definitions first; defsOnly restricts to definitions.
template <class ELFT>
void Pipeline<ELFT>::replayTraced(InputFile *file, const FileData &d,
                                  bool phaseSplit, bool defsOnly) {
  SmallVector<uint32_t, 0> order = symbolOrder(d);
  for (int phase = 0, end = phaseSplit ? 2 : 1; phase != end; ++phase)
    for (uint32_t ri : order) {
      const SymRecord &rec = d.records[ri];
      bool isDef = rec.flags & FDef;
      if ((defsOnly && !isDef) || (phaseSplit && (phase == 0) != isDef))
        continue;
      Symbol *sym = symOf(rec);
      if (sym->traced)
        applyRecord(sym, file, rec, isDef);
    }
}

template <class ELFT>
void Pipeline<ELFT>::resolveName(Bucket &bu, uint32_t nameId) {
  NameInfo &ni = bu.names[nameId];
  Symbol *sym = ni.sym;

  // Apply the @@ rename and default flags. Serial insert() renames the symbol
  // on every versioned insertion and flags any name containing '@'.
  if (ni.verName) {
    sym->setName(StringRef(ni.verName, ni.verNameLen));
    sym->hasVersionSuffix = true;
  } else if (ni.seedIdx == UINT32_MAX && (recOf(bu, ni).flags & FHasAt)) {
    sym->hasVersionSuffix = true;
  }

  // Merge the definition and undefined-reference chains into one event stream
  // ordered by (file, definitions first, symbol index), matching the serial
  // linker. The output anchor ignores the phase: the file that first inserts
  // the symbol.
  uint32_t anchorRank = ni.seedIdx == UINT32_MAX ? UINT32_MAX : 0;
  uint64_t anchorSub = ni.seedIdx == UINT32_MAX ? UINT64_MAX : ni.seedIdx;
  const bool traced = sym->traced;
  bool inserted = ni.seedIdx != UINT32_MAX;
  auto key = [&](uint32_t r, uint64_t undefPhase) {
    const RefNode &node = bu.refs[r];
    return (uint64_t(node.fileIdx + 1) << 33) | (undefPhase << 32) |
           fd[node.fileIdx].records[node.recIdx].elfIdx;
  };
  uint32_t dIt = ni.firstDef, uIt = ni.firstUndef;
  for (;;) {
    // An unextracted lazy file never inserts its undefined references.
    while (uIt != UINT32_MAX && files[bu.refs[uIt].fileIdx]->lazy)
      uIt = bu.refs[uIt].next;
    if (dIt == UINT32_MAX && uIt == UINT32_MAX)
      break;
    bool isDef =
        uIt == UINT32_MAX || (dIt != UINT32_MAX && key(dIt, 0) < key(uIt, 1));
    uint32_t r = isDef ? dIt : uIt;
    (isDef ? dIt : uIt) = bu.refs[r].next;
    const RefNode &node = bu.refs[r];
    uint32_t f = node.fileIdx;
    const SymRecord &rec = fd[f].records[node.recIdx];
    uint32_t rank = f + 1;
    // Sub-order within a file: a bitcode file inserts every definition before
    // its undefined references; a shared file inserts a symbol's unversioned
    // name before its versioned one ('@').
    uint64_t asub;
    if (rec.flags & FBitcode)
      asub = (uint64_t(!isDef) << 32) | rec.elfIdx;
    else if (rec.flags & FShared)
      asub = (uint64_t(rec.elfIdx) << 1) | bool(rec.flags & FHasAt);
    else
      asub = rec.elfIdx;
    if (rank < anchorRank || (rank == anchorRank && asub < anchorSub)) {
      anchorRank = rank;
      anchorSub = asub;
    }
    if (traced)
      continue;
    inserted = true;
    applyRecord(sym, files[f], rec, isDef);
  }
  ni.anchorRank = anchorRank;
  ni.anchorSub = anchorSub;
  if (!traced && !inserted)
    ni.anchorRank = UINT32_MAX;
}

template <class ELFT> void Pipeline<ELFT>::resolveSymbols() {
  // A late batch extends an already-installed symbol table: seeds are resolved
  // in place and only the new names are ordered and appended.
  auto isLive = [&](const NameInfo &ni) {
    return ni.anchorRank != UINT32_MAX &&
           !(incremental && ni.seedIdx != UINT32_MAX);
  };

  uint32_t counts[numShards];
  {
    llvm::TimeTraceScope scope1("Resolve buckets");
    parallelFor(0, numShards, [&](size_t b) {
      Bucket &bu = buckets[b];
      size_t n = bu.names.size();
      counts[b] = 0;
      if (!n)
        return;
      SymbolUnion *storage = makeThreadLocalN<SymbolUnion>(n);
      uint32_t live = 0;
      for (size_t id = 0; id != n; ++id) {
        NameInfo &ni = bu.names[id];
        if (ni.seedIdx != UINT32_MAX) {
          // A late batch resolves a pre-existing symbol in place so that
          // references from already-parsed files stay valid.
          if (!incremental) {
            SymbolUnion *su = &storage[id];
            memcpy(static_cast<void *>(su), ni.sym, sizeof(SymbolUnion));
            ni.sym = reinterpret_cast<Symbol *>(su);
          }
        } else {
          SymbolUnion *su = &storage[id];
          memset(static_cast<void *>(su), 0, sizeof(SymbolUnion));
          auto *s = reinterpret_cast<Symbol *>(su);
          // The map key (any record's stem) is the initial name.
          s->versionId = VER_NDX_GLOBAL;
          s->setName(recOf(bu, ni).stem());
          ni.sym = reinterpret_cast<Symbol *>(su);
        }
        resolveName(bu, id);
        live += isLive(bu.names[id]);
      }
      counts[b] = live;
    });
  }

  llvm::TimeTraceScope scope2("Order symbols");
  // Order the live symbols by their serial insertion point and move them into
  // the final storage. Names that no serial insertion event would have created
  // are dropped before the sort.
  uint32_t liveStart[numShards + 1];
  uint32_t sum = 0;
  for (uint32_t b = 0; b != numShards; ++b) {
    liveStart[b] = sum;
    sum += counts[b];
  }
  liveStart[numShards] = sum;
  size_t live = sum;
  SmallVector<OrderItem, 0> items;
  items.resize_for_overwrite(live);
  parallelFor(0, numShards, [&](size_t b) {
    Bucket &bu = buckets[b];
    OrderItem *out = items.begin() + liveStart[b];
    for (auto [id, ni] : llvm::enumerate(bu.names))
      if (isLive(ni))
        *out++ = {ni.anchorRank, ni.anchorSub, (uint32_t)b, (uint32_t)id};
  });
  parallelSort(items.begin(), items.end());

  SymbolUnion *out = getSpecificAllocSingleton<SymbolUnion>().Allocate(live);
  if (incremental) {
    // Seeds were resolved in place; append the new names, registering each in
    // its hash shard.
    for (auto [i, it] : llvm::enumerate(items)) {
      Bucket &bu = buckets[it.bucket];
      NameInfo &ni = bu.names[it.nameId];
      memcpy(static_cast<void *>(&out[i]), ni.sym, sizeof(SymbolUnion));
      ni.sym = reinterpret_cast<Symbol *>(&out[i]);
      const SymRecord &rec = recOf(bu, ni);
      ctx.symtab->appendShardedSymbol(CachedHashStringRef(rec.stem(), rec.hash),
                                      ni.sym);
    }
    recordExtractions();
    return;
  }

  SmallVector<Symbol *, 0> symVector(live);
  parallelFor(0, live, [&](size_t i) {
    const OrderItem &it = items[i];
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
        const SymRecord &rec = recOf(bu, ni);
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
  if (ctx.arg.whyExtract.empty())
    return;
  for (const Extraction &ex : extractions) {
    Symbol *sym = buckets[ex.bucket].names[ex.nameId].sym;
    InputFile *trigger =
        ex.trigFile == files.size() ? ctx.internalFile : files[ex.trigFile];
    ctx.whyExtractRecords.emplace_back(toStr(ctx, trigger), files[ex.member],
                                       *sym);
  }
}

template <class ELFT> void Pipeline<ELFT>::wireSymbols() {
  auto cost = [&](uint32_t i) -> uint64_t { return fd[i].records.size(); };
  parallelForLPT(files.size(), cost, [&](uint32_t i) {
    if (!fd[i].eligible)
      return;
    InputFile *f = files[i];
    if (f->kind() == InputFile::ObjKind) {
      bool inactiveLazy = f->lazy;
      f->allocateSymbols();
      MutableArrayRef<Symbol *> syms = f->getMutableSymbols();
      for (const SymRecord &rec : fd[i].records) {
        // An unextracted lazy file has only its definitions wired.
        if (inactiveLazy && !(rec.flags & FDef))
          continue;
        Bucket &bu = buckets[rec.hash % numShards];
        syms[rec.elfIdx] = bu.names[rec.nameId].sym;
      }
    } else if (f->kind() == InputFile::BitcodeKind) {
      // resolveName resolved the bitcode symbols in place; wire the array.
      auto *bf = cast<BitcodeFile>(f);
      bool inactiveLazy = f->lazy;
      bf->allocateSymbols(bf->obj->symbols().size());
      MutableArrayRef<Symbol *> syms = bf->getMutableSymbols();
      for (const SymRecord &rec : fd[i].records) {
        if (inactiveLazy && !(rec.flags & FDef))
          continue;
        syms[rec.elfIdx] = symOf(rec);
      }
    } else if (f->kind() == InputFile::SharedKind &&
               ctx.arg.unresolvedSymbolsInShlib != UnresolvedPolicy::Ignore) {
      // Collect the DSO's strong undefined references for
      // reportUndefinedSymbols.
      auto *sf = cast<SharedFile>(f);
      for (uint32_t ri : symbolOrder(fd[i])) {
        const SymRecord &rec = fd[i].records[ri];
        if (!(rec.flags & (FDef | FWeak)))
          sf->requiredSymbols.push_back(symOf(rec));
      }
    }
  });
}

template <class ELFT> void Pipeline<ELFT>::epilogue() {
  // Run the order-sensitive side effects in command-line order: file
  // registration (which determines output section order), dependent libraries,
  // ARM attributes and -y traced replay.
  firstObjFile = ctx.objectFiles.size();
  for (auto [i, f] : llvm::enumerate(files)) {
    if (!fd[i].compatible)
      continue;
    FileData &d = fd[i];
    if (f->lazy) {
      if (auto *bf = dyn_cast<BitcodeFile>(f)) {
        // Unlike a lazy object, parseLazy has no early exit, so all
        // definitions are visible.
        ctx.lazyBitcodeFiles.push_back(bf);
        if (ctx.symtab->hasTracedSymbol)
          replayTraced(f, d, /*phaseSplit=*/false, /*defsOnly=*/true);
        continue;
      }
      if (ctx.symtab->hasTracedSymbol && f->kind() == InputFile::ObjKind)
        replayTraced(f, d, /*phaseSplit=*/false, /*defsOnly=*/true);
      continue;
    }
    // -t traces the input files and extracted members, but not LTO outputs.
    if (ctx.arg.trace && !ignoreComdats)
      Msg(ctx) << f;
    if (auto *bf = dyn_cast<BitcodeFile>(f)) {
      ctx.bitcodeFiles.push_back(bf);
      bf->parseComdats();
      if (ctx.symtab->hasTracedSymbol)
        replayTraced(f, d, /*phaseSplit=*/true, /*defsOnly=*/false);
      for (auto l : bf->obj->getDependentLibraries())
        addDependentLibrary(ctx, l, bf);
      continue;
    }
    if (auto *sf = dyn_cast<SharedFile>(f)) {
      if (d.dupSoname)
        continue;
      ctx.sharedFiles.push_back(sf);
      sf->allocateSymbols();
      sf->parseGnuAndFeatures<ELFT>(sf->getObj<ELFT>());
      if (ctx.symtab->hasTracedSymbol)
        replayTraced(f, d, /*phaseSplit=*/false, /*defsOnly=*/false);
      continue;
    }
    if (auto *bin = dyn_cast<BinaryFile>(f)) {
      // A binary blob defines _binary_<name>_{start,end,size} directly.
      ctx.binaryFiles.push_back(bin);
      bin->parse();
      continue;
    }
    auto *obj = cast<ObjFile<ELFT>>(f);
    ctx.objectFiles.push_back(obj);
    if (ctx.symtab->hasTracedSymbol)
      replayTraced(f, d, /*phaseSplit=*/true, /*defsOnly=*/false);
    obj->processEarlySections();
  }
}

template <class ELFT> void Pipeline<ELFT>::initSections() {
  // Runs after the epilogue, so that comdat group ownership and
  // ctx.in.attributes are final, and after the phase-local data is freed.
  ArrayRef<ELFFileBase *> objs = ArrayRef(ctx.objectFiles).slice(firstObjFile);
  auto cost = [&](uint32_t i) -> uint64_t {
    auto *f = cast<ObjFile<ELFT>>(objs[i]);
    return f->template getELFShdrs<ELFT>().size() + f->firstGlobal;
  };
  parallelForLPT(objs.size(), cost, [&](uint32_t i) {
    cast<ObjFile<ELFT>>(objs[i])->initSectionsAndLocalSyms(ignoreComdats);
  });
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
  registerComdats();
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
  {
    llvm::TimeTraceScope timeScope("Initialize sections");
    initSections();
  }
}

template <class ELFT>
static void runPipeline(Ctx &ctx, SmallVector<InputFile *, 0> files,
                        bool incremental, ArrayRef<Symbol *> triggers = {},
                        bool ignoreComdats = false) {
  Pipeline<ELFT> p(ctx, std::move(files), incremental, triggers, ignoreComdats);
  p.run();

  // The undefined symbols of newly parsed dynamic debugging objects are added
  // after the batch, like -u; their lazy definitions are extracted by a
  // reactivation pass, whose own batch takes care of any further such objects.
  if (!ctx.hasDynDbg)
    return;
  SmallVector<Symbol *, 0> dynDbgTriggers;
  for (ELFFileBase *f : ctx.objectFiles) {
    auto *obj = cast<ObjFile<ELFT>>(f);
    if (obj->dynDbgSec && !obj->dynDbgSymbolsAdded)
      obj->initDynDbgSymbols(dynDbgTriggers);
  }
  reactivate(ctx, dynDbgTriggers);
}

template <class ELFT>
static void
doParseFiles(Ctx &ctx,
             const SmallVector<std::unique_ptr<InputFile>, 0> &files) {
  // Parsing may append files (addDependentLibrary); a new batch seeds its name
  // database from the symbols resolved so far.
  for (size_t done = 0; done < files.size();) {
    size_t end = files.size();
    SmallVector<InputFile *, 0> batch;
    batch.reserve(end - done);
    for (size_t i = done; i != end; ++i)
      batch.push_back(files[i].get());
    runPipeline<ELFT>(ctx, std::move(batch), /*incremental=*/done != 0);
    done = end;
  }
  if (ctx.driver.armCmseImpLib)
    cast<ObjFile<ELFT>>(*ctx.driver.armCmseImpLib).importCmseSymbols();
}

void elf::parseFiles(Ctx &ctx,
                     const SmallVector<std::unique_ptr<InputFile>, 0> &files) {
  llvm::TimeTraceScope timeScope("Parse input files");
  invokeELFT(doParseFiles, ctx, files);
}

void elf::parseLtoObjectFiles(Ctx &ctx, ArrayRef<InputFile *> files) {
  if (files.empty())
    return;
  invokeELFT(runPipeline, ctx,
             SmallVector<InputFile *, 0>(files.begin(), files.end()),
             /*incremental=*/true, /*triggers=*/ArrayRef<Symbol *>(),
             /*ignoreComdats=*/true);

  // An LTO output may reference a runtime libcall defined in an archive member
  // not loaded before LTO; extract such members.
  SmallVector<Symbol *, 0> triggers;
  for (InputFile *f : files)
    for (Symbol *sym : cast<ELFFileBase>(f)->getGlobalSymbols())
      if (sym && sym->isLazy() && !sym->isWeak())
        triggers.push_back(sym);
  reactivate(ctx, triggers);
}

void elf::reactivate(Ctx &ctx, ArrayRef<Symbol *> triggers) {
  if (triggers.empty())
    return;
  SmallVector<InputFile *, 0> lazyFiles;
  for (auto &f : ctx.driver.getFiles())
    if (f->lazy)
      lazyFiles.push_back(f.get());
  if (lazyFiles.empty())
    return;
  invokeELFT(runPipeline, ctx, std::move(lazyFiles), /*incremental=*/true,
             triggers);
}

template class elf::ObjFile<ELF32LE>;
template class elf::ObjFile<ELF32BE>;
template class elf::ObjFile<ELF64LE>;
template class elf::ObjFile<ELF64BE>;

