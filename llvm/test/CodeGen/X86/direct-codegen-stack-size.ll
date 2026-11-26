; RUN: llc -O0 -mtriple=x86_64-linux-gnu -direct-codegen -direct-codegen-report -stack-size-section -stack-usage-file=%t.su -filetype=obj %s -o %t.o 2>&1 | FileCheck %s --check-prefix=REPORT
; RUN: llvm-readobj --stack-sizes %t.o | FileCheck %s
; RUN: FileCheck %s --input-file=%t.su --check-prefix=SU

;; A directly compiled function's stack size counts the pushed rbp and the
;; stack adjustment, which a leaf function in the red zone omits.

; REPORT-NOT: direct-codegen: {{[a-z]}}
; REPORT:     direct-codegen: dynalloc:
; REPORT-NOT: direct-codegen:

; CHECK:      Functions: [leaf]
; CHECK-NEXT: Size: 0x8
; CHECK:      Functions: [small]
; CHECK-NEXT: Size: 0x18
; CHECK:      Functions: [large]
; CHECK-NEXT: Size: 0xD8
; CHECK:      Functions: [comdat]
; CHECK-NEXT: Size: 0x8
; CHECK-NOT:  dynalloc

; SU:      :leaf	8	static
; SU-NEXT: :small	24	static
; SU-NEXT: :large	216	static
; SU-NEXT: :comdat	8	static
; SU-NEXT: :dynalloc	{{[0-9]+}}	dynamic

declare void @ext(ptr)

define void @leaf(i32 %x) {
  %a = alloca i32
  store i32 %x, ptr %a
  ret void
}

define void @small() {
  %a = alloca i32
  call void @ext(ptr %a)
  ret void
}

define void @large() {
  %a = alloca [200 x i8]
  call void @ext(ptr %a)
  ret void
}

$comdat = comdat any
define linkonce_odr void @comdat() comdat {
  ret void
}

define void @dynalloc(i32 %n) {
  %a = alloca i32, i32 %n
  call void @ext(ptr %a)
  ret void
}
