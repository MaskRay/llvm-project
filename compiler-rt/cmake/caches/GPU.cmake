# This file sets up a CMakeCache for GPU builds of compiler-rt. This supports
# amdgcn and nvptx builds targeting the builtins library.

set(COMPILER_RT_INCLUDE_TESTS OFF CACHE BOOL "")
set(COMPILER_RT_HAS_SAFESTACK OFF CACHE BOOL "")

set(COMPILER_RT_BUILD_BUILTINS ON CACHE BOOL "")
set(COMPILER_RT_BAREMETAL_BUILD ON CACHE BOOL "")
set(COMPILER_RT_BUILD_CRT OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_SANITIZERS OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_XRAY OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_LIBFUZZER OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_PROFILE OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_MEMPROF OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_XRAY_NO_PREINIT OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_ORC OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_GWP_ASAN OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_SCUDO_SANTDALONE_WITH_LLVM_LIBC OFF CACHE BOOL "")
