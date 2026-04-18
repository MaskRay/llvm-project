#ifndef LLVM_DWP_DWP_H
#define LLVM_DWP_DWP_H

#include "DWPStringPool.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFUnitIndex.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"
#include <deque>
#include <vector>

namespace llvm {
class raw_pwrite_stream;

enum OnCuIndexOverflow {
  HardStop,
  SoftStop,
  Continue,
};

enum Dwarf64StrOffsetsPromotion {
  Disabled, ///< Don't do any conversion of .debug_str_offsets tables.
  Enabled,  ///< Convert any .debug_str_offsets tables to DWARF64 if needed.
  Always,   ///< Always emit .debug_str_offsets talbes as DWARF64 for testing.
};

/// Section identifiers for DWP output.
enum DWPSectionId : unsigned {
  DS_Info,
  DS_Types,
  DS_Abbrev,
  DS_Line,
  DS_Loc,
  DS_Loclists,
  DS_Rnglists,
  DS_Macro,
  DS_Str,
  DS_StrOffsets,
  DS_CUIndex,
  DS_TUIndex,
  DS_NumSections
};

/// Direct ELF writer for DWP output, bypassing MCStreamer.
///
/// Section data is stored as zero-copy StringRef chunks pointing to the
/// mmap'd input files, plus an inline buffer for constructed data
/// (emitIntValue). This avoids copying gigabytes of debug section data
/// through the MC infrastructure (MCContext, MCAssembler, MCDataFragment
/// allocation, layout, etc.).
class LLVM_ABI DWPWriter {
public:
  /// Per-section storage. Chunks are stored in emission order so callers
  /// that interleave emitBytes()/emitIntValue() (writeStringsAndOffsets,
  /// writeIndex) produce bytes in the correct order. Each chunk is either
  /// a borrowed StringRef into an input mmap or a slice of OwnedBytes.
  struct SectionData {
    enum Kind : uint8_t { Borrowed, Owned };
    struct Chunk {
      Kind K;
      StringRef Borrowed;  // when K == Borrowed
      uint32_t OwnedBegin; // when K == Owned
      uint32_t OwnedLen;
    };
    SmallVector<Chunk, 4> Chunks;
    SmallVector<char, 0> OwnedBytes;

    uint64_t totalSize() const {
      uint64_t Size = 0;
      for (const Chunk &C : Chunks)
        Size += C.K == Borrowed ? C.Borrowed.size() : C.OwnedLen;
      return Size;
    }

    bool empty() const { return Chunks.empty(); }

    void appendBorrowed(StringRef Data) {
      if (!Data.empty())
        Chunks.push_back({Borrowed, Data, 0, 0});
    }

    void appendOwned(const char *Data, uint32_t Len) {
      if (!Len)
        return;
      uint32_t Begin = OwnedBytes.size();
      OwnedBytes.append(Data, Data + Len);
      if (!Chunks.empty() && Chunks.back().K == Owned &&
          Chunks.back().OwnedBegin + Chunks.back().OwnedLen == Begin) {
        Chunks.back().OwnedLen += Len;
      } else {
        Chunks.push_back({Owned, StringRef(), Begin, Len});
      }
    }
  };

private:
  SectionData Sections[DS_NumSections];
  /// Storage for DWPStringPool. Kept separate so the pool has a stable
  /// SmallVectorImpl<char>& to append into; handed off to DS_Str at write
  /// time via a single Borrowed chunk.
  SmallVector<char, 0> StrPoolStorage;
  DWPSectionId CurrentSection = DS_Info;
  uint16_t ELFMachine = 0;
  uint8_t ELFOSABI = 0;
  bool Is64Bit = true;
  bool IsLittleEndian = true;

public:
  DWPWriter() = default;

  void setMachine(uint16_t Machine) { ELFMachine = Machine; }
  void setOSABI(uint8_t OSABI) { ELFOSABI = OSABI; }
  void setClass(bool Is64) { Is64Bit = Is64; }
  void setLittleEndian(bool Little) { IsLittleEndian = Little; }

  SmallVectorImpl<char> &getStringPoolStorage() { return StrPoolStorage; }

  void switchSection(DWPSectionId Id) { CurrentSection = Id; }

  /// Zero-copy: stores a reference to the input data without copying.
  void emitBytes(StringRef Data) {
    Sections[CurrentSection].appendBorrowed(Data);
  }

  void emitIntValue(uint64_t Value, unsigned Size) {
    char Buf[8];
    for (unsigned I = 0; I < Size; ++I) {
      Buf[I] = static_cast<char>(Value & 0xff);
      Value >>= 8;
    }
    Sections[CurrentSection].appendOwned(Buf, Size);
  }

  Error writeELF(raw_pwrite_stream &OS);
};

struct UnitIndexEntry {
  DWARFUnitIndex::Entry::SectionContribution Contributions[8];
  std::string Name;
  std::string DWOName;
  StringRef DWPName;
};

// Holds data for Skeleton, Split Compilation, and Type Unit Headers (only in
// v5) as defined in Dwarf 5 specification, 7.5.1.2, 7.5.1.3 and Dwarf 4
// specification 7.5.1.1.
struct InfoSectionUnitHeader {
  // unit_length field. Note that the type is uint64_t even in 32-bit dwarf.
  uint64_t Length = 0;

  // version field.
  uint16_t Version = 0;

  // unit_type field. Initialized only if Version >= 5.
  uint8_t UnitType = 0;

  // address_size field.
  uint8_t AddrSize = 0;

  // debug_abbrev_offset field. Note that the type is uint64_t even in 32-bit
  // dwarf. It is assumed to be 0.
  uint64_t DebugAbbrevOffset = 0;

  // dwo_id field. This resides in the header only if Version >= 5.
  // In earlier versions, it is read from DW_AT_GNU_dwo_id.
  std::optional<uint64_t> Signature;

  // Derived from the length of Length field.
  dwarf::DwarfFormat Format = dwarf::DwarfFormat::DWARF32;

  // The size of the Header in bytes. This is derived while parsing the header,
  // and is stored as a convenience.
  uint8_t HeaderSize = 0;
};

struct CompileUnitIdentifiers {
  uint64_t Signature = 0;
  const char *Name = "";
  const char *DWOName = "";
};

LLVM_ABI Error write(DWPWriter &Out, ArrayRef<std::string> Inputs,
                     OnCuIndexOverflow OverflowOptValue,
                     Dwarf64StrOffsetsPromotion StrOffsetsOptValue,
                     raw_pwrite_stream *OS = nullptr);

typedef std::vector<std::pair<DWARFSectionKind, uint32_t>> SectionLengths;

LLVM_ABI Expected<InfoSectionUnitHeader>
parseInfoSectionUnitHeader(StringRef Info);

} // namespace llvm
#endif // LLVM_DWP_DWP_H
