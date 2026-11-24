; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -filetype=obj a.ll -o a.o 2>&1 | FileCheck %s
; RUN: llvm-objdump -d -r --no-show-raw-insn a.o | FileCheck %s --check-prefix=CODE

;; Each function needs something the skipped passes would do, so it takes the
;; regular pipeline.

; CHECK: onepass: as256: address space
; CHECK: onepass: large: global reference
; CHECK: onepass: absolute: global reference
; CHECK: onepass: trapfunc: call attribute "trap-func-name"="mytrap"
; CHECK: onepass: retpoline: target feature
; CHECK: onepass: sls: target feature
; CHECK: onepass: reserved: target feature
; CHECK: onepass: deadlabel: address-taken block

; CODE-LABEL: <as256>:
; CODE:         movl %gs:0x0, %eax
; CODE-LABEL: <retpoline>:
; CODE:         R_X86_64_PLT32 __llvm_retpoline_r11-0x4

;--- a.ll
target triple = "x86_64-linux-gnu"

@far = global i32 0, code_model "large"
@abs = external hidden global i8, !absolute_symbol !0
@label = global ptr blockaddress(@deadlabel, %dead)

define i32 @as256() {
  %v = load i32, ptr addrspace(256) null
  ret i32 %v
}

define i32 @large() {
  %v = load i32, ptr @far
  ret i32 %v
}

define ptr @absolute() {
  ret ptr @abs
}

define void @trapfunc() {
  call void @llvm.trap() #0
  unreachable
}

define void @retpoline(ptr %f) "target-features"="+retpoline-indirect-calls" {
  call void %f()
  ret void
}

define void @sls() "target-features"="+harden-sls-ret" {
  ret void
}

define i32 @reserved(i32 %a) "target-features"="+reserve-r10" {
  ret i32 %a
}

define void @deadlabel() {
  ret void
dead:
  ret void
}

declare void @llvm.trap()

attributes #0 = { "trap-func-name"="mytrap" }
!0 = !{i64 0, i64 4294967296}

;--- b.ll
;; The regular pipeline diagnoses calls to "dontcall-error" functions.
; RUN: not llc -O0 -onepass-codegen -filetype=obj b.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=DONTCALL

; DONTCALL: error: call to bad marked "dontcall-error": nope

target triple = "x86_64-linux-gnu"

declare void @bad() "dontcall-error"="nope"

define void @f() {
  call void @bad()
  ret void
}

;--- c.ll
;; A target feature from -mattr counts like the attribute.
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -mattr=+harden-sls-ret -filetype=obj c.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=MATTR
; MATTR: onepass: f: target feature

;; Options that change skipped passes for every function.
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -filetype=obj c.ll -o /dev/null -x86-indirect-branch-tracking 2>&1 | FileCheck %s --check-prefix=OPT
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -filetype=obj c.ll -o /dev/null -x86-speculative-load-hardening 2>&1 | FileCheck %s --check-prefix=OPT
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -filetype=obj c.ll -o /dev/null -x86-seses-enable-without-lvi-cfi 2>&1 | FileCheck %s --check-prefix=OPT
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -filetype=obj c.ll -o /dev/null -function-splitting=all 2>&1 | FileCheck %s --check-prefix=TARGET
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -filetype=obj c.ll -o /dev/null -pass-remarks=. 2>&1 | FileCheck %s --check-prefix=REMARKS
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -filetype=asm c.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ASM
; RUN: llc -O0 -onepass-codegen -onepass-codegen-report -filetype=obj -mtriple=x86_64_lfi-linux-gnu c.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=LFI

; OPT: onepass: module not supported: codegen option
; TARGET: onepass: module not supported: target option
; REMARKS: onepass: module not supported: remarks
; ASM: onepass: module not supported: not an object streamer
; LFI: onepass: f: target{{$}}

target triple = "x86_64-linux-gnu"

define i32 @f(i32 %a) {
  ret i32 %a
}
