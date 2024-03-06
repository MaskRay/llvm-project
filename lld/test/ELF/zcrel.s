# REQUIRES: x86
## -z crel replaces .rela.dyn with .crel.dyn (CREL).

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 -crel a.s -o a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 -crel b.s -o b.o
# RUN: ld.lld -shared -z crel a.o -o lazy.so
# RUN: llvm-readelf -S -d -r -x .data lazy.so | FileCheck %s --check-prefix=LAZY
# RUN: ld.lld -shared -z now -z crel -z rela a.o -o now.so
# RUN: llvm-readelf -S -d -r -x .data now.so | FileCheck %s --check-prefix=NOW

# LAZY:      [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
# LAZY-NEXT:                        NULL            {{.*}}                         00      0   0  0
# LAZY-NEXT:      .dynsym           DYNSYM          {{.*}}                         18   A  4   1  8
# LAZY-NEXT:      .gnu.hash         GNU_HASH        {{.*}}                         00   A  1   0  8
# LAZY-NEXT:      .hash             HASH            {{.*}}                         04   A  1   0  4
# LAZY-NEXT:      .dynstr           STRTAB          {{.*}}                         00   A  0   0  1
# LAZY-NEXT:      .crel.dyn         CREL            [[#%x,DYN:]]            {{.*}} 00   A  1   0  8
# LAZY-NEXT:      .rel.plt          REL             [[#%x,PLT:]]            {{.*}} 10  AI  1  13  8
# LAZY-NEXT:      .text             PROGBITS        {{.*}}                         00  AX  0   0  4
# LAZY:           .data             PROGBITS        [[#%x,D:]]              {{.*}} 00  WA  0   0  1
# LAZY:      [13] .got.plt          PROGBITS        [[#%x,GOTPLT:]]         {{.*}} 00  WA  0   0  8

# LAZY:      Type        Name/Value
# LAZY-NEXT: (CREL)      0x[[#DYN]]
# LAZY-NEXT: (JMPREL)    0x[[#PLT]]
# LAZY-NEXT: (PLTRELSZ)  16 (bytes)
# LAZY-NEXT: (PLTGOT)    0x[[#GOTPLT]]
# LAZY-NEXT: (PLTREL)    REL{{$}}
# LAZY-NEXT: (SYMTAB)    {{.*}}

# LAZY:       Relocation section '.crel.dyn' at offset {{.*}} contains 3 entries:
# LAZY-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name{{$}}
# LAZY-NEXT:  [[#D+8]]          {{.*}}           R_X86_64_64            {{.*}}           _start{{$}}
# LAZY-NEXT:                    {{.*}}           R_X86_64_GLOB_DAT      0000000000000000 func
# LAZY-NEXT:  [[#D]]            {{.*}}           R_X86_64_RELATIVE
# LAZY-EMPTY:
# LAZY-NEXT:  Relocation section '.rel.plt' at offset {{.*}} contains 1 entries:
# LAZY-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name
# LAZY-NEXT:  [[#GOTPLT+24]]    {{.*}}           R_X86_64_JUMP_SLOT     0000000000000000 func

# LAZY:      Hex dump of section '.data':
# LAZY-NEXT: 0x{{0*}}[[#D]] b8330000 00000000 2a000000 00000000 .

# NOW:      [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
# NOW-NEXT:                        NULL            {{.*}}                         00      0   0  0
# NOW-NEXT:      .dynsym           DYNSYM          {{.*}}                         18   A  4   1  8
# NOW-NEXT:      .gnu.hash         GNU_HASH        {{.*}}                         00   A  1   0  8
# NOW-NEXT:      .hash             HASH            {{.*}}                         04   A  1   0  4
# NOW-NEXT:      .dynstr           STRTAB          {{.*}}                         00   A  0   0  1
# NOW-NEXT:      .crel.dyn         CREL            [[#%x,DYN:]]     {{.*}}        00   A  1   0  8
# NOW-NEXT:      .crel.plt         CREL            [[#%x,PLT:]]     {{.*}}        00  AI  1  11  8
# NOW-NEXT:      .text             PROGBITS        {{.*}}                               00  AX  0   0  4
# NOW:      [11] .got.plt          PROGBITS        [[#%x,GOTPLT:]]  {{.*}}        00  WA  0   0  8
# NOW:           .data             PROGBITS        [[#%x,D:]]       {{.*}}        00  WA  0   0  1

