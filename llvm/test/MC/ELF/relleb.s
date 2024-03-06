# RUN: llvm-mc -filetype=obj -relleb -triple=x86_64 %s -o %t.o
# RUN: llvm-readelf -Sr %t.o | FileCheck %s

# RUN: %if aarch64-registered-target %{ llvm-mc -filetype=obj -relleb -triple=aarch64_be %s -o %t.be.o %}
# RUN: %if aarch64-registered-target %{ llvm-readelf -r %t.be.o | FileCheck %s --check-prefix=A64BE %}
# RUN: %if aarch64-registered-target %{ llvm-readelf -r %t.ppc.o | FileCheck %s --check-prefix=PPC %}

# CHECK:      [ 4] .data             PROGBITS        0000000000000000 000042 000000 00  WA  0   0  1
# CHECK-NEXT: [ 5] .relleb.data      RELLEB          0000000000000000 000198 00002d 01   I 10   4  1
# CHECK-NEXT: [ 6] .rodata           PROGBITS        0000000000000000 000042 00008b 00   A  0   0  1
# CHECK-NEXT: [ 7] .relleb.rodata    RELLEB          0000000000000000 0001c5 000016 01   I 10   6  1
# CHECK-NEXT: [ 8] .debug_addr       PROGBITS        0000000000000000 0000cd 000008 00      0   0  1
# CHECK-NEXT: [ 9] .relleb.debug_addr RELLEB         0000000000000000 0001db 000005 01   I 10   8  1

# CHECK:      Relocation section '.relleb.data' at offset {{.*}} contains 7 entries:
# CHECK-NEXT:     Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# CHECK-NEXT: 0000000000000000  {{.*}}           R_X86_64_NONE          0000000000000000 a0 + 0
# CHECK-NEXT: 0000000000000001  {{.*}}           R_X86_64_NONE          0000000000000000 a1 - 1
# CHECK-NEXT: 0000000000000002  {{.*}}           R_X86_64_NONE          0000000000000000 a2 - 1
# CHECK-NEXT: 0000000000000003  {{.*}}           R_X86_64_32            0000000000000000 a3 + 4000
# CHECK-NEXT: 0000000000000004  {{.*}}           R_X86_64_64            0000000000000000 a0 - 8000000000000000
# CHECK-NEXT: 0000000000000005  {{.*}}           R_X86_64_64            0000000000000000 a1 + 7fffffffffffffff
# CHECK-NEXT: 0000000000000005  {{.*}}           R_X86_64_32            0000000000000000 a1 - 1
# CHECK-EMPTY:
# CHECK-NEXT: Relocation section '.relleb.rodata' at offset {{.*}} contains 5 entries:
# CHECK-NEXT:     Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# CHECK-NEXT: 0000000000000000  {{.*}}           R_X86_64_32            0000000000000000 foo + 0
# CHECK-NEXT: 000000000000003f  {{.*}}           R_X86_64_32            0000000000000000 foo - 40
# CHECK-NEXT: 000000000000007f  {{.*}}           R_X86_64_32            0000000000000000 foo - 81
# CHECK-NEXT: 0000000000000083  {{.*}}           R_X86_64_64            0000000000000000 _start + 1f7f
# CHECK-NEXT: 000000000000008a  {{.*}}           R_X86_64_NONE          0000000000000000 _start + 0
# CHECK-EMPTY:
# CHECK-NEXT: Relocation section '.relleb.debug_addr' at offset 0x1db contains 1 entries:
# CHECK-NEXT:     Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# CHECK-NEXT: 0000000000000000  {{.*}}           R_X86_64_64            0000000000000000 .text + 4

# A64BE:      0000000000000000  {{.*}}           R_AARCH64_NONE         0000000000000000 a0 + 0
# A64BE-NEXT: 0000000000000001  {{.*}}           R_AARCH64_NONE         0000000000000000 a1 - 1
# A64BE-NEXT: 0000000000000002  {{.*}}           R_AARCH64_NONE         0000000000000000 a2 - 1
# A64BE-NEXT: 0000000000000003  {{.*}}           R_AARCH64_ABS32        0000000000000000 a3 + 4000
# A64BE-NEXT: 0000000000000004  {{.*}}           R_AARCH64_ABS64        0000000000000000 a0 - 8000000000000000
# A64BE-NEXT: 0000000000000005  {{.*}}           R_AARCH64_ABS64        0000000000000000 a1 + 7fffffffffffffff
# A64BE-NEXT: 0000000000000005  {{.*}}           R_AARCH64_ABS32        0000000000000000 a1 - 1

.globl _start
_start:
  ret

.section .text.1,"ax"
  ret

.data
.reloc .+0, BFD_RELOC_NONE, a0
.reloc .+1, BFD_RELOC_NONE, a1-1
.reloc .+2, BFD_RELOC_NONE, a2-1
.reloc .+3, BFD_RELOC_32, a3+0x4000
.reloc .+4, BFD_RELOC_64, a0-0x8000000000000000
.reloc .+5, BFD_RELOC_64, a1+0x7fffffffffffffff
.reloc .+5, BFD_RELOC_32, a1-1

.rodata
.long foo
.space 63-4
.long foo - 64  // offset+=63
.space 64-4
.long foo - 129  // offset+=64
.quad _start + 8063
.reloc .-1, BFD_RELOC_NONE, _start

.section .debug_addr
  .quad .text + 4
