// RUN: %clang -### -c --target=x86_64 -mrelleb %s 2>&1 | FileCheck %s
// RUN: %clang -### -c --target=x86_64 -mrelleb -mno-relleb %s 2>&1 | FileCheck %s --check-prefix=NO
// RUN: %clang -### -c --target=x86_64 -mrelleb -x assembler %s 2>&1 | FileCheck %s --check-prefix=ASM
// RUN: not %clang -### -c --target=arm64-apple-darwin -mrelleb %s 2>&1 | FileCheck %s --check-prefix=ERR

// CHECK: "-cc1" {{.*}}"-mrelleb"
// NO:     "-cc1"
// NO-NOT: "-mrelleb"
// ASM:   "-cc1as" {{.*}}"-mrelleb"
// ERR: error: unsupported option '-mrelleb' for target 'arm64-apple-darwin'

// RUN: %clang -### --target=x86_64-linux -flto -mrelleb %s 2>&1 | FileCheck %s --check-prefix=LTO

// LTO:       "-plugin-opt=-relleb"
