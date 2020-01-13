## Check no prefixes is added to the instruction if there is a align directive between the instruction and the target branch
# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown --x86-align-branch-boundary=32 --x86-align-branch=jmp  --x86-align-branch-prefix-size=5 %s | llvm-objdump -d  - | FileCheck %s

# CHECK: 0000000000000000 test1:
# CHECK-NEXT:        0: 31 d2                            xorl    %edx, %edx
# CHECK-NEXT:        2: 89 8c 24 84 00 00 00             movl    %ecx, 132(%rsp)
# CHECK-NEXT:        9: 4c 89 c1                         movq    %r8, %rcx
# CHECK-NEXT:        c: 4c 8b 8c 24 88 00 00 00          movq    136(%rsp), %r9
# CHECK-COUNT-4:      : 90                               nop
# CHECK:            18: 66 66 90                         nop
# CHECK-NEXT:       1b: 2e 2e 4c 89 c1                   movq    %r8, %rcx
# CHECK-NEXT:       20: eb de                            jmp     {{.*}}
# CHECK-NEXT:       22: c3                               retq

  .text
  .globl    test1
test1:
.Ltmp0:
  xorl    %edx, %edx
  movl    %ecx, 132(%rsp)
  movq    %r8, %rcx
  movq    136(%rsp), %r9
  .p2align    3, 0x90
  .byte    102
  .byte    102
  nop
  movq    %r8, %rcx
  jmp    .Ltmp0
  retq
