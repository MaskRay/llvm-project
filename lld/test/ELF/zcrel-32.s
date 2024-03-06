# REQUIRES: x86
## -z crel replaces .rela.dyn with .crel.dyn (CREL).
## CREL with implicit addends is used unless -z rel is specified.

# RUN: llvm-mc -filetype=obj -triple=i686 -crel %s -o %t.o
# RUN: ld.lld -shared -z crel -z rel %t.o -o %t.so
# RUN: llvm-readelf -S -d -r -x .data %t.so | FileCheck %s --check-prefix=CREL
# RUN: ld.lld -shared -z crel -z rela %t.o -o %ta.so
# RUN: llvm-readelf -S -d -r -x .data %ta.so | FileCheck %s --check-prefix=CRELA
# RUN: ld.lld -shared -z crel %t.o -o %t && cmp %t %t.so

# CREL:      .crel.dyn   CREL      {{0*}}[[#%x,DYN:]] {{.*}}  000004 00   A  1   0  4
# CREL-NEXT: .text       PROGBITS
# CREL:      .data       PROGBITS  {{0*}}[[#%x,D:]]   {{.*}}         00  WA  0   0  1

# CREL:      Relocation section '.crel.dyn' at offset {{.*}} contains 1 entries:
# CREL-NEXT:  Offset     Info    Type                Sym. Value  Symbol's Name
# CREL-NEXT: {{0*}}[[#D]]{{.*}} R_386_RELATIVE

# CREL:      Hex dump of section '.data':
# CREL-NEXT: 0x{{0*}}[[#D]] de310000 .

# CRELA:      .crel.dyn   CREL      {{0*}}[[#%x,DYN:]] {{.*}}  000007 00   A  1   0  4
# CRELA-NEXT: .text       PROGBITS
# CRELA:      .data       PROGBITS  {{0*}}[[#%x,D:]]   {{.*}}         00  WA  0   0  1

# CRELA:      Relocation section '.crel.dyn' at offset {{.*}} contains 1 entries:
# CRELA-NEXT:  Offset     Info    Type                Sym. Value  Symbol's Name + Addend
# CRELA-NEXT: {{0*}}[[#D]]{{.*}} R_386_RELATIVE                    [[#D+42]]

# CRELA:      Hex dump of section '.data':
# CRELA-NEXT: 0x{{0*}}[[#D]] 00000000 .

.data
.long .data+42
