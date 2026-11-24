; RUN: llc -mtriple=x86_64-- -O2 -enable-machine-outliner -debug-pass=Structure < %s -o /dev/null 2>&1 | FileCheck %s

; REQUIRES: asserts

; Confirm that passes that can add CFI instructions run before CFIFixup.

; CHECK-LABEL: Pass Arguments:
; CHECK:       Insert CFI remember/restore state instructions
; CHECK-NOT:   X86 Optimize Call Frame
; CHECK-NOT:   Prologue/Epilogue Insertion & Frame Finalization
; CHECK-NOT:   Machine Outliner

define void @f() {
  ret void
}
