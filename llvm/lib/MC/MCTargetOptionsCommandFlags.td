//===- MCTargetOptionsCommandFlags.td - MC command line options -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The command line options that fill MCTargetOptions. Each option names the
// member it stores into.
//
//===----------------------------------------------------------------------===//

include "llvm/Option/OptParser.td"

def mc_group : OptionGroup<"mc">, HelpText<"MC OPTIONS">;

// -name=<v> or -name <v>.
multiclass Val<string name, code type, code member, string help,
               list<OptionFlag> flags = []> {
  def NAME#_EQ : Joined<["-", "--"], name#"=">, Group<mc_group>,
                 HelpText<help>, Field<type, member>, Flags<flags>;
  def NAME : Separate<["-", "--"], name>, Group<mc_group>,
             Alias<!cast<Option>(NAME#"_EQ")>, Flags<flags>;
}

// -name or -name=<bool>.
multiclass Bool<string name, code member, string help,
                list<OptionFlag> flags = []> {
  def NAME#_EQ : Joined<["-", "--"], name#"=">, Group<mc_group>,
                 Field<"bool", member>, Flags<flags>;
  def NAME : Flag<["-", "--"], name>, Group<mc_group>, HelpText<help>,
             Alias<!cast<Option>(NAME#"_EQ")>, AliasArgs<["true"]>,
             Flags<flags>;
}

// -name=<enumerator> or -name <enumerator>. spellings and enumerators are
// parallel lists.
multiclass Enum<string name, code type, code member, string help,
                string spellings, list<code> enumerators> {
  def NAME#_EQ : Joined<["-", "--"], name#"=">, Group<mc_group>,
                 HelpText<help>, Field<type, member>, Values<spellings>,
                 NormalizedValuesScope<type>, NormalizedValues<enumerators>;
  def NAME : Separate<["-", "--"], name>, Group<mc_group>,
             Alias<!cast<Option>(NAME#"_EQ")>;
}

defm mc_relax_all : Bool<"mc-relax-all", "MCRelaxAll",
    "When used with filetype=obj, relax all fixups in the emitted object file">;
defm incremental_linker_compatible : Bool<"incremental-linker-compatible",
    "MCIncrementalLinkerCompatible",
    "When used with filetype=obj, emit an object file which can be used with "
    "an incremental linker">;
defm fdpic : Bool<"fdpic", "FDPIC", "Use the FDPIC ABI">;
defm dwarf_version : Val<"dwarf-version", "int", "DwarfVersion",
    "Dwarf version">;
defm dwarf64 : Bool<"dwarf64", "Dwarf64",
    "Generate debugging info in the 64-bit DWARF format">;
defm emit_dwarf_unwind : Enum<"emit-dwarf-unwind", "EmitDwarfUnwindType",
    "EmitDwarfUnwind",
    "Whether to emit DWARF EH frame entries: always, no-compact-unwind (only "
    "when compact unwind is not available), dwarf-only (force compact unwind "
    "to reference DWARF), default (the target platform default)",
    "always,no-compact-unwind,dwarf-only,default",
    ["Always", "NoCompactUnwind", "DwarfOnly", "Default"]>;
defm emit_compact_unwind_non_canonical : Bool<
    "emit-compact-unwind-non-canonical", "EmitCompactUnwindNonCanonical",
    "Whether to try to emit Compact Unwind for non canonical entries.">;
defm gsframe : Bool<"gsframe", "EmitSFrameUnwind",
    "Whether to emit .sframe unwind sections.">;
defm asm_show_inst : Bool<"asm-show-inst", "ShowMCInst",
    "Emit internal instruction representation to assembly file">;
defm fatal_warnings : Bool<"fatal-warnings", "MCFatalWarnings",
    "Treat warnings as errors">;
defm no_warn : Bool<"no-warn", "MCNoWarn", "Suppress all warnings">;
def W : Flag<["-"], "W">, Group<mc_group>, HelpText<"Alias for --no-warn">,
        Alias<no_warn_EQ>, AliasArgs<["true"]>;
defm no_deprecated_warn : Bool<"no-deprecated-warn", "MCNoDeprecatedWarn",
    "Suppress all deprecated warnings">;
defm no_type_check : Bool<"no-type-check", "MCNoTypeCheck",
    "Suppress type errors (Wasm)">;
defm save_temp_labels : Bool<"save-temp-labels", "MCSaveTempLabels",
    "Don't discard temporary labels">;
defm crel : Bool<"crel", "Crel", "Use CREL relocation format for ELF">;
defm implicit_mapsyms : Bool<"implicit-mapsyms", "ImplicitMapSyms",
    "Allow mapping symbol at section beginning to be implicit, lowering number "
    "of mapping symbols at the expense of some portability. Recommended for "
    "projects that can build all their object files using this option">;
defm x86_relax_relocations : Bool<"x86-relax-relocations",
    "X86RelaxRelocations",
    "Emit GOTPCRELX/REX_GOTPCRELX/CODE_4_GOTPCRELX instead of GOTPCREL on "
    "x86-64 ELF">;
defm x86_sse2avx : Bool<"x86-sse2avx", "X86Sse2Avx",
    "Specify that the assembler should encode SSE instructions with VEX "
    "prefix">;
defm no_integrated_as : Bool<"no-integrated-as", "DisableIntegratedAS",
    "Disable integrated assembler">;
defm reloc_section_sym : Enum<"reloc-section-sym", "RelocSectionSymType",
    "RelocSectionSym",
    "Control section symbol conversion for relocations: all (for all eligible "
    "local symbols), internal (only for internal local symbols), none",
    "all,internal,none", ["All", "Internal", "None"]>;
defm large_eh_encoding : Bool<"large-eh-encoding", "LargeEHEncoding",
    "Force 8-byte (sdata8) pointer encodings for ELF exception-handling "
    "sections to avoid relocation overflows in large binaries (x86_64: "
    "FDE/personality/LSDA/TType; AArch64/PPC64: FDE only, the rest are already "
    "sdata8)">;
defm target_abi : Val<"target-abi", "std::string", "ABIName",
    "The name of the ABI to be targeted from the backend.">;
defm as_secure_log_file : Val<"as-secure-log-file", "std::string",
    "AsSecureLogFile", "As secure log file name", [HelpHidden]>;
defm asm_macro_max_nesting_depth : Val<"asm-macro-max-nesting-depth",
    "unsigned", "AsmMacroMaxNestingDepth",
    "The maximum nesting depth allowed for assembly macros.", [HelpHidden]>;
