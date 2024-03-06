// RUN: %clang -### -c --target=x86_64 -Wa,--cshdr %s -Werror 2>&1 | FileCheck %s
// RUN: %clang -### -c --target=x86_64 -Wa,--cshdr,--no-cshdr %s -Werror 2>&1 | FileCheck %s --check-prefix=NO
// RUN: not %clang -### -c --target=arm64-apple-darwin -Wa,--cshdr %s 2>&1 | FileCheck %s --check-prefix=ERR

// RUN: %clang -### -c --target=x86_64 -Werror -Wa,--cshdr -x assembler %s -Werror 2>&1 | FileCheck %s --check-prefix=ASM

// CHECK:  "-cc1" {{.*}}"--cshdr"
// NO:     "-cc1"
// NO-NOT: "--cshdr"
// ASM:    "-cc1as" {{.*}}"--cshdr"
// ERR: error: unsupported option '-Wa,--cshdr' for target 'arm64-apple-darwin'

/// Check LTO.
// RUN: %clang -### --target=x86_64-linux -Werror -flto -Wa,--cshdr %s 2>&1 | FileCheck %s --check-prefix=LTO
// RUN: %clang -### --target=x86_64-linux -Werror -flto -Wa,--cshdr -Wa,--no-cshdr %s 2>&1 | FileCheck %s --check-prefix=LTO-NO

// LTO: "-plugin-opt=-cshdr"
// LTO-NO-NOT: "-plugin-opt=-cshdr"

// RUN: touch %t.o
// RUN: not %clang -### --target=x86_64-windows-gnu -flto -Wa,--cshdr %t.o 2>&1 | FileCheck %s --check-prefix=LTO-ERR

// LTO-ERR: error: unsupported option '-Wa,--cshdr' for target 'x86_64-windows-gnu'
