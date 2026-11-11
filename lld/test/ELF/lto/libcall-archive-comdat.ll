; REQUIRES: x86
;; A lazy relocatable file extracted for a libcall of an LTO-generated object
;; file is parsed after the bitcode files have selected their COMDAT groups and
;; definitions. It does not take over either.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-as a.ll -o a.bc
; RUN: llvm-mc -filetype=obj -triple=x86_64 b.s -o b.o
; RUN: ld.lld -shared a.bc --start-lib b.o --end-lib -o out.so
; RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=foo,bar out.so | FileCheck %s

; CHECK:      <foo>:
; CHECK-NEXT:   retq
; CHECK:      <bar>:
; CHECK-NEXT:   retq

;--- a.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

$foo = comdat any

define void @foo() comdat {
  ret void
}

define weak void @bar() {
  ret void
}

define void @_start(ptr %a, ptr %b) {
  call void @llvm.memcpy.p0.p0.i64(ptr %a, ptr %b, i64 1024, i1 false)
  ret void
}

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

;--- b.s
.globl memcpy, foo
.weak bar
memcpy:
  ud2
bar:
  ud2

.section .text.foo,"axG",@progbits,foo,comdat
foo:
  ud2
