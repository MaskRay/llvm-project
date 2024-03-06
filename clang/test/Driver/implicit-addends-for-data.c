// RUN: %clang -### -c --target=x86_64 -Wa,--light-elf %s 2>&1 | FileCheck %s
// RUN: %clang -### -c --target=x86_64 -Wa,--light-elf,--no-light-elf %s 2>&1 | FileCheck %s --check-prefix=NO
// RUN: not %clang -### -c --target=arm64-apple-darwin -Wa,--light-elf %s 2>&1 | FileCheck %s --check-prefix=ERR
// RUN: not %clang -### -c --target=mips64 -Wa,--light-elf %s 2>&1 | FileCheck %s --check-prefix=ERR

// RUN: %clang -### -c --target=aarch64 -Werror -Wa,--light-elf -x assembler %s -Werror 2>&1 | FileCheck %s --check-prefix=ASM
// RUN: not %clang -### -c --target=mips64 -Wa,--light-elf -x assembler %s 2>&1 | FileCheck %s --check-prefix=ERR

// CHECK: "-cc1" {{.*}}"--light-elf"
// NO:     "-cc1"
// NO-NOT: "--light-elf"
// ASM:   "-cc1as" {{.*}}"--light-elf"
// ERR: error: unsupported option '-Wa,--light-elf' for target '{{.*}}'

// RUN: %clang -### --target=x86_64-linux -Werror -flto -Wa,--light-elf %s 2>&1 | FileCheck %s --check-prefix=LTO
// LTO:       "-plugin-opt=-light-elf"

// RUN: touch %t.o
// RUN: not %clang -### --target=mips64-linux-gnu -flto -Wa,--light-elf %t.o 2>&1 | FileCheck %s --check-prefix=ERR
