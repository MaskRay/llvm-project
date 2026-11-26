; RUN: llc -O0 -mtriple=aarch64-linux-gnu -mattr=+sve -direct-codegen -direct-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s
; RUN: llvm-readelf -s %t.o | FileCheck %s --check-prefix=SYM

;; Attributes and metadata that only IR passes consume do not send a function
;; to the regular pipeline.

define i32 @hints(ptr %p) null_pointer_is_valid sanitize_hwaddress vscale_range(1,16) {
  %v = load i32, ptr %p
  ret i32 %v
}

;; -fsanitize=function: the signature and the type hash precede the entry.
; CHECK:         .word 0xc105cafe
; CHECK-NEXT:    .word 0x0000002a
; CHECK-EMPTY:
; CHECK-NEXT:  <func_sanitize>:

; SYM:      14 0 NOTYPE LOCAL DEFAULT 2 $d
; SYM-NEXT: 1c 0 NOTYPE LOCAL DEFAULT 2 $x
define void @func_sanitize() !func_sanitize !0 {
  ret void
}

!llvm.module.flags = !{!1}
!0 = !{i32 -1056584962, i32 42}
!1 = !{i32 4, !"nosanitize_hwaddress", i32 1}
