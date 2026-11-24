; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llc -O0 -mtriple=aarch64-linux-gnu -onepass-codegen -onepass-codegen-report -filetype=obj -mattr=+sve,+sme a.ll -o /dev/null 2>&1 | FileCheck %s
; RUN: llc -O0 -mtriple=aarch64-linux-gnu -onepass-codegen -onepass-codegen-report -filetype=obj -mattr=+strict-align b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=FEATURE
; RUN: llc -O0 -mtriple=aarch64-linux-gnu -onepass-codegen -onepass-codegen-report -filetype=obj -mattr=+harden-sls-retbr b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=FEATURE
; RUN: llc -O0 -mtriple=aarch64-linux-gnu -onepass-codegen -onepass-codegen-report -filetype=obj -mattr=+fix-cortex-a53-835769 b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=FEATURE
; RUN: llc -O0 -mtriple=aarch64-linux-gnu -onepass-codegen -onepass-codegen-report -filetype=obj -mattr=+call-saved-x9 b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=FEATURE
; RUN: llc -O0 -mtriple=aarch64-linux-gnu -onepass-codegen -onepass-codegen-report -filetype=obj -mattr=+reserve-x9 b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=FEATURE
; RUN: llc -O0 -mtriple=aarch64_be-linux-gnu -onepass-codegen -onepass-codegen-report -filetype=obj b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=TARGET
; RUN: llc -O0 -mtriple=aarch64-linux-gnu_ilp32 -onepass-codegen -onepass-codegen-report -filetype=obj b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=TARGET
; RUN: llc -O0 -mtriple=aarch64-linux-gnu -code-model=large -onepass-codegen -onepass-codegen-report -filetype=obj b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=TARGET

;; Functions that need something the one-pass compiler does not implement are
;; left to the regular pipeline, and so is everything under a subtarget whose
;; passes would rewrite the code.

; CHECK-NOT:  onepass:
; CHECK:      onepass: byval: argument attribute
; CHECK-NEXT: onepass: coerced: argument type
; CHECK-NEXT: onepass: tls: global reference
; CHECK-NEXT: onepass: quad: argument type
; CHECK-NEXT: onepass: mte: function attribute sanitize_memtag
; CHECK-NEXT: onepass: pauthlr: function attribute "branch-protection-pauth-lr"
; CHECK-NEXT: onepass: smecall: callee attribute
; CHECK-NEXT: onepass: scalable: vector gep
; CHECK-NOT:  onepass:

; FEATURE: onepass: ok: target feature
; TARGET:  onepass: ok: target

;--- a.ll
%struct.S = type { i64, i64, i64 }
@t = thread_local global i32 0

define i64 @byval(ptr byval(%struct.S) %p) {
  %v = load i64, ptr %p
  ret i64 %v
}

define i64 @coerced([2 x i64] %a) {
  %v = extractvalue [2 x i64] %a, 0
  ret i64 %v
}

define i32 @tls() {
  %v = load i32, ptr @t
  ret i32 %v
}

define fp128 @quad(fp128 %a, fp128 %b) {
  %r = fadd fp128 %a, %b
  ret fp128 %r
}

define void @mte() sanitize_memtag {
  ret void
}

define void @pauthlr() "sign-return-address"="all" "branch-protection-pauth-lr" {
  ret void
}

;; The attribute may be on the declaration only.
declare void @streaming() "aarch64_pstate_sm_enabled"
define void @smecall() {
  call void @streaming()
  ret void
}

define i32 @scalable(ptr %p) {
  %g = getelementptr <vscale x 4 x i32>, ptr %p, i64 1
  %v = load i32, ptr %g
  ret i32 %v
}

;--- b.ll
define void @ok() {
  ret void
}
