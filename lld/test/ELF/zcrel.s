# REQUIRES: x86
## -z crel replaces .rela.dyn with .crel.dyn (CREL).

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 -crel a.s -o a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 -crel b.s -o b.o
# RUN: ld.lld -shared -z crel a.o -o out.so
# RUN: llvm-readelf -S -d -r out.so | FileCheck %s

# CHECK:      Name              Type            Address          Off    Size   ES Flg Lk Inf Al
# CHECK-NEXT:                   NULL            {{.*}}                         00      0   0  0
# CHECK-NEXT: .dynsym           DYNSYM          {{.*}}                         18   A  4   1  8
# CHECK-NEXT: .gnu.hash         GNU_HASH        {{.*}}                         00   A  1   0  8
# CHECK-NEXT: .hash             HASH            {{.*}}                         04   A  1   0  4
# CHECK-NEXT: .dynstr           STRTAB          {{.*}}                         00   A  0   0  1
# CHECK-NEXT: .crel.dyn       CREL          {{0*}}[[#%x,DYN:]]      {{.*}} 00   A  1   0  8
# CHECK-NEXT: .rela.plt         RELA            {{.*}}                  000018 18  AI  1  13  8
# CHECK-NEXT: .text             PROGBITS        {{.*}}                         00  AX  0   0  4

# CHECK:      Type        Name/Value
# CHECK-NEXT: (CREL)    0x298
# CHECK-NEXT: (RELAENT)   24 (bytes)
# CHECK-NEXT: (JMPREL)    0x2b8
# CHECK-NEXT: (PLTRELSZ)  24 (bytes)
# CHECK-NEXT: (PLTGOT)    {{.*}}
# CHECK-NEXT: (PLTREL)    RELA

# CHECK:       Relocation section '.crel.dyn' at offset {{.*}} contains 3 entries:
# CHECK-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# CHECK-NEXT:  00000000000033e0  0000000200000001 R_X86_64_64            00000000000012d0 _start + 2a
# CHECK-NEXT:  00000000000023d0  0000000100000006 R_X86_64_GLOB_DAT      0000000000000000 func + 0
# CHECK-NEXT:  00000000000033d8  0000000000000008 R_X86_64_RELATIVE                 33d8
# CHECK-EMPTY:
# CHECK-NEXT:  Relocation section '.rela.plt' at offset {{.*}} contains 1 entries:
# CHECK-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# CHECK-NEXT:  0000000000003400  0000000100000007 R_X86_64_JUMP_SLOT     0000000000000000 func + 0

## The latter -z rela wins.
# RUN: ld.lld -shared -z crel -z rela a.o -o out0.so --fatal-warnings
# RUN: llvm-readelf -d -r out0.so | FileCheck %s --check-prefix=RELA

# RUN: ld.lld -v -zcrel 2>&1 | FileCheck /dev/null --implicit-check-not=warning:

# RELA:      Type        Name/Value
# RELA-NEXT: (RELA)      {{.*}}
# RELA-NEXT: (RELASZ)    72 (bytes)
# RELA-NEXT: (RELAENT)   24 (bytes)
# RELA-NEXT: (RELACOUNT) 1
# RELA-NEXT: (JMPREL)    {{.*}}
# RELA-NEXT: (PLTRELSZ)  24 (bytes)
# RELA-NEXT: (PLTGOT)    {{.*}}
# RELA-NEXT: (PLTREL)    RELA

# RUN: ld.lld -pie -z crel a.o b.o -o out
# RUN: llvm-readelf -rs out | FileCheck %s --check-prefix=CHECK2

# CHECK2:       Relocation section '.crel.dyn' at offset {{.*}} contains 3 entries:
# CHECK2-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# CHECK2-NEXT:  0000000000003390  0000000000000008 R_X86_64_RELATIVE                 3390
# CHECK2-NEXT:  0000000000003398  0000000000000008 R_X86_64_RELATIVE                 129a
# CHECK2-NEXT:  00000000000033a0  0000000000000008 R_X86_64_RELATIVE                 1280
# CHECK2-EMPTY:
# CHECK2-NEXT:  Relocation section '.rela.dyn' at offset {{.*}} contains 1 entries:
# CHECK2-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# CHECK2-NEXT:  00000000000033a8  0000000000000025 R_X86_64_IRELATIVE                [[#%x,FUNC:]]

# CHECK2:       {{0*}}[[#FUNC]] 0 NOTYPE  GLOBAL DEFAULT [[#]] func

#--- a.s
.globl _start
_start:
  call func@PLT
  movq func@GOTPCREL(%rip), %rax

.data
  .quad .data
  .quad _start+42

#--- b.s
.globl func
func:
  ret
.type ifunc, @gnu_indirect_function
.set ifunc, func

.data
  .quad ifunc
