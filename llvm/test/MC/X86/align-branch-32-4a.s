## Check prefix of instruction is limited by option --x86-align-branch-prefix-size=NUM.
# RUN: llvm-mc -filetype=obj -triple i386-unknown-unknown --x86-align-branch-boundary=32 --x86-align-branch=fused+jcc+jmp  --x86-align-branch-prefix-size=4 %s | llvm-objdump -d  - | FileCheck %s

# CHECK: 00000000 foo:
# CHECK-NEXT:        0: 3e 66 0f 3a 60 00 03             pcmpestrm    $3, %ds:(%eax), %xmm0
# CHECK-NEXT:        7: 3e c4 e3 79 60 00 03             vpcmpestrm   $3, %ds:(%eax), %xmm0
# CHECK-NEXT:        e: 65 65 65 a3 01 00 00 00          movl    %eax, %gs:1
# CHECK-COUNT-3:      : 89 75 f4                         movl    %esi, -12(%ebp)
# CHECK-NEXT:       1f: 55                               pushl   %ebp
# CHECK-NEXT:       20: a8 04                            testb   $4, %al
# CHECK-NEXT:       22: 70 dc                            jo      {{.*}}

  .text
  .globl  foo
  .p2align  4
foo:
.L1:
  pcmpestrm $3, (%eax), %xmm0
  vpcmpestrm $3, (%eax), %xmm0
  movl  %eax, %gs:0x1
  movl  %esi, -12(%ebp)
  movl  %esi, -12(%ebp)
  movl  %esi, -12(%ebp)
  pushl  %ebp
  testb $0x4,%al
  jo  .L1

