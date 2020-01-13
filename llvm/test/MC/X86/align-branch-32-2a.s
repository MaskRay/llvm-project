## Check no prefix is inserted after hardcode.
# RUN: llvm-mc -filetype=obj -triple i386-unknown-unknown --x86-align-branch-boundary=32 --x86-align-branch=fused+jcc+jmp  --x86-align-branch-prefix-size=2 %s | llvm-objdump -d  - | FileCheck %s

# CHECK: 00000000 main:
# CHECK-NEXT:        0: 2e 55                            pushl    %ebp
# CHECK-NEXT:        2: 2e 89 e5                         movl     %esp, %ebp
# CHECK-NEXT:        5: 3e 55                            pushl    %ebp
# CHECK-COUNT-25:       55                               pushl    %ebp
# CHECK-NEXT:       20: eb 00                            jmp      {{.*}}
# CHECK: 00000022 infiniteLoop:
# CHECK-NEXT:       22: eb dc                            jmp      {{.*}}

    .text
    .globl infiniteLoop
main:
    .byte 0x2e
    pushl %ebp
    .byte 0x2e
    movl  %esp, %ebp
    .rept 26
    pushl %ebp
    .endr
    jmp infiniteLoop
infiniteLoop:
    jmp main
