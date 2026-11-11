# REQUIRES: x86
## If two files define different default versions of a symbol, the version does
## not depend on the order of the files.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 a.s -o a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 b.s -o b.o
# RUN: ld.lld -shared --version-script=ver a.o b.o -o ab.so
# RUN: llvm-readelf --dyn-syms ab.so | FileCheck %s
# RUN: ld.lld -shared --version-script=ver b.o a.o -o ba.so
# RUN: llvm-readelf --dyn-syms ba.so | FileCheck %s

# CHECK:       1: {{.*}} NOTYPE WEAK DEFAULT [[#]] foo@@v1
# CHECK-EMPTY:

#--- a.s
.weak foo
.symver foo, foo@@v1
foo:

#--- b.s
.weak foo
.symver foo, foo@@v2
foo:

#--- ver
v1 {};
v2 {};
