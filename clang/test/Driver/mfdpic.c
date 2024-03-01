// RUN: %clang -### --target=armv7-linux -mfdpic %s 2>&1 | FileCheck %s --check-prefix=FDPIC
// RUN: %clang -### --target=armv7-linux -mfdpic -mno-fdpic %s 2>&1 | FileCheck %s --check-prefix=NOFDPIC

/// Unsupported target
// RUN: not %clang --target=aarch64-linux -mfdpic %s 2>&1 | FileCheck --check-prefix=UNSUPPORTED-TARGET %s

// FDPIC:       "-cc1" {{.*}}"-mfdpic"
// NOFDPIC-NOT: "-mfdpic"
// FIXME
// LTO-DESC:       "-plugin-opt=-enable-tlsdesc"
// LTO-NODESC-NOT: "-plugin-opt=-enable-tlsdesc"

// UNSUPPORTED-TARGET: error: unsupported option '-mfdpic' for target
