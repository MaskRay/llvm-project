; RUN: llc -O0 -mtriple=aarch64-linux-gnu -relocation-model=pic -direct-codegen -direct-codegen-report -filetype=obj %s -o %t.o 2>&1 | FileCheck %s --check-prefix=REPORT
; RUN: llvm-objdump -d -r --no-show-raw-insn %t.o | FileCheck %s
; RUN: llvm-dwarfdump --eh-frame %t.o | FileCheck %s --check-prefix=FRAME

;; Return address signing, branch target identification, the stack protector
;; and stack probes are handled rather than sent to the regular pipeline.

; REPORT-NOT: direct-codegen:
; REPORT:     direct-codegen: bkey: function attribute "sign-return-address-key"="b_key"
; REPORT-NEXT: direct-codegen: probegap: stack probe
; REPORT-NEXT: direct-codegen: bigframe: stack probe
; REPORT-NOT: direct-codegen:

;; The guard is right below the frame record and the array is below the guard;
;; other objects are above the frame record.
; CHECK-LABEL: <ssp>:
; CHECK-NEXT:    paciasp
; CHECK-NEXT:    sub sp, sp, #0x40
; CHECK-NEXT:    stp x29, x30, [sp, #0x20]
; CHECK-NEXT:    add x29, sp, #0x20
; CHECK-NEXT:    adrp x16, 0x0
; CHECK-NEXT:      R_AARCH64_ADR_GOT_PAGE __stack_chk_guard
; CHECK-NEXT:    ldr x16, [x16]
; CHECK-NEXT:      R_AARCH64_LD64_GOT_LO12_NC __stack_chk_guard
; CHECK-NEXT:    ldr x16, [x16]
; CHECK-NEXT:    stur x16, [x29, #-0x8]
; CHECK-NEXT:    mov w9, #0x7
; CHECK-NEXT:    str w9, [x29, #0x10]
; CHECK-NEXT:    sub x0, x29, #0x18
; CHECK-NEXT:    bl
; CHECK-NEXT:      R_AARCH64_CALL26 use
; CHECK-NEXT:    ldr w0, [x29, #0x10]
; CHECK-NEXT:    ldur x17, [x29, #-0x8]
; CHECK:         cmp x16, x17
; CHECK-NEXT:    b.eq
; CHECK-NEXT:    bl
; CHECK-NEXT:      R_AARCH64_CALL26 __stack_chk_fail
; CHECK-NEXT:    ldp x29, x30, [sp, #0x20]
; CHECK-NEXT:    add sp, sp, #0x40
; CHECK-NEXT:    autiasp
; CHECK-NEXT:    ret
define i32 @ssp() sspstrong "sign-return-address"="non-leaf" "sign-return-address-key"="a_key" {
  %x = alloca i32
  %buf = alloca [16 x i8]
  store i32 7, ptr %x
  call void @use(ptr %buf)
  %v = load i32, ptr %x
  ret i32 %v
}

; FRAME:      DW_CFA_advance_loc: 4 to 0x4
; FRAME-NEXT: DW_CFA_AARCH64_negate_ra_state:
; FRAME:      DW_CFA_restore: W29
; FRAME-NEXT: DW_CFA_advance_loc: 4
; FRAME-NEXT: DW_CFA_AARCH64_negate_ra_state:

;; A local function whose address is not taken cannot be called indirectly.
; CHECK-LABEL: <local>:
; CHECK-NEXT:    stp x29, x30
define internal void @local() "branch-target-enforcement" {
  ret void
}

; CHECK-LABEL: <addrtaken>:
; CHECK-NEXT:    bti c
define internal void @addrtaken() "branch-target-enforcement" {
  call void @local()
  ret void
}

;; setjmp returns through an indirect branch, and so does the unwinder to a
;; landing pad.
; CHECK-LABEL: <bti>:
; CHECK-NEXT:    bti c
; CHECK:           R_AARCH64_CALL26 setjmp
; CHECK-NEXT:    bti j
; CHECK:           R_AARCH64_CALL26 use
; CHECK-NEXT:    b
; CHECK-NEXT:    bti j
; CHECK-NEXT:    str x0, [x29,
define void @bti(ptr %env) "branch-target-enforcement" personality ptr @__gxx_personality_v0 {
  %r = call i32 @setjmp(ptr %env) returns_twice
  invoke void @use(ptr @addrtaken) to label %cont unwind label %lpad
cont:
  ret void
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}

; CHECK-LABEL: <bkey>:
define void @bkey() "sign-return-address"="all" "sign-return-address-key"="b_key" {
  ret void
}

;; A frame below the probe size needs no probe: the store of the frame record
;; touches it.
; CHECK-LABEL: <smallframe>:
; CHECK-NEXT:    sub sp, sp, #0x810
; CHECK-NEXT:    stp x29, x30, [sp]
define void @smallframe() "probe-stack"="inline-asm" {
  %buf = alloca [2040 x i8]
  call void @use(ptr %buf)
  ret void
}

;; The store of the frame record would be more than 4096 - 1024 bytes below
;; the incoming sp.
; CHECK-LABEL: <probegap>:
define void @probegap() "probe-stack"="inline-asm" {
  %buf = alloca [4000 x i8]
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

!llvm.module.flags = !{!0, !1, !2, !3}
!0 = !{i32 8, !"branch-target-enforcement", i32 2}
!1 = !{i32 8, !"sign-return-address", i32 2}
!2 = !{i32 8, !"guarded-control-stack", i32 2}
!3 = !{i32 4, !"probe-stack", !"inline-asm"}
