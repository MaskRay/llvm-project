## Check only fused conditional jumps, conditional jumps and unconditional jumps are aligned with option --x86-align-branch-boundary=32 --x86-align-branch=fused+jcc+jmp --x86-align-branch-prefix-size=4
# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown --x86-align-branch-boundary=32 --x86-align-branch=fused+jcc+jmp --x86-align-branch-prefix-size=4 %p/Inputs/align-branch-64-1.s | llvm-objdump -d  - > %t1
# RUN: FileCheck --input-file=%t1 %s

# CHECK: 0000000000000000 foo:
# CHECK-NEXT:       0: 64 64 64 64 89 04 25 01 00 00 00 movl    %eax, %fs:1
# CHECK-COUNT-2:     : 64 89 04 25 01 00 00 00          movl    %eax, %fs:1
# CHECK:           1b: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:      1e: 31 c0                            xorl    %eax, %eax
# CHECK-NEXT:      20: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:      23: 74 5d                            je      {{.*}}
# CHECK-NEXT:      25: 64 64 89 04 25 01 00 00 00       movl    %eax, %fs:1
# CHECK-COUNT-2:     : 64 89 04 25 01 00 00 00          movl    %eax, %fs:1
# CHECK:           3e: 31 c0                            xorl    %eax, %eax
# CHECK-NEXT:      40: 74 40                            je      {{.*}}
# CHECK-NEXT:      42: 5d                               popq    %rbp
# CHECK-NEXT:      43: 74 3d                            je      {{.*}}
# CHECK-NEXT:      45: 64 64 89 04 25 01 00 00 00       movl    %eax, %fs:1
# CHECK-COUNT-2:     : 64 89 04 25 01 00 00 00          movl    %eax, %fs:1
# CHECK:           5e: 31 c0                            xorl    %eax, %eax
# CHECK-NEXT:      60: eb 26                            jmp     {{.*}}
# CHECK-NEXT:      62: eb 24                            jmp     {{.*}}
# CHECK-NEXT:      64: eb 22                            jmp     {{.*}}
# CHECK-COUNT-2:     : 64 89 04 25 01 00 00 00          movl    %eax, %fs:1
# CHECK:           76: 89 45 fc                         movl    %eax, -4(%rbp)
# CHECK-NEXT:      79: 5d                               popq    %rbp
# CHECK-NEXT:      7a: 48 39 c5                         cmpq    %rax, %rbp
# CHECK-NEXT:      7d: 74 03                            je      {{.*}}
# CHECK-NEXT:      7f: 90                               nop
# CHECK-NEXT:      80: eb 06                            jmp     {{.*}}
# CHECK-NEXT:      82: 8b 45 f4                         movl    -12(%rbp), %eax
# CHECK-NEXT:      85: 89 45 fc                         movl    %eax, -4(%rbp)
# CHECK-COUNT-10:    : 89 b5 50 fb ff ff                movl    %esi, -1200(%rbp)
# CHECK:           c4: eb c2                            jmp     {{.*}}
# CHECK-NEXT:      c6: c3                               retq
