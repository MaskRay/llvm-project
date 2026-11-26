; RUN: llc -O0 -mtriple=aarch64-linux-gnu -relocation-model=pic -direct-codegen -direct-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-objdump -d -r --no-show-raw-insn %t.o | FileCheck %s
; RUN: llvm-readelf -s %t.o | FileCheck %s --check-prefix=SYM

;; The first instruction of the section gets a mapping symbol.
; SYM: 0000000000000000 0 NOTYPE LOCAL DEFAULT [[#]] $x

@g = global i32 0
@l = internal global i64 0
declare void @ext(ptr)
declare i32 @printf(ptr, ...)

;; A small frame is allocated by the store of the frame record. A value used
;; once in its block stays in a register.
; CHECK-LABEL: <arith>:
; CHECK-NEXT:    stp x29, x30, [sp, #-0x10]!
; CHECK-NEXT:    mov x29, sp
; CHECK-NEXT:    add w0, w0, w1
; CHECK-NEXT:    lsl w0, w0, #3
; CHECK-NEXT:    ldp x29, x30, [sp], #0x10
; CHECK-NEXT:    ret
define i32 @arith(i32 %a, i32 %b) {
  %s = add i32 %a, %b
  %r = shl i32 %s, 3
  ret i32 %r
}

;; A static alloca is addressed from x29. A global is reached through the GOT
;; unless it is local.
; CHECK-LABEL: <mem>:
; CHECK:         str w0, [x29, #0x10]
; CHECK-NEXT:    adrp x9, 0x0
; CHECK-NEXT:      R_AARCH64_ADR_GOT_PAGE g
; CHECK-NEXT:    ldr x9, [x9]
; CHECK-NEXT:      R_AARCH64_LD64_GOT_LO12_NC g
; CHECK-NEXT:    ldr w0, [x9]
; CHECK-NEXT:    adrp x9, 0x0
; CHECK-NEXT:      R_AARCH64_ADR_PREL_PG_HI21 .bss+0x8
; CHECK-NEXT:    add x9, x9, #0x0
; CHECK-NEXT:      R_AARCH64_ADD_ABS_LO12_NC .bss+0x8
; CHECK-NEXT:    ldr x9, [x9]
; CHECK:         add x0, x29, #0x10
; CHECK-NEXT:    bl
; CHECK-NEXT:      R_AARCH64_CALL26 ext
define i64 @mem(i32 %a) {
  %x = alloca i32
  store i32 %a, ptr %x
  %v = load i32, ptr @g
  %w = load i64, ptr @l
  %e = zext i32 %v to i64
  %s = add i64 %e, %w
  call void @ext(ptr %x)
  ret i64 %s
}

;; A compare is folded into its branch, and a PHI lives in a frame slot.
; CHECK-LABEL: <phi>:
; CHECK:         cmp w9, #0x5
; CHECK-NEXT:    b.ge
; CHECK:         cmn w9, #0x1
; CHECK:         ret
define i32 @phi(i32 %a) {
entry:
  %c = icmp slt i32 %a, 5
  br i1 %c, label %t, label %f
t:
  %x = mul i32 %a, 3
  br label %join
f:
  %d = icmp eq i32 %a, -1
  %y = zext i1 %d to i32
  br label %join
join:
  %p = phi i32 [ %x, %t ], [ %y, %f ]
  ret i32 %p
}

;; The ninth integer argument is on the stack of the caller, which x17 points
;; to after the prologue. Outgoing stack arguments are at sp.
; CHECK-LABEL: <stackargs>:
; CHECK-NEXT:    sub sp, sp, #0x30
; CHECK-NEXT:    stp x29, x30, [sp, #0x10]
; CHECK-NEXT:    add x29, sp, #0x10
; CHECK-NEXT:    add x17, x29, #0x20
; CHECK-NEXT:    ldr x16, [x17]
; CHECK-NEXT:    str x16, [x29, #0x10]
; CHECK:         str x9, [sp]
; CHECK:         bl
; CHECK-NEXT:      R_AARCH64_CALL26 stackargs
; CHECK:         ldp x29, x30, [sp, #0x10]
; CHECK-NEXT:    add sp, sp, #0x30
; CHECK-NEXT:    ret
define i64 @stackargs(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g, i64 %h, i64 %i) {
  %x = add i64 %i, 1
  %r = call i64 @stackargs(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g, i64 %h, i64 %x)
  %s = add i64 %r, %i
  ret i64 %s
}

;; va_start fills in the AAPCS64 va_list from the register save areas.
; CHECK-LABEL: <va>:
; CHECK:         str x7, [x29,
; CHECK:         str q7, [x29,
; CHECK:         mov w{{[0-9]+}}, #-0x38
; CHECK:         mov w{{[0-9]+}}, #-0x80
define void @va(i32 %n, ...) {
  %ap = alloca [32 x i8]
  call void @llvm.va_start(ptr %ap)
  call void @ext(ptr %ap)
  call void @llvm.va_end(ptr %ap)
  ret void
}

;; llvm.fmuladd is fused, as in the regular pipeline.
; CHECK-LABEL: <fp>:
; CHECK:         fmadd d0, d0, d1, d2
; CHECK:         fcmp d0, d16
; CHECK-NEXT:    cset w9, mi
; CHECK-NEXT:    csinc w9, w9, wzr, le
define i1 @fp(double %a, double %b, double %c) {
  %m = call double @llvm.fmuladd.f64(double %a, double %b, double %c)
  %r = fcmp one double %m, %b
  ret i1 %r
}

;; A second return branches to the epilogue at the end.
; CHECK-LABEL: <tworets>:
; CHECK:         b.eq
; CHECK-NEXT:    mov w0, #0x1
; CHECK-NEXT:    b
; CHECK-NEXT:    mov w0, #0x2
; CHECK-NEXT:    ldp x29, x30, [sp], #0x10
; CHECK-NEXT:    ret
define i32 @tworets(i1 %c) {
  br i1 %c, label %a, label %b
a:
  ret i32 1
b:
  ret i32 2
}

;; A frame of 4096 bytes or more takes two instructions to allocate, and an
;; offset beyond the range of the load/store forms goes through x16.
; CHECK-LABEL: <bigframe>:
; CHECK-NEXT:    sub sp, sp, #0x9, lsl #12
; CHECK-NEXT:    sub sp, sp, #0xc60
; CHECK-NEXT:    stp x29, x30, [sp]
; CHECK-NEXT:    mov x29, sp
; CHECK-NEXT:    mov w9, #0x1
; CHECK-NEXT:    add x16, x29, #0x9, lsl #12
; CHECK-NEXT:    add x16, x16, #0xc50
; CHECK-NEXT:    strb w9, [x16]
; CHECK:         ldp x29, x30, [sp]
; CHECK-NEXT:    add sp, sp, #0x9, lsl #12
; CHECK-NEXT:    add sp, sp, #0xc60
; CHECK-NEXT:    ret
define void @bigframe() {
  %a = alloca [40000 x i8]
  %b = alloca i8
  store i8 1, ptr %b
  call void @ext(ptr %a)
  ret void
}

;; With a stack protector, the guarded array is below the frame record. A
;; frame record too far above sp for stp/ldp is addressed through x16.
; CHECK-LABEL: <farrecord>:
; CHECK-NEXT:    sub sp, sp, #0x7f0
; CHECK-NEXT:    add x16, sp, #0x7e0
; CHECK-NEXT:    stp x29, x30, [x16]
; CHECK-NEXT:    mov x29, x16
; CHECK:         stur x16, [x29, #-0x8]
; CHECK-NEXT:    sub x0, x29, #0x7d8
; CHECK:         add x16, sp, #0x7e0
; CHECK-NEXT:    ldp x29, x30, [x16]
; CHECK-NEXT:    add sp, sp, #0x7f0
; CHECK-NEXT:    ret
define void @farrecord() sspstrong {
  %a = alloca [2000 x i8]
  call void @ext(ptr %a)
  ret void
}

;; The callee may rely on an i1 argument being zero-extended to 8 bits.
; CHECK-LABEL: <i1arg>:
; CHECK:         and w0, w0, #0x1
; CHECK-NEXT:    bl
; CHECK-NEXT:      R_AARCH64_CALL26 takesbool
define void @i1arg(i32 %x, ptr %p) {
  %t = trunc i32 %x to i1
  call void @takesbool(i1 %t, ptr %p)
  ret void
}
declare void @takesbool(i1, ptr)

;; "ssp" singles out no object here, but the guard must still be between the
;; array and the frame record.
; CHECK-LABEL: <sspbasic>:
; CHECK:         stur x16, [x29, #-0x8]
; CHECK-NEXT:    sub x0, x29, #0x1a0
define void @sspbasic() ssp {
  %a = alloca [100 x i32]
  %c = alloca [8 x i8]
  call void @ext(ptr %a)
  ret void
}

;; Constant-offset addresses that feed one another in a block are folded into
;; the load, and a compare with zero that feeds a branch is a cbz or cbnz.
; CHECK-LABEL: <chain>:
; CHECK:         ldr w0, [x0, #0x14]
; CHECK-NEXT:    cbnz w0,
; CHECK-LABEL: <zero>:
; CHECK:         cbz x0,
; CHECK-NEXT:    ldrb w9, [x29, #0x10]
; CHECK-NEXT:    cbz w9,
define i32 @chain(ptr %p) {
  %a = getelementptr { i64, { i64, { i32, i32 } } }, ptr %p, i32 0, i32 1
  %b = getelementptr { i64, { i32, i32 } }, ptr %a, i32 0, i32 1
  %c = getelementptr { i32, i32 }, ptr %b, i32 0, i32 1
  %v = load i32, ptr %c
  %z = icmp eq i32 %v, 0
  br i1 %z, label %t, label %f
t:
  ret i32 1
f:
  ret i32 2
}
define i32 @zero(ptr %p, i8 %x) {
  %z = icmp ne ptr %p, null
  br i1 %z, label %t, label %f
t:
  %y = icmp eq i8 %x, 0
  br i1 %y, label %f, label %u
u:
  ret i32 1
f:
  ret i32 2
}