# NOW:      Type        Name/Value
# NOW-NEXT: (FLAGS)     BIND_NOW
# NOW-NEXT: (FLAGS_1)   NOW
# NOW-NEXT: (CREL)      0x[[#DYN]]
# NOW-NEXT: (JMPREL)    0x[[#PLT]]
# NOW-NEXT: (PLTGOT)    {{.*}}
# NOW-NEXT: (PLTREL)    CREL
# NOW-NEXT: (SYMTAB)    {{.*}}

# NOW:       Relocation section '.crel.dyn' at offset {{.*}} contains 3 entries:
# NOW-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# NOW-NEXT:  [[#D+8]]          {{.*}}           R_X86_64_64            {{.*}}           _start + 2a
# NOW-NEXT:                    {{.*}}           R_X86_64_GLOB_DAT      0000000000000000 func + 0
# NOW-NEXT:  [[#D]]            {{.*}}           R_X86_64_RELATIVE                       [[#D]]
# NOW-EMPTY:
# NOW-NEXT:  Relocation section '.crel.plt' at offset {{.*}} contains 1 entries:
# NOW-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# NOW-NEXT:  [[#GOTPLT+24]]    {{.*}}           R_X86_64_JUMP_SLOT     0000000000000000 func + 0

# NOW:       Hex dump of section '.data':
# NOW-NEXT:  0x{{0*}}[[#D]] 00000000 00000000 00000000 00000000 .

## The latter -z nocrel wins.
# RUN: ld.lld -shared -z crel -z nocrel a.o -o out0.so --fatal-warnings
# RUN: llvm-readelf -dr out0.so | FileCheck %s --check-prefix=NOCREL

# RUN: ld.lld -v -zcrel 2>&1 | FileCheck /dev/null --implicit-check-not=warning:

# NOCREL:      Type        Name/Value
# NOCREL-NEXT: (RELA)      {{.*}}
# NOCREL-NEXT: (RELASZ)    72 (bytes)
# NOCREL-NEXT: (RELAENT)   24 (bytes)
# NOCREL-NEXT: (RELACOUNT) 1
# NOCREL-NEXT: (JMPREL)    {{.*}}
# NOCREL-NEXT: (PLTRELSZ)  24 (bytes)
# NOCREL-NEXT: (PLTGOT)    {{.*}}
# NOCREL-NEXT: (PLTREL)    RELA

# RUN: ld.lld -pie -z crel -z rela -z now a.o b.o -o out
# RUN: llvm-readelf -Sdrs out | FileCheck %s --check-prefix=CHECK2

# CHECK2:      .crel.dyn         CREL            [[#%x,DYN:]]      {{.*}} 00   A  1   0  8
# CHECK2-NEXT: .text             PROGBITS        [[#%x,TEXT:]]     {{.*}} 00  AX  0   0  4
# CHECK2:      .got.plt          PROGBITS        [[#%x,GOTPLT:]]   {{.*}} 00  WA  0   0  8
# CHECK2:      .data             PROGBITS        [[#%x,D:]]        {{.*}} 00  WA  0   0  1

# CHECK2:      (CREL)      0x[[#DYN]]
# CHECK2-NEXT: (SYMTAB)    {{.*}}

# CHECK2:       Relocation section '.crel.dyn' at offset {{.*}} contains 5 entries:
# CHECK2-NEXT:      Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
# CHECK2-NEXT:  [[#D]]             {{.*}}          R_X86_64_RELATIVE                 [[#D]]
# CHECK2-NEXT:  [[#D+8]]           {{.*}}          R_X86_64_RELATIVE                 [[#TEXT+42]]
# CHECK2-NEXT:  [[#D+16]]          {{.*}}          R_X86_64_RELATIVE                 [[#%x,IFUNC:]]
# CHECK2-NEXT:  [[#D+32]]          {{.*}}          R_X86_64_RELATIVE                 [[#D+32]]
# CHECK2-NEXT:  [[#GOTPLT]]        {{.*}}          R_X86_64_IRELATIVE                [[#%x,FUNC:]]

# CHECK2:       [[#IFUNC]] 0 FUNC    LOCAL  DEFAULT [[#]] ifunc
# CHECK2:       [[#FUNC]]  0 NOTYPE  GLOBAL DEFAULT [[#]] func

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

.section write,"aw"
  .quad 0
  .quad write+8
