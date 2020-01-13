## Check only indirect jumps and calls are aligned with option --x86-align-branch-boundary=32 --x86-align-branch=indirect+call --x86-align-branch-prefix-size=4
# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown --x86-align-branch-boundary=32 --x86-align-branch=indirect+call --x86-align-branch-prefix-size=4 %p/Inputs/align-branch-64-2.s  | llvm-objdump -d  - | FileCheck %s

# CHECK: 0000000000000000 foo:
# CHECK-NEXT:       0: 64 64 64 89 04 25 01 00 00 00    movl    %eax, %fs:1
# CHECK-COUNT-2:     : 64 89 04 25 01 00 00 00          movl    %eax, %fs:1
# CHECK-COUNT-2:     : 89 75 f4                         movl    %esi, -12(%rbp)
# CHECK:           20: ff e0                            jmpq    *%rax
# CHECK-NEXT:      22: 64 64 64 89 04 25 01 00 00 00    movl    %eax, %fs:1
# CHECK-COUNT-2:     : 64 89 04 25 01 00 00 00          movl    %eax, %fs:1
# CHECK:           3c: 89 75 f4                         movl    %esi, -12(%rbp)
# CHECK-NEXT:      3f: 55                               pushq   %rbp
# CHECK-NEXT:      40: ff d0                            callq   *%rax
# CHECK-NEXT:      42: 64 64 64 64 89 04 25 01 00 00 00 movl    %eax, %fs:1
# CHECK-NEXT:      4d: 64 64 64 89 04 25 01 00 00 00    movl    %eax, %fs:1
# CHECK-NEXT:      57: 64 89 04 25 01 00 00 00          movl    %eax, %fs:1
# CHECK-NEXT:      5f: 55                               pushq   %rbp
# CHECK-NEXT:      60: e8 9b ff ff ff                   callq   {{.*}}
# CHECK-COUNT-4:     : 64 89 04 25 01 00 00 00          movl    %eax, %fs:1
# CHECK:           85: ff 14 25 00 00 00 00             callq   *0
