# REQUIRES: x86
## --fortran-common pulls in an archive member whose STB_GLOBAL definition
## overrides an active tentative (COMMON) definition. Check that the decision
## does not depend on where the COMMON appears among the definitions, and that
## a member providing only a weak definition (which would not override the
## COMMON) is not extracted.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 main.s -o main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 common.s -o common.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 weak.s -o weak.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 ref.s -o ref.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 def.s -o def.o
# RUN: ld.lld -shared def.o -o d.so
# RUN: llvm-ar rc cw.a common.o weak.o
# RUN: llvm-ar rc w.a weak.o
# RUN: llvm-ar rc d.a def.o

## The COMMON is in an unextracted member, so no COMMON is active.
# RUN: ld.lld --fortran-common main.o cw.a weak.o ref.o -o a1 --why-extract=- | \
# RUN:   FileCheck %s --check-prefix=NONE

## A weak definition does not override a COMMON, so its member stays lazy.
# RUN: ld.lld --fortran-common main.o common.o w.a -o a2 --why-extract=- | \
# RUN:   FileCheck %s --check-prefix=NONE

# NONE:      reference{{.*}}extracted{{.*}}symbol
# NONE-NOT:  {{.}}

## The override target is found whether the COMMON precedes or follows the weak
## definition.
# RUN: ld.lld --fortran-common main.o weak.o common.o d.so d.a -o a3 --why-extract=- | \
# RUN:   FileCheck %s --check-prefix=EXTRACT
# RUN: ld.lld --fortran-common main.o common.o weak.o d.so d.a -o a4 --why-extract=- | \
# RUN:   FileCheck %s --check-prefix=EXTRACT

# EXTRACT:      reference{{.*}}extracted{{.*}}symbol
# EXTRACT-NEXT: common.o d.a(def.o) foo
# EXTRACT-NOT:  {{.}}

## The COMMON becomes active when its member is extracted for another symbol.
# RUN: llvm-mc -filetype=obj -triple=x86_64 commonbar.s -o commonbar.o
# RUN: llvm-ar rc cb.a commonbar.o def.o
# RUN: ld.lld --fortran-common -u bar main.o d.so cb.a -o a5 --why-extract=- | \
# RUN:   FileCheck %s --check-prefix=LATE
# RUN: ld.lld --fortran-common -u bar main.o cb.a d.so -o a6 --why-extract=- | \
# RUN:   FileCheck %s --check-prefix=LATE

# LATE:      reference{{.*}}extracted{{.*}}symbol
# LATE-NEXT: <internal> cb.a(commonbar.o) bar
# LATE-NEXT: cb.a(commonbar.o) cb.a(def.o) foo
# LATE-NOT:  {{.}}

#--- main.s
.globl _start
_start:
  ret

#--- common.s
.comm foo,4,4

#--- commonbar.s
.comm foo,4,4
.globl bar
bar:
  ret

#--- weak.s
.data
.weak foo
foo:
  .long 1

#--- def.s
.data
.globl foo
foo:
  .long 2

#--- ref.s
.globl ref
ref:
  movl foo(%rip), %eax
  ret
