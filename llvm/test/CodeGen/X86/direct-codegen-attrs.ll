; RUN: llc -O0 -mtriple=x86_64-linux-gnu -direct-codegen -direct-codegen-abort -filetype=obj %s -o %t.o 2>&1 | FileCheck %s --check-prefix=WARN --implicit-check-not=warning:
; RUN: llvm-objdump -s -j .text %t.o | FileCheck %s

;; Attributes, metadata and module flags that only IR passes consume do not
;; send a function to the regular pipeline.

; WARN: warning: {{.*}}stack frame size (72) exceeds limit (16) in function 'big_frame'

declare void @ext()

define i32 @hints(ptr %p) null_pointer_is_valid optforfuzzing sanitize_address sanitize_thread sanitize_memory vscale_range(1,16) "sample-profile-suffix-elision-policy"="selected" {
  %v = load i32, ptr %p
  ret i32 %v
}

;; -fsanitize=function: the signature and the type hash precede the entry.
; CHECK: 0010 feca05c1 2a000000 554889e5
define void @func_sanitize() !func_sanitize !0 {
  ret void
}

define void @small_frame() "warn-stack-size"="16" {
  ret void
}

define void @big_frame() "warn-stack-size"="16" {
  %a = alloca [64 x i8]
  call void @ext()
  ret void
}

!llvm.module.flags = !{!1, !2, !3}
!0 = !{i32 -1056584962, i32 42}
!1 = !{i32 4, !"nosanitize_address", i32 1}
!2 = !{i32 1, !"EnableValueProfiling", i32 0}
!3 = !{i32 1, !"MemProfProfileFilename", !"x.profraw"}
