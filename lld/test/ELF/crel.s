# REQUIRES: x86
# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 -crel a.s -o a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 -crel b.s -o b.o
# RUN: ld.lld -pie a.o b.o -o out
# RUN: llvm-objdump -d out | FileCheck %s
# RUN: llvm-readelf -Srs out | FileCheck %s --check-prefix=RELOC

# CHECK:       <_start>:
# CHECK-NEXT:    callq {{.*}} <foo>
# CHECK-NEXT:    callq {{.*}} <foo>
# CHECK-EMPTY:
# CHECK-NEXT:  <foo>:
# CHECK-NEXT:    leaq {{.*}}  # 0x264
# CHECK-NEXT:    leaq {{.*}}  # 0x260

# RELOC:  .data             PROGBITS        {{0*}}[[#%x,DATA:]]

# RELOC:  {{0*}}[[#DATA]]  0000000000000008 R_X86_64_RELATIVE [[#%x,FOO:]]

# RELOC:  {{0*}}[[#FOO]]     0 NOTYPE  GLOBAL DEFAULT [[#]] foo

# RUN: ld.lld -pie --emit-relocs a.o b.o -o out1
# RUN: llvm-objdump -dr out1 | FileCheck %s --check-prefix=CHECKE
# RUN: llvm-readelf -r out1 | FileCheck %s --check-prefix=RELOCE

# CHECKE:       <_start>:
# CHECKE-NEXT:    callq {{.*}} <foo>
# CHECKE-NEXT:      R_X86_64_PLT32 foo-0x4
# CHECKE-NEXT:    callq {{.*}} <foo>
# CHECKE-NEXT:      R_X86_64_PLT32 .text+0x6
# CHECKE-EMPTY:
# CHECKE-NEXT:  <foo>:
# CHECKE-NEXT:    leaq {{.*}}  # 0x264
# CHECKE-NEXT:      R_X86_64_PC32 .L.str-0x4
# CHECKE-NEXT:    leaq {{.*}}  # 0x260
# CHECKE-NEXT:      R_X86_64_PC32 .L.str1-0x4

# RELOCE:      Relocation section '.crel.rodata' at offset {{.*}} contains 7 entries:
# RELOCE-NEXT:     Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# RELOCE-NEXT: 0000000000000268  {{.*}}           R_X86_64_PC32          {{.*}}           foo + 0
# RELOCE-NEXT: 00000000000002a7  {{.*}}           R_X86_64_PC32          {{.*}}           foo + 3f
# RELOCE-NEXT: 00000000000002e7  {{.*}}           R_X86_64_PC32          {{.*}}           foo + 7f
# RELOCE-NEXT: 00000000000002eb  {{.*}}           R_X86_64_PC32          {{.*}}           foo - 1f81
# RELOCE-NEXT: 00000000000002ef  {{.*}}           R_X86_64_PC64          {{.*}}           foo - 1f81
# RELOCE-NEXT: 00000000000002f7  {{.*}}           R_X86_64_PC64          {{.*}}           _start - 1f81
# RELOCE-NEXT: 00000000000042ff  {{.*}}           R_X86_64_NONE          {{.*}}           _start + 4000

#--- a.s
.global _start, foo
_start:
  call foo
  call .text.foo

.section .text.foo,"ax"
foo:
  leaq .L.str(%rip), %rsi
  leaq .L.str1(%rip), %rsi

.data
.quad foo

.section .rodata.str1.1,"aMS",@progbits,1
.L.str:
  .asciz  "abc"
.L.str1:
  .asciz  "def"

#--- b.s
.rodata
.long foo - .
.space 63-4
.long foo - . + 63  # offset+=63
.space 64-4
.long foo - . + 127  # offset+=64
.long foo - . - 8065
.quad foo - . - 8065
.quad _start - . - 8065
.reloc .+0x4000, BFD_RELOC_NONE, _start - .
