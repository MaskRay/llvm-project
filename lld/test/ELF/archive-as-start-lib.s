# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 a.s -o a.o
# RUN: llvm-mc -filetype=obj -triple=x86_64 b.s -o b.o

## Create an archive with incomplete index: foo is missing.
# RUN: llvm-ar --format=gnu rc a.a a.o
# RUN: llvm-ar --format=gnu rcS b.a b.o && tail -c +9 b.a > b-tail
# RUN: cat a.a b-tail > weird.a

## foo defined by weird.a(b.o) is not in the archive index. Not extracted.
# RUN: ld.lld -m elf_x86_64 -u foo weird.a --no-archive-as-start-lib -o archive
# RUN: llvm-nm archive | FileCheck %s --check-prefix=NM1 --implicit-check-not={{.}}

# NM1: [[#%x,]] T _start

## The archive index is ignored. -u foo extracts weird.a(b.o).
# RUN: ld.lld -m elf_x86_64 -u foo weird.a --archive-as-start-lib -o lazy
# RUN: llvm-nm lazy | FileCheck %s --check-prefix=NM2 --implicit-check-not={{.}}

# NM2: [[#%x,]] T _start
# NM2: [[#%x,]] T foo

# RUN: ld.lld -m elf_x86_64 -u foo weird.a -o archive.2 && cmp archive archive.2

#--- a.s
.globl _start
_start:

#--- b.s
.globl foo
foo:
