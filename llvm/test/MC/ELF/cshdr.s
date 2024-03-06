# RUN: llvm-mc -filetype=obj -cshdr -triple=x86_64 %s -o %t
# RUN: llvm-readelf -hS %t | FileCheck %s
# RUN: od -Ax -tx1 -j 0xb0 %t | FileCheck %s --ignore-case --check-prefix=HEX

# CHECK:       Size of section headers:           0 (bytes)
# CHECK:       There are 7 section headers, starting at offset 0xb0:
# CHECK:       [ 0]                   NULL            0000000000000000 {{.*}} 000000 00      0   0  1
# CHECK-NEXT:  [ 1] .strtab           STRTAB          0000000000000000 {{.*}} 000031 00      0   0  1
# CHECK-NEXT:  [ 2] .text             PROGBITS        0000000000000000 000040 000001 00  AX  0   0  4
# CHECK-NEXT:  [ 3] .text.1           PROGBITS        0000000000000000 000041 000001 00  AX  0   0  1
# CHECK-NEXT:  [ 4] .link             PROGBITS        0000000000000000 000042 000001 08 AML  3   0  1
# CHECK-NEXT:  [ 5] .bss              NOBITS          0000000000000000 000043 000001 00  WA  0   0  1
# CHECK-NEXT:  [ 6] .symtab           SYMTAB          0000000000000000 {{.*}} 000030 18      1   1  8

# HEX: 0000b0 01 01 01 01 09 33 f1 07 63 4a 03 81 0d 03 05 0a

.globl _start
_start:
  ret

.section .text.1,"ax"
  ret

.section .link,"aMo",@progbits,8,.text.1
.space 1

.bss
.space 1
