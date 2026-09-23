# REQUIRES: x86

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 %t/b.s -o %t/b.o

## foo and __foo are interconnected and defined in two lazy object files.
## Test we resolve both to the same file.
## With order-independent archive extraction the live set is computed to a
## fixpoint and ties break by file index, so the weak foo/__foo resolve to the
## lower-indexed a.o (instead of b.o's later positional definition winning).
# RUN: ld.lld -y a -y foo -y __foo %t/main.o --start-lib %t/a.o %t/b.o --end-lib -o /dev/null | FileCheck %s

# CHECK:      a.o: definition of a
# CHECK-NEXT: a.o: definition of foo
# CHECK-NEXT: a.o: definition of __foo
# CHECK-NEXT: b.o: reference to a

#--- main.s
.globl _start
_start:
  call b

#--- a.s
.globl a
.weak foo
a:
foo:

.weak __foo
__foo:

#--- b.s
.globl b
.weak foo
b:
  call a
foo:

.weak __foo
__foo:
