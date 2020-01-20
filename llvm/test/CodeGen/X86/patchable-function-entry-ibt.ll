; RUN: llc -mtriple=x86_64 %s -o - | FileCheck --check-prefixes=CHECK %s

;; -fpatchable-function-entry=0 -fcf-protection=branch
define void @f0() "patchable-function-entry"="0" "branch-target-enforcement" {
; CHECK-LABEL: f0:
; CHECK-NEXT: .Lfunc_begin0:
; CHECK:       # %bb.0:
; CHECK-NEXT:  endbr64
; CHECK-NEXT:  retq
; CHECK-NOT:   .section __patchable_function_entries
  ret void
}

;; -fpatchable-function-entry=1 -fcf-protection=branch
define void @f1() "patchable-function-entry"="1" {
; CHECK-LABEL: f1:
; CHECK-NEXT: .Lfunc_begin1:
; CHECK:       endbr64
; CHECK-NEXT:  nop
; CHECK-NEXT:  retq
; CHECK:       .section __patchable_function_entries,"awo",@progbits,f1,unique,0
; CHECK-NEXT:  .p2align 3
; CHECK-NEXT:  .quad .Lfunc_begin1
  ret void
}

;; -fpatchable-function-entry=2,1 -mbranch-protection=bti
define void @f1_1() "patchable-function-entry"="1" "patchable-function-prefix"="1" {
; CHECK-LABEL: .type f1_1,@function
; CHECK-NEXT: .Ltmp0:
; CHECK-NEXT: nop
; CHECK-NEXT: f1_1:
; CHECK-NEXT: .Lfunc_begin2:
; CHECK:      # %bb.0:
; CHECK-NEXT: endbr64
; CHECK-NEXT: nop
; CHECK-NEXT: retq
; CHECK:      .size f1_1, .Lfunc_end2-f1_1
; CHECK:      .section __patchable_function_entries,"awo",@progbits,f1,unique,0
; CHECK-NEXT: .p2align 3
; CHECK-NEXT: .quad .Ltmp0
  ret void
}

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"cf-protection-branch", i32 1}
