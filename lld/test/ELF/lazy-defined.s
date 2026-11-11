# REQUIRES: x86
## A definition in a relocatable object file is preferred to a lazy definition
## regardless of the order of the files, so the lazy file is not extracted.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 ref.s -o ref.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 lazy.s -o lazy.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 def.s -o def.o
# RUN: ld.lld ref.o --start-lib lazy.o --end-lib def.o -o 1
# RUN: llvm-nm 1 | FileCheck %s --implicit-check-not=lazy
# RUN: ld.lld --start-lib lazy.o --end-lib ref.o def.o -o 2
# RUN: llvm-nm 2 | FileCheck %s --implicit-check-not=lazy
# RUN: ld.lld def.o ref.o --start-lib lazy.o --end-lib -o 3
# RUN: llvm-nm 3 | FileCheck %s --implicit-check-not=lazy

# CHECK-DAG: T _start
# CHECK-DAG: T foo

#--- ref.s
.globl _start
_start:
  call foo

#--- lazy.s
.globl foo, lazy
foo:
lazy:

#--- def.s
.globl foo
foo:
