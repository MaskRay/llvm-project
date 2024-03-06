// RUN: %clang -### -c --target=x86_64 -Wa,--implicit-addends-for-data %s 2>&1 | FileCheck %s
// RUN: %clang -### -c --target=x86_64 -Wa,--implicit-addends-for-data,--no-implicit-addends-for-data %s 2>&1 | FileCheck %s --check-prefix=NO
// RUN: not %clang -### -c --target=arm64-apple-darwin -Wa,--implicit-addends-for-data %s 2>&1 | FileCheck %s --check-prefix=ERR
// RUN: not %clang -### -c --target=mips64 -Wa,--implicit-addends-for-data %s 2>&1 | FileCheck %s --check-prefix=ERR

// RUN: %clang -### -c --target=aarch64 -Werror -Wa,--implicit-addends-for-data -x assembler %s -Werror 2>&1 | FileCheck %s --check-prefix=ASM
// RUN: not %clang -### -c --target=mips64 -Wa,--implicit-addends-for-data -x assembler %s 2>&1 | FileCheck %s --check-prefix=ERR

// CHECK: "-cc1" {{.*}}"--implicit-addends-for-data"
// NO:     "-cc1"
// NO-NOT: "--implicit-addends-for-data"
// ASM:   "-cc1as" {{.*}}"--implicit-addends-for-data"
// ERR: error: unsupported option '-Wa,--implicit-addends-for-data' for target '{{.*}}'

// RUN: %clang -### --target=x86_64-linux -Werror -flto -Wa,--implicit-addends-for-data %s 2>&1 | FileCheck %s --check-prefix=LTO
// LTO:       "-plugin-opt=-implicit-addends-for-data"

// RUN: touch %t.o
// RUN: not %clang -### --target=mips64-linux-gnu -flto -Wa,--implicit-addends-for-data %t.o 2>&1 | FileCheck %s --check-prefix=ERR
