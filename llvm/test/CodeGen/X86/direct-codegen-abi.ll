; RUN: llc -O0 -mtriple=x86_64-linux-gnu -direct-codegen -direct-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-objdump -d -r --no-show-raw-insn %t.o | FileCheck %s

;; Calling convention cases, all compiled directly.

%pair = type { i64, i64 }
%big = type { i64, i64, i64 }

declare void @varargs(i32, ...)
declare void @take_byval(ptr byval(%big) align 8)
declare void @seven(i64, i64, i64, i64, i64, i64, i64)

;; A variadic call passes the number of vector registers used in AL.
; CHECK-LABEL: <call_varargs>:
; CHECK:         movb $0x1, %al
; CHECK-NEXT:    callq
; CHECK-NEXT:      R_X86_64_PLT32 varargs-0x4
define void @call_varargs(double %d) {
  call void (i32, ...) @varargs(i32 1, double %d)
  ret void
}

;; byval copies the object to the outgoing argument area.
; CHECK-LABEL: <call_byval>:
; CHECK:         movq 0x10(%rax), %r10
; CHECK-NEXT:    movq %r10, 0x10(%rsp)
; CHECK:         callq
; CHECK-NEXT:      R_X86_64_PLT32 take_byval-0x4
define void @call_byval(ptr %p) {
  call void @take_byval(ptr byval(%big) align 8 %p)
  ret void
}

;; The seventh integer argument goes on the stack.
; CHECK-LABEL: <call_seven>:
; CHECK:         movq $0x7, (%rsp)
; CHECK:         callq
; CHECK-NEXT:      R_X86_64_PLT32 seven-0x4
define void @call_seven() {
  call void @seven(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7)
  ret void
}

;; A struct of two integers is returned in RAX:RDX.
; CHECK-LABEL: <ret_pair>:
; CHECK:         movq -0x{{[0-9a-f]+}}(%rbp), %rax
; CHECK:         movq -0x{{[0-9a-f]+}}(%rbp), %rdx
; CHECK-NEXT:    leave
define %pair @ret_pair(i64 %a, i64 %b) {
  %p = insertvalue %pair poison, i64 %a, 0
  %q = insertvalue %pair %p, i64 %b, 1
  ret %pair %q
}

;; PHIs that swap their values read both incoming values before writing either.
; CHECK-LABEL: <swap>:
; CHECK:         jae
; CHECK-NEXT:    movq -0x[[B:[0-9a-f]+]](%rbp), %[[R1:[a-z0-9]+]]
; CHECK-NEXT:    movq -0x[[A:[0-9a-f]+]](%rbp), %[[R2:[a-z0-9]+]]
; CHECK-NEXT:    movq -0x{{[0-9a-f]+}}(%rbp), %[[R3:[a-z0-9]+]]
; CHECK-NEXT:    movq %[[R1]], -0x[[A]](%rbp)
; CHECK-NEXT:    movq %[[R2]], -0x[[B]](%rbp)
define i64 @swap(i64 %x, i64 %y, i64 %n) {
entry:
  br label %loop
loop:
  %a = phi i64 [ %x, %entry ], [ %b, %loop ]
  %b = phi i64 [ %y, %entry ], [ %a, %loop ]
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, %n
  br i1 %c, label %loop, label %exit
exit:
  ret i64 %a
}
