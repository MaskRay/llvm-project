# REQUIRES: x86
## Test how .symver interacts with --version-script.
# RUN: llvm-mc -filetype=obj -triple=x86_64 %s -o %t.o
# RUN: echo 'call foo3; call foo4' > %tref.s
# RUN: llvm-mc -filetype=obj -triple=x86_64 %tref.s -o %tref.o

# RUN: echo 'v1 { local: foo1; }; v2 { local: foo2; };' > %t1.script
# RUN: ld.lld --version-script %t1.script -shared %t.o -o %t1.so
# RUN: llvm-readelf --dyn-syms %t1.so | FileCheck --check-prefix=EXACT %s
# EXACT:      UND
# EXACT-NEXT: [[#]] foo4@@v2
# EXACT-NEXT: [[#]] _start{{$}}
# EXACT-NEXT: [[#]] foo3@v1
# EXACT-NOT:  {{.}}

# RUN: echo 'v1 { local: foo*; }; v2 {};' > %t2.script
# RUN: ld.lld --version-script %t2.script -shared %t.o -o %t2.so
# RUN: llvm-readelf --dyn-syms %t2.so | FileCheck --check-prefix=WC %s
# WC:      UND
# WC-NEXT: [[#]] foo4@@v2
# WC-NEXT: [[#]] _start{{$}}
# WC-NOT:  {{.}}

## A wildcard local: pattern in the symbol's own version node localizes a
## default-versioned definition (foo4@@v2), like an exact pattern and GNU ld.
# RUN: echo 'v1 {}; v2 { local: foo4*; };' > %twc2.script
# RUN: ld.lld --version-script %twc2.script -shared %t.o -o %twc2.so
# RUN: llvm-readelf --dyn-syms %twc2.so | FileCheck --check-prefix=WC2 %s
# WC2:      UND
# WC2-NEXT: [[#]] foo1{{$}}
# WC2-NEXT: [[#]] foo2{{$}}
# WC2-NEXT: [[#]] _start{{$}}
# WC2-NEXT: [[#]] foo3@v1
# WC2-NOT:  {{.}}

## A local: pattern in a different node does not localize foo4@@v2; its version
## node (v2) takes precedence over the v1 pattern.
# RUN: echo 'v1 { local: foo4; }; v2 {};' > %tother.script
# RUN: ld.lld --version-script %tother.script -shared %t.o -o %tother.so
# RUN: llvm-readelf --dyn-syms %tother.so | FileCheck --check-prefix=OTHER %s
# OTHER:      UND
# OTHER-NEXT: [[#]] foo1{{$}}
# OTHER-NEXT: [[#]] foo2{{$}}
# OTHER-NEXT: [[#]] foo4@@v2
# OTHER-NEXT: [[#]] _start{{$}}
# OTHER-NEXT: [[#]] foo3@v1
# OTHER-NOT:  {{.}}

# RUN: echo 'v1 { global: *; local: foo*; }; v2 {};' > %t3.script
# RUN: ld.lld --version-script %t3.script -shared %t.o -o %t3.so
# RUN: llvm-readelf --dyn-syms %t3.so | FileCheck --check-prefix=MIX1 %s
# MIX1:      UND
# MIX1-NEXT: [[#]] foo4@@v2
# MIX1-NEXT: [[#]] _start@@v1
# MIX1-NOT:  {{.}}

# RUN: echo 'v1 { global: foo*; local: *; }; v2 { global: foo4; local: *; };' > %t4.script
# RUN: ld.lld --version-script %t4.script -shared %t.o -o %t4.so
# RUN: llvm-readelf --dyn-syms %t4.so | FileCheck --check-prefix=MIX2 %s
# MIX2:      UND
# MIX2-NEXT: [[#]] foo1@@v1
# MIX2-NEXT: [[#]] foo2@@v1
# MIX2-NEXT: [[#]] foo4@@v2
# MIX2-NEXT: [[#]] foo3@v1
# MIX2-NOT:  {{.}}

# RUN: ld.lld --version-script %t4.script -pie --export-dynamic %t.o -o %t4
# RUN: llvm-readelf --dyn-syms %t4 | FileCheck --check-prefix=MIX2 %s
# RUN: ld.lld --version-script %t4.script -pie %t.o -o %t4
# RUN: llvm-readelf --dyn-syms %t4 | FileCheck --check-prefix=EXE %s

# EXE: Symbol table '.dynsym' contains 1 entries:

# RUN: ld.lld --version-script %t4.script -shared %t.o %tref.o -o %t5.so
# RUN: llvm-readelf -r %t5.so | FileCheck --check-prefix=RELOC %s

# RELOC: R_X86_64_JUMP_SLOT {{.*}} foo4@@v2 + 0
# RELOC: R_X86_64_JUMP_SLOT {{.*}} foo3@v1 + 0

.globl foo1; foo1: ret
.globl foo2; foo2: ret
.globl foo3; .symver foo3,foo3@v1; foo3: ret
.globl foo4; .symver foo4,foo4@@v2; foo4: ret

.globl _start; _start: ret
