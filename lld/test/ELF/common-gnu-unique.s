# REQUIRES: x86
## STB_GNU_UNIQUE is treated like STB_WEAK in symbol resolution: a common symbol
## overrides it, in either order.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 unique.s -o unique.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 common.s -o common.o
# RUN: ld.lld -shared unique.o common.o -o 1.so
# RUN: llvm-readelf --dyn-syms 1.so | FileCheck %s
# RUN: ld.lld -shared common.o unique.o -o 2.so
# RUN: llvm-readelf --dyn-syms 2.so | FileCheck %s

# CHECK: 16 OBJECT GLOBAL DEFAULT [[#]] x

#--- unique.s
.globl x
.type x, @gnu_unique_object
.data
x:
  .quad 0
.size x, 8

#--- common.s
.comm x,16,8
