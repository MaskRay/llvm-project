## Check the case multiple CMPs are followed a jcc is correctly handled.
# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown --x86-align-branch-boundary=32 --x86-align-branch=fused+jcc  --x86-align-branch-prefix-size=5 %s | llvm-objdump -d  - | FileCheck %s

# CHECK: 0000000000000000 test1:
# CHECK-NEXT:        0: 2e 2e 2e 2e 48 39 c5             cmpq    %rax, %rbp
# CHECK-NEXT:        7: 2e 48 39 c5                      cmpq    %rax, %rbp
# CHECK-NEXT:        b: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:        e: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:       11: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:       14: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:       17: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:       1a: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:       1d: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:       20: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:       23: 74 db                            je      {{.*}}

  .text
  .globl    test1
test1:
.Ltmp0:
  .rept 10
  cmp     %rax, %rbp
  .endr
  je      .Ltmp0
