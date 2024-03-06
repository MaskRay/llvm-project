// RUN: %clang -### -c --target=x86_64 -mcrel %s 2>&1 | FileCheck %s
// RUN: %clang -### -c --target=x86_64 -mcrel -mno-crel %s 2>&1 | FileCheck %s --check-prefix=NO
// RUN: not %clang -### -c --target=arm64-apple-darwin -mcrel %s 2>&1 | FileCheck %s --check-prefix=ERR
// RUN: not %clang -### -c --target=mips64 -mcrel %s 2>&1 | FileCheck %s --check-prefix=ERR

// RUN: %clang -### -c --target=aarch64 -mcrel -x assembler %s -Werror 2>&1 | FileCheck %s --check-prefix=ASM
// RUN: %clang -### -c --target=mips64 -mcrel -x assembler %s 2>&1 | FileCheck %s --check-prefix=ASM-WARN

// CHECK: "-cc1" {{.*}}"-mcrel"
// NO:     "-cc1"
// NO-NOT: "-mcrel"
// ASM:   "-cc1as" {{.*}}"-mcrel"
// ERR: error: unsupported option '-mcrel' for target '{{.*}}'

// ASM-WARN:     warning: argument unused during compilation: '-mcrel' [-Wunused-command-line-argument]
// ASM-WARN:     "-cc1as"
// ASM-WARN-NOT: "-mcrel"

// RUN: %clang -### --target=x86_64-linux -flto -mcrel %s 2>&1 | FileCheck %s --check-prefix=LTO

// LTO:       "-plugin-opt=-crel"
