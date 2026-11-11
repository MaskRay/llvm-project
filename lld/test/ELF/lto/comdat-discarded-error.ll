; REQUIRES: x86
;; The diagnostic for a reference to a discarded COMDAT group names the bitcode
;; file that has the prevailing group.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-as a.ll -o a.bc
; RUN: llvm-mc -filetype=obj -triple=x86_64 b.s -o b.o
; RUN: not ld.lld a.bc b.o 2>&1 | FileCheck %s --implicit-check-not=error:

; CHECK:      error: relocation refers to a discarded section: .text.foo
; CHECK-NEXT: >>> defined in b.o
; CHECK-NEXT: >>> section group signature: foo
; CHECK-NEXT: >>> prevailing definition is in a.bc
; CHECK-NEXT: >>> referenced by b.o:(.data+0x0)

;--- a.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

$foo = comdat any

define void @foo() comdat {
  ret void
}

;--- b.s
.globl _start
_start:

.section .text.foo,"axG",@progbits,foo,comdat
.globl foo
foo:

.data
  .quad .text.foo
