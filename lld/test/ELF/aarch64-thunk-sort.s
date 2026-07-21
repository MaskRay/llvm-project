# REQUIRES: aarch64
## Thunks in a ThunkSection are sorted by descending destination address,
## backward branches first, so that a thunk promoted to its long form cannot
## push other thunks out of range. Otherwise each promotion could force another
## on the next pass: with the thunks below in creation order, one cascading
## promotion per pass would exceed the 30-pass limit and fail with "address
## assignment did not converge" (#61250).

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64 a.s -o a.o
# RUN: ld.lld -T lds a.o -o out
# RUN: llvm-objdump -d --no-show-raw-insn out | FileCheck %s

## Forward thunks are in descending destination order, unlike the creation
## order hi0..hi43. The farthest destinations (out of range once the section's
## own 176 bytes shift .high) are promoted to long form at the head of the
## section, where their growth shifts the remaining thunks and their
## destinations in .high together.
# CHECK:      <__AArch64AbsLongThunk_hi43>:
# CHECK-NEXT:  ldr x16,
# CHECK:      <__AArch64AbsLongThunk_hi36>:
# CHECK:      <__AArch64AbsLongThunk_hi35>:
# CHECK-NEXT:  ldr x16,
# CHECK:      <__AArch64AbsLongThunk_hi34>:
# CHECK-NEXT:  b {{.*}} <hi34>
# CHECK:      <__AArch64AbsLongThunk_hi33>:
# CHECK:      <__AArch64AbsLongThunk_hi1>:
# CHECK:      <__AArch64AbsLongThunk_hi0>:
## Backward thunks (in .textbw, placed after .high) are likewise in descending
## destination order, unlike the creation order lo0, lo1, lo2.
# CHECK:      <__AArch64AbsLongThunk_lo2>:
# CHECK:      <__AArch64AbsLongThunk_lo1>:
# CHECK:      <__AArch64AbsLongThunk_lo0>:

#--- a.s
.macro fwdcall i
  bl hi\i
  .space 12
.endm
.macro target i
  .globl hi\i
hi\i:
  ret
  .space 12
.endm

.section .low,"ax",%progbits
.irpc n,012
.globl lo\n
lo\n:
  ret
  .space 12
.endr

.text
.globl _start
_start:
.rept 44
  fwdcall \+
.endr

.section .high,"ax",%progbits
.rept 44
  target \+
.endr

.section .textbw,"ax",%progbits
.irpc n,012
  bl lo\n
.endr

#--- lds
## .low is far below .text, so calls to lo<n> need (always long) thunks.
## .high starts a gap past the end of .text, so it moves as the ThunkSection
## in .text grows. Every bl hi<i> is initially out of range by 16 bytes, so 44
## thunks are created in pass 0. Callers and their destinations are both
## spaced 16 bytes apart while short thunks are packed 4 bytes apart, so thunk
## i's distance to hi<i> is 12*i short of the branch limit, minus the 688 = 44
## calls * 16 - 16 + 12 below. Once the 176-byte all-short ThunkSection shifts
## .high, only the farthest thunk is out of range, and each promotion (12 or
## 16 bytes; long thunks are 8-aligned) pushes out exactly one more thunk if
## thunks are in creation order.
SECTIONS {
  .low 0x10000 : { *(.low) }
  .text 0x10000000 : { *(.text) }
  . = . + 0x8000000 - 688;
  .high : { *(.high) }
  .textbw : { *(.textbw) }
}
