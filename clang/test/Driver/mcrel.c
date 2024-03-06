// RUN: %clang -### -c --target=x86_64 -mcrel %s 2>&1 | FileCheck %s
// RUN: %clang -### -c --target=x86_64 -mcrel -mno-crel %s 2>&1 | FileCheck %s --check-prefix=NO
// RUN: %clang -### -c --target=x86_64 -mcrel -x assembler %s 2>&1 | FileCheck %s --check-prefix=ASM
// RUN: not %clang -### -c --target=arm64-apple-darwin -mcrel %s 2>&1 | FileCheck %s --check-prefix=ERR

// CHECK: "-cc1" {{.*}}"-mcrel"
// NO:     "-cc1"
// NO-NOT: "-mcrel"
// ASM:   "-cc1as" {{.*}}"-mcrel"
// ERR: error: unsupported option '-mcrel' for target 'arm64-apple-darwin'

// RUN: %clang -### --target=x86_64-linux -flto -mcrel %s 2>&1 | FileCheck %s --check-prefix=LTO

// LTO:       "-plugin-opt=-crel"
