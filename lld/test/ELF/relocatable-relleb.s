# REQUIRES: x86
# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 -relleb a.s -o a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 -relleb b.s -o b.o
# RUN: ld.lld -r b.o a.o -o out
# RUN: llvm-readobj -r out | FileCheck %s --check-prefixes=CHECK,RELLEBFOO

# RUN: llvm-mc -filetype=obj -triple=x86_64 a.s -o a1.o
# RUN: ld.lld -r b.o a1.o -o out1
# RUN: llvm-readobj -r out1 | FileCheck %s --check-prefixes=CHECK,RELAFOO
# RUN: ld.lld -r a1.o b.o -o out2
# RUN: llvm-readobj -r out2 | FileCheck %s --check-prefixes=CHECK2

# CHECK:      Relocations [
# CHECK-NEXT:   Section (2) .relleb.text {
# CHECK-NEXT:     0x1 R_X86_64_PLT32 fb 0xFFFFFFFFFFFFFFFC
# CHECK-NEXT:     0x9 R_X86_64_PLT32 foo 0xFFFFFFFFFFFFFFFC
# CHECK-NEXT:     0xE R_X86_64_PLT32 .text.foo 0xFFFFFFFFFFFFFFFC
# CHECK-NEXT:     0x15 R_X86_64_PC32 .L.str 0xFFFFFFFFFFFFFFFC
# CHECK-NEXT:     0x1C R_X86_64_PC32 .L.str1 0xFFFFFFFFFFFFFFFC
# CHECK-NEXT:   }
# RELLEBFOO-NEXT: Section (4) .relleb.text.foo {
# RELAFOO-NEXT:   Section (4) .rela.text.foo {
# CHECK-NEXT:     0x1 R_X86_64_PLT32 g 0xFFFFFFFFFFFFFFFC
# CHECK-NEXT:     0x6 R_X86_64_PLT32 g 0xFFFFFFFFFFFFFFFC
# CHECK-NEXT:   }
# CHECK-NEXT: ]

# CHECK2:      Relocations [
# CHECK2-NEXT:   Section (2) .relleb.text {
# CHECK2-NEXT:     0x1 R_X86_64_PLT32 foo 0xFFFFFFFFFFFFFFFC
# CHECK2-NEXT:     0x6 R_X86_64_PLT32 .text.foo 0xFFFFFFFFFFFFFFFC
# CHECK2-NEXT:     0xD R_X86_64_PC32 .L.str 0xFFFFFFFFFFFFFFFC
# CHECK2-NEXT:     0x14 R_X86_64_PC32 .L.str1 0xFFFFFFFFFFFFFFFC
# CHECK2-NEXT:     0x19 R_X86_64_PLT32 fb 0xFFFFFFFFFFFFFFFC
# CHECK2-NEXT:   }
# CHECK2-NEXT:   Section (4) .rela.text.foo {
# CHECK2-NEXT:     0x1 R_X86_64_PLT32 g 0xFFFFFFFFFFFFFFFC
# CHECK2-NEXT:     0x6 R_X86_64_PLT32 g 0xFFFFFFFFFFFFFFFC
# CHECK2-NEXT:   }
# CHECK2-NEXT: ]

#--- a.s
.global _start, foo
_start:
  call foo
  call .text.foo
  leaq .L.str(%rip), %rsi
  leaq .L.str1(%rip), %rsi

.section .text.foo,"ax"
foo:
  call g
  call g

.section .rodata.str1.1,"aMS",@progbits,1
.L.str:
  .asciz  "abc"
.L.str1:
  .asciz  "def"

#--- b.s
.globl fb
fb:
  call fb
