set(LLVM_TARGET_DEFINITIONS TargetLibraryInfo.td)
tablegen(LLVM TargetLibraryInfo.inc -gen-target-library-info)

# -gen-opt-parser-defs is in llvm-tblgen, not llvm-min-tblgen.
set(LLVM_TABLEGEN_PROJECT "")
set(LLVM_TARGET_DEFINITIONS AnalysisOptions.td)
tablegen(LLVM AnalysisOptions.inc -gen-opt-parser-defs)

add_public_tablegen_target(analysis_gen)
