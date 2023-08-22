// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t
// RUN: llvm-readobj -r %t | FileCheck %s

// CHECK:      .rela.text {
// CHECK-NEXT:   0x0 R_AARCH64_LD64_GOT_LO12_NC x 0x0
// CHECK-NEXT:   0x4 R_AARCH64_LD64_GOT_LO12_NC y 0x0
// CHECK-NEXT:   0x8 R_AARCH64_ADR_GOT_PAGE z 0x0
// CHECK-NEXT:   0xC R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 tls 0x0
// CHECK-NEXT: }

ldr x1, [x1, :got_lo12:x]
ldr x1, [x1, :got_lo12:y]
adrp x1, :got:z
adrp x1, :gottprel:tls

ldr x2, [x2, :got_lo12:x+4]

.data
x: .long 0
y: .long 0
z: .long 0
