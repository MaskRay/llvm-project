add_llvm_component_library(LLVMDWP
  DWP.cpp
  DWPError.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/DWP

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  DebugInfoDWARF
  Object
  ObjCopy
  Support
  )

# Prototype: DWP.cpp includes ObjCopy's private ELFObject.h via a relative
# path. A non-prototype version would promote the writer API to a public
# header.
target_include_directories(LLVMDWP PRIVATE ${LLVM_MAIN_SRC_DIR}/lib)
