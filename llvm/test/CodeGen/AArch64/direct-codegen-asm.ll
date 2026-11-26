; RUN: llc -O0 -mtriple=aarch64-linux-gnu -relocation-model=pic -direct-codegen -direct-codegen-abort %s -o %t.s
; RUN: FileCheck %s < %t.s
; RUN: llc -O0 -mtriple=aarch64-linux-gnu -relocation-model=pic -direct-codegen -direct-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-mc -triple=aarch64-linux-gnu -filetype=obj %t.s -o %t.s.o
; RUN: cmp %t.o %t.s.o

;; Text output prints the instructions that object output encodes, and
;; assembles to the same object. The prologue, which is generated last, comes
;; first, and every return branches to the label of the epilogue.

; CHECK-LABEL: branch:
; CHECK-NEXT:    .cfi_startproc
; CHECK-NEXT:    stp x29, x30, [sp, #-16]!
; CHECK:         mov x29, sp
; CHECK-NEXT:    .cfi_def_cfa w29, 16
; CHECK-NEXT:    cbnz w0, [[F:.Ltmp[0-9]+]]
; CHECK-NEXT:    mov w0, #1
; CHECK-NEXT:    bl ext
; CHECK-NEXT:    b [[RET:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[F]]:
; CHECK-NEXT:    mov w0, #2
; CHECK-NEXT:  [[RET]]:
; CHECK-NEXT:    ldp x29, x30, [sp], #16

; CHECK-LABEL: eh:
; CHECK-NEXT:  [[BEGIN:.Lfunc_begin[0-9]+]]:
; CHECK:         .cfi_lsda 28, [[LSDA:.Lexception[0-9]+]]
; CHECK:       [[CALL:.Ltmp[0-9]+]]:
; CHECK-NEXT:    bl ext
; CHECK-NEXT:  [[CALLEND:.Ltmp[0-9]+]]:
; CHECK:         b .Ltmp
; CHECK-NEXT:  [[LPAD:.Ltmp[0-9]+]]:
; CHECK:       [[LSDA]]:
; CHECK:         .uleb128 [[CSTEND:.Lcst_end[0-9]+]]-[[CSTBEGIN:.Lcst_begin[0-9]+]]
; CHECK-NEXT:  [[CSTBEGIN]]:
; CHECK-NEXT:    .uleb128 [[CALL]]-[[BEGIN]]
; CHECK-NEXT:    .uleb128 [[CALLEND]]-[[CALL]]
; CHECK-NEXT:    .uleb128 [[LPAD]]-[[BEGIN]]
; CHECK-NEXT:    .byte 1
; CHECK:       [[CSTEND]]:

declare i32 @ext(i32)
declare i32 @__gxx_personality_v0(...)
@_ZTIi = external constant ptr

define i32 @branch(i32 %a) uwtable {
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %t, label %f
t:
  %r = call i32 @ext(i32 1)
  ret i32 %r
f:
  ret i32 2
}

define i32 @eh(i32 %a) uwtable personality ptr @__gxx_personality_v0 {
entry:
  %r = invoke i32 @ext(i32 %a) to label %cont unwind label %lpad
cont:
  ret i32 %r
lpad:
  %e = landingpad { ptr, i32 } catch ptr @_ZTIi
  %s = call i32 @ext(i32 0)
  resume { ptr, i32 } %e
}
