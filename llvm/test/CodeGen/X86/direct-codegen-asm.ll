; RUN: llc -O0 -mtriple=x86_64-linux-gnu -relocation-model=pic -direct-codegen -direct-codegen-abort %s -o %t.s
; RUN: FileCheck %s < %t.s
; RUN: llc -O0 -mtriple=x86_64-linux-gnu -relocation-model=pic -direct-codegen -direct-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-mc -triple=x86_64-linux-gnu -mcpu=x86-64 -filetype=obj %t.s -o %t.s.o
; RUN: cmp %t.o %t.s.o

;; Text output prints the instructions that object output encodes, and
;; assembles to the same object. Branch targets and the call sites of the
;; exception table are labels, as the assembler chooses instruction lengths.

; CHECK-LABEL: branch:
; CHECK:         movq %rsp, %rbp
; CHECK-NEXT:    .cfi_def_cfa_register %rbp
; CHECK-NEXT:    cmpl $0, %edi
; CHECK-NEXT:    jne [[F:.Ltmp[0-9]+]]
; CHECK-NEXT:    movl $1, %edi
; CHECK-NEXT:    callq ext@PLT
; CHECK-NEXT:    leave
; CHECK-NEXT:    .cfi_def_cfa %rsp, 8
; CHECK-NEXT:    retq
; CHECK-NEXT:    .cfi_def_cfa %rbp, 16
; CHECK-NEXT:  [[F]]:
; CHECK-NEXT:    movl $2, %eax

; CHECK-LABEL: eh:
; CHECK-NEXT:  [[BEGIN:.Lfunc_begin[0-9]+]]:
; CHECK:         .cfi_lsda 27, [[LSDA:.Lexception[0-9]+]]
; CHECK:         subq $32, %rsp
; CHECK-NEXT:  [[CALL:.Ltmp[0-9]+]]:
; CHECK-NEXT:    callq ext@PLT
; CHECK-NEXT:  [[CALLEND:.Ltmp[0-9]+]]:
; CHECK:         retq
; CHECK-NEXT:    .cfi_def_cfa %rbp, 16
; CHECK-NEXT:  [[LPAD:.Ltmp[0-9]+]]:
; CHECK:       [[LSDA]]:
; CHECK:         .uleb128 [[TTBASE:.Lttbase[0-9]+]]-[[TTBASEREF:.Lttbaseref[0-9]+]]
; CHECK-NEXT:  [[TTBASEREF]]:
; CHECK:         .uleb128 [[CSTEND:.Lcst_end[0-9]+]]-[[CSTBEGIN:.Lcst_begin[0-9]+]]
; CHECK-NEXT:  [[CSTBEGIN]]:
; CHECK-NEXT:    .uleb128 [[CALL]]-[[BEGIN]]
; CHECK-NEXT:    .uleb128 [[CALLEND]]-[[CALL]]
; CHECK-NEXT:    .uleb128 [[LPAD]]-[[BEGIN]]
; CHECK-NEXT:    .byte 1
; CHECK:       [[CSTEND]]:
; CHECK:       [[TTBASE]]:

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
