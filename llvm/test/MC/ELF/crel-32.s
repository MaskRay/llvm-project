# REQUIRES: powerpc-registered-target
# RUN: llvm-mc -filetype=obj -crel -triple=ppc %s -o %t.o
# RUN: llvm-readelf -Sr %t.o | FileCheck %s

# CHECK:      [ 3] .data             PROGBITS        00000000 000034 000000 00  WA  0   0  1
# CHECK-NEXT: [ 4] .crel.data      CREL          00000000 000084 000041 01   I  5   3  1

# CHECK:      Relocation section '.crel.data' at offset {{.*}} contains 7 entries:
# CHECK-NEXT:  Offset     Info    Type                Sym. Value  Symbol's Name + Addend
# CHECK-NEXT: ffffffc1  00000100 R_PPC_NONE             00000000   a0 + 0
# CHECK-NEXT: ffffff81  00000200 R_PPC_NONE             00000000   a1 - 1
# CHECK-NEXT: ffffffc0  00000300 R_PPC_NONE             00000000   a2 - 1
# CHECK-NEXT: 00000000  00000401 R_PPC_ADDR32           00000000   a3 + 4000
# CHECK-NEXT: 00000040  00000101 R_PPC_ADDR32           00000000   a0 - 80000000
# CHECK-NEXT: 00000040  00000203 R_PPC_ADDR16           00000000   a1 + 7fffffff
# CHECK-NEXT: 00000000  00000203 R_PPC_ADDR16           00000000   a1 - 1

.data
.reloc .-63, BFD_RELOC_NONE, a0
.reloc .-127, BFD_RELOC_NONE, a1-1
.reloc .-64, BFD_RELOC_NONE, a2-1
.reloc ., BFD_RELOC_32, a3+0x4000
.reloc .+64, BFD_RELOC_32, a0-0x80000000
.reloc .+64, BFD_RELOC_16, a1+0x7fffffff
.reloc ., BFD_RELOC_16, a1-1
