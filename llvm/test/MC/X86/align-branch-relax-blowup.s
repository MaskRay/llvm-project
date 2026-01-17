// Test that sections containing large amounts of branches with alignment
// constraints do not cause quadratic relaxation blowup.

// RUN: llvm-mc -filetype=obj -triple x86_64  --stats -o /dev/null \
// RUN:   --x86-align-branch-boundary=8 --x86-align-branch=call %s 2>&1 \
// RUN: | FileCheck %s
// CHECK: 1 assembler         - Number of assembler layout and relaxation steps

  .text
  .globl foo
  .type foo, @function
foo:
  call bar
  call bar
  call bar
  call bar
  call bar
  ret
