## Check approriate prefix is choosen to prefix an instruction.
# RUN: llvm-mc -filetype=obj -triple i386-unknown-unknown --x86-align-branch-boundary=32 --x86-align-branch=fused+jcc+jmp  --x86-align-branch-prefix-size=2 %s | llvm-objdump -d  - | FileCheck %s

# CHECK: 00000000 foo:
# CHECK-NEXT:        0: 65 65 a3 01 00 00 00             movl    %eax, %gs:1
# CHECK-NEXT:        7: 3e 55                            pushl   %ebp
# CHECK-NEXT:        9: 57                               pushl   %edi
# CHECK-COUNT-2:      : 55                               pushl   %ebp
# CHECK:             c: 89 e5                            movl    %esp, %ebp
# CHECK-NEXT:        e: 89 7d f8                         movl    %edi, -8(%ebp)
# CHECK-COUNT-5:      : 89 75 f4                         movl    %esi, -12(%ebp)
# CHECK:            20: 39 c5                            cmpl    %eax, %ebp
# CHECK-NEXT:       22: 74 5e                            je      {{.*}}
# CHECK-NEXT:       24: 3e 89 73 f4                      movl    %esi, %ds:-12(%ebx)
# CHECK-NEXT:       28: 89 75 f4                         movl    %esi, -12(%ebp)
# CHECK-NEXT:       2b: 89 7d f8                         movl    %edi, -8(%ebp)
# CHECK-COUNT-5:      : 89 75 f4                         movl    %esi, -12(%ebp)
# CHECK-COUNT-3:      : 5d                               popl    %ebp
# CHECK:            40: 74 40                            je      {{.*}}
# CHECK-NEXT:       42: 5d                               popl    %ebp
# CHECK-NEXT:       43: 74 3d                            je      {{.*}}
# CHECK-NEXT:       45: 36 89 44 24 fc                   movl    %eax, %ss:-4(%esp)
# CHECK-NEXT:       4a: 89 75 f4                         movl    %esi, -12(%ebp)
# CHECK-NEXT:       4d: 89 7d f8                         movl    %edi, -8(%ebp)
# CHECK-COUNT-5:      : 89 75 f4                         movl    %esi, -12(%ebp)
# CHECK:            5f: 5d                               popl    %ebp
# CHECK-NEXT:       60: eb 26                            jmp     {{.*}}
# CHECK-NEXT:       62: eb 24                            jmp     {{.*}}
# CHECK-NEXT:       64: eb 22                            jmp     {{.*}}
# CHECK-NEXT:       66: 89 45 fc                         movl    %eax, -4(%ebp)
# CHECK-NEXT:       69: 89 75 f4                         movl    %esi, -12(%ebp)
# CHECK-NEXT:       6c: 89 7d f8                         movl    %edi, -8(%ebp)
# CHECK-COUNT-3:      : 89 75 f4                         movl    %esi, -12(%ebp)
# CHECK-COUNT-2:      : 5d                               popl    %ebp
# CHECK-NEXT:       7a: 39 c5                            cmpl    %eax, %ebp
# CHECK-NEXT:       7c: 74 04                            je      {{.*}}
# CHECK-COUNT-2:      : 90                               nop
# CHECK-NEXT:       80: eb 06                            jmp     {{.*}}
# CHECK-NEXT:       82: 8b 45 f4                         movl    -12(%ebp), %eax
# CHECK-NEXT:       85: 89 45 fc                         movl    %eax, -4(%ebp)
# CHECK-COUNT-4:      : 89 b5 50 fb ff ff                movl    %esi, -1200(%ebp)
# CHECK:            a0: 89 75 0c                         movl    %esi, 12(%ebp)
# CHECK-NEXT:       a3: e9 fc ff ff ff                   jmp     {{.*}}
# CHECK-COUNT-4:      : 89 b5 50 fb ff ff                movl    %esi, -1200(%ebp)
# CHECK:            c0: 89 75 00                         movl    %esi, (%ebp)
# CHECK-NEXT:       c3: 74 c3                            je      {{.*}}
# CHECK-NEXT:       c5: 74 c1                            je      {{.*}}

  .text
  .globl  foo
  .p2align  4
foo:
  movl    %eax, %gs:0x1
  pushl  %ebp
  pushl  %edi
  .rept 2
  pushl  %ebp
  .endr
  movl  %esp, %ebp
  movl  %edi, -8(%ebp)
  .rept 5
  movl  %esi, -12(%ebp)
  .endr
  cmp  %eax, %ebp
  je  .L_2
  movl  %esi, -12(%ebx)
  movl  %esi, -12(%ebp)
  movl  %edi, -8(%ebp)
  .rept 5
  movl  %esi, -12(%ebp)
  .endr
  .rept 3
  popl  %ebp
  .endr
  je  .L_2
  popl  %ebp
  je  .L_2
  movl  %eax, -4(%esp)
  movl  %esi, -12(%ebp)
  movl  %edi, -8(%ebp)
  .rept 5
  movl  %esi, -12(%ebp)
  .endr
  popl  %ebp
  jmp  .L_3
  jmp  .L_3
  jmp  .L_3
  movl  %eax, -4(%ebp)
  movl  %esi, -12(%ebp)
  movl  %edi, -8(%ebp)
  .rept 3
  movl  %esi, -12(%ebp)
  .endr
  .rept 2
  popl  %ebp
  .endr
  cmp  %eax, %ebp
  je  .L_2
  jmp  .L_3
.L_2:
  movl  -12(%ebp), %eax
  movl  %eax, -4(%ebp)
.L_3:
  .rept 4
  movl  %esi, -1200(%ebp)
  .endr
  movl  %esi, 12(%ebp)
  jmp  bar
  .rept 4
  movl  %esi, -1200(%ebp)
  .endr
  movl  %esi, (%ebp)
  je .L_3
  je .L_3
