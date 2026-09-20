# On AIX, enable run-time linking to allow symbols from the plugins shared
# objects to be properly bound.
if(CMAKE_SYSTEM_NAME STREQUAL "AIX")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-brtl")
endif()

add_subdirectory(PassBuilderBindings)
add_subdirectory(Plugins)

set(LLVM_LINK_COMPONENTS Core Passes Support)
add_llvm_unittest(PassesTests
  PassesOptionsTest.cpp

  DEPENDS
  passes_options_gen
  )
