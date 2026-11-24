; RUN: llc -O0 -mtriple=x86_64-linux-gnu -relocation-model=pic -onepass-codegen -onepass-codegen-report -filetype=obj %s -o %t.o 2>&1 | FileCheck %s --check-prefix=REPORT
; RUN: llvm-objdump -d -r --no-show-raw-insn %t.o | FileCheck %s

;; Stack protector, indirect branch tracking and stack probes are handled
;; rather than sent to the regular pipeline.

; REPORT-NOT: onepass:
; REPORT:     onepass: bigframe: stack probe
; REPORT-NOT: onepass:

;; The guard is next to the frame pointer and the array is right below it.
; CHECK-LABEL: <ssp>:
; CHECK-NEXT:    endbr64
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    movq %rsp, %rbp
; CHECK-NEXT:    subq $0x20, %rsp
; CHECK-NEXT:    movq %fs:0x28, %r11
; CHECK-NEXT:    movq %r11, -0x8(%rbp)
; CHECK-NEXT:    movl $0x7, -0x20(%rbp)
; CHECK-NEXT:    leaq -0x18(%rbp), %rdi
; CHECK-NEXT:    callq
; CHECK-NEXT:      R_X86_64_PLT32 use-0x4
; CHECK-NEXT:    movl -0x20(%rbp), %eax
; CHECK-NEXT:    movq %fs:0x28, %r11
; CHECK-NEXT:    cmpq -0x8(%rbp), %r11
; CHECK-NEXT:    je
; CHECK-NEXT:    callq
; CHECK-NEXT:      R_X86_64_PLT32 __stack_chk_fail-0x4
; CHECK-NEXT:    movq %rbp, %rsp
; CHECK-NEXT:    popq %rbp
; CHECK-NEXT:    retq
define i32 @ssp() sspstrong {
  %x = alloca i32
  %buf = alloca [16 x i8]
  store i32 7, ptr %x
  call void @use(ptr %buf)
  %v = load i32, ptr %x
  ret i32 %v
}

;; sspstrong without an array or an escaping local needs no guard.
; CHECK-LABEL: <nossp>:
; CHECK-NOT:     %fs
; CHECK:         retq
define i32 @nossp(i32 %a) sspstrong {
  %x = alloca i32
  store i32 %a, ptr %x
  %v = load i32, ptr %x
  ret i32 %v
}

;; A local function whose address is not taken cannot be called indirectly.
; CHECK-LABEL: <local>:
; CHECK-NEXT:    pushq %rbp
define internal void @local() {
  ret void
}

; CHECK-LABEL: <addrtaken>:
; CHECK-NEXT:    endbr64
define internal void @addrtaken() {
  call void @local()
  ret void
}

;; setjmp returns through an indirect jump, and so does the unwinder to a
;; landing pad.
; CHECK-LABEL: <ibt>:
; CHECK-NEXT:    endbr64
; CHECK:         callq
; CHECK-NEXT:      R_X86_64_PLT32 setjmp-0x4
; CHECK-NEXT:    endbr64
; CHECK:         callq
; CHECK-NEXT:      R_X86_64_PLT32 use-0x4
; CHECK:         retq
; CHECK-NEXT:    endbr64
; CHECK-NEXT:    movq %rax,
define void @ibt(ptr %env) personality ptr @__gxx_personality_v0 {
  %r = call i32 @setjmp(ptr %env)
  invoke void @use(ptr @addrtaken) to label %cont unwind label %lpad
cont:
  ret void
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}

;; A frame below the probe size needs no probe.
; CHECK-LABEL: <smallframe>:
; CHECK:         subq $0x800, %rsp
define void @smallframe() "probe-stack"="inline-asm" {
  %buf = alloca [2040 x i8]
  call void @use(ptr %buf)
  ret void
}

; CHECK-LABEL: <bigframe>:
define void @bigframe() "probe-stack"="inline-asm" {
  %buf = alloca [8192 x i8]
  call void @use(ptr %buf)
  ret void
}

declare void @use(ptr)
declare i32 @setjmp(ptr) returns_twice
declare i32 @__gxx_personality_v0(...)

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 8, !"cf-protection-return", i32 1}
!1 = !{i32 8, !"cf-protection-branch", i32 1}
!2 = !{i32 4, !"probe-stack", !"inline-asm"}
