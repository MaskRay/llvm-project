; RUN: llc -O0 -mtriple=x86_64-linux-gnu -relocation-model=pic -direct-codegen -filetype=obj %s -o %t.o
; RUN: llvm-objdump -d -r --no-show-raw-insn %t.o | FileCheck %s
; RUN: llvm-readelf -s %t.o | FileCheck %s --check-prefix=SYM

;; Functions the direct code generator supports are emitted directly. Others
;; take the regular pipeline. Both kinds end up in the object in module order.

declare i32 @ext(i32, ptr)
@g = global i32 0

define i32 @direct(i32 %a, i32 %b) uwtable {
entry:
  %a.addr = alloca i32
  %b.addr = alloca i32
  store i32 %a, ptr %a.addr
  store i32 %b, ptr %b.addr
  %0 = load i32, ptr %a.addr
  %1 = load i32, ptr %b.addr
  %cmp = icmp slt i32 %0, %1
  br i1 %cmp, label %then, label %else

then:
  %2 = load i32, ptr %a.addr
  %add = add i32 %2, 7
  br label %end

else:
  %3 = load i32, ptr %b.addr
  %call = call i32 @ext(i32 %3, ptr @g)
  br label %end

end:
  %r = phi i32 [ %add, %then ], [ %call, %else ]
  ret i32 %r
}

define x86_fp80 @fallback(x86_fp80 %x) {
  %r = fadd x86_fp80 %x, %x
  ret x86_fp80 %r
}

define i64 @after(i64 %x) {
  %r = shl i64 %x, 3
  ret i64 %r
}

;; Signed i1 compares see true as -1.
define i32 @slt_i1(i32 %x) {
  %b = trunc i32 %x to i1
  %c = icmp slt i1 %b, true
  %r = zext i1 %c to i32
  ret i32 %r
}

;; An i1 stored to memory is 0 or 1, also inside an aggregate.
define void @store_agg_i1(i32 %x, ptr %p) {
  %b = trunc i32 %x to i1
  %a = insertvalue { i1, i64 } poison, i1 %b, 0
  %a2 = insertvalue { i1, i64 } %a, i64 7, 1
  store { i1, i64 } %a2, ptr %p
  ret void
}

;; A load used only by the next instruction is folded into a compare or a
;; sign extension.
define i32 @fold_load(ptr %p, ptr %q) {
  %v = load i32, ptr %p
  %c = icmp eq i32 %v, 0
  br i1 %c, label %t, label %f
t:
  %w = load i16, ptr %q
  %s = sext i16 %w to i32
  ret i32 %s
f:
  ret i32 1
}

;; A block laid out before the definition of a frame address reads it from
;; its slot, so the definition stores it there.
define i64 @gep_use_first(i64 %x) {
entry:
  %a = alloca [4 x i64]
  br label %def
use:
  %v = load i64, ptr %g
  ret i64 %v
def:
  %g = getelementptr i64, ptr %a, i64 1
  store i64 %x, ptr %g
  br label %use
}

;; memset takes its byte as an int.
define void @memset_byte(ptr %p, i32 %x, i64 %n) {
  %v = trunc i32 %x to i8
  call void @llvm.memset.p0.i64(ptr %p, i8 %v, i64 %n, i1 false)
  ret void
}

;; A value with several uses is stored once and stays in its register until
;; that register is needed. A constant offset from a frame object is folded
;; into each address.
define i32 @reuse(i32 %a) {
  %s = alloca { i32, i32 }
  %x = add i32 %a, 1
  %f = getelementptr { i32, i32 }, ptr %s, i32 0, i32 1
  store i32 %x, ptr %f
  %v = load i32, ptr %f
  %y = mul i32 %x, %v
  %z = add i32 %y, %x
  ret i32 %z
}

;; As FastISel, a branch alone in its block is kept even to the next block, so
;; that its line has a location. A fall-through after other code needs none.
define i32 @lone_br(i32 %x) {
entry:
  br label %next
next:
  %y = add i32 %x, 1
  br label %done
done:
  ret i32 %y
}

;; Constant-offset addresses that feed one another in a block are folded into
;; the memory operand of the load.
define i32 @chain(ptr %p) {
  %a = getelementptr { i64, { i64, { i32, i32 } } }, ptr %p, i32 0, i32 1
  %b = getelementptr { i64, { i32, i32 } }, ptr %a, i32 0, i32 1
  %c = getelementptr { i32, i32 }, ptr %b, i32 0, i32 1
  %v = load i32, ptr %c
  %r = add i32 %v, 1
  ret i32 %r
}

; CHECK-LABEL: <direct>:
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    movq %rsp, %rbp
; CHECK-NEXT:    subq $0x10, %rsp
; CHECK-NEXT:    movl %edi, -0x4(%rbp)
; CHECK-NEXT:    movl %esi, -0x8(%rbp)
; CHECK-NEXT:    movl -0x4(%rbp), %eax
; CHECK-NEXT:    movl -0x8(%rbp), %r10d
; CHECK-NEXT:    cmpl %r10d, %eax
; CHECK-NEXT:    jge {{.*}} <direct+0x26>
; CHECK-NEXT:    movl -0x4(%rbp), %eax
; CHECK-NEXT:    addl $0x7, %eax
; CHECK-NEXT:    movq %rax, -0x10(%rbp)
; CHECK-NEXT:    jmp {{.*}} <direct+0x39>
; CHECK-NEXT:    movl -0x8(%rbp), %edi
; CHECK-NEXT:    movq (%rip), %rsi
; CHECK-NEXT:      R_X86_64_REX_GOTPCRELX g-0x4
; CHECK-NEXT:    callq
; CHECK-NEXT:      R_X86_64_PLT32 ext-0x4
; CHECK-NEXT:    movq %rax, -0x10(%rbp)
; CHECK-NEXT:    movl -0x10(%rbp), %eax
; CHECK-NEXT:    leave
; CHECK-NEXT:    retq

; CHECK-LABEL: <fallback>:
; CHECK-NEXT:    fldt 0x8(%rsp)

;; A leaf function with no frame uses the red zone and does not adjust rsp.
; CHECK-LABEL: <after>:
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    movq %rsp, %rbp
; CHECK-NEXT:    shlq $0x3, %rdi
; CHECK-NEXT:    movq %rdi, %rax
; CHECK-NEXT:    leave
; CHECK-NEXT:    retq

; CHECK-LABEL: <slt_i1>:
; CHECK:         andl $0x1, %edi
; CHECK-NEXT:    negl %edi
; CHECK-NEXT:    cmpl $-0x1, %edi
; CHECK-NEXT:    setl %al

; CHECK-LABEL: <store_agg_i1>:
; CHECK:         movzbl -0x20(%rbp), %eax
; CHECK-NEXT:    andl $0x1, %eax
; CHECK-NEXT:    movb %al, (%rsi)

; CHECK-LABEL: <fold_load>:
; CHECK:         cmpl $0x0, (%rdi)
; CHECK-NEXT:    jne
; CHECK-NEXT:    movq -0x8(%rbp), %rax
; CHECK-NEXT:    movswl (%rax), %eax

; CHECK-LABEL: <gep_use_first>:
; CHECK:         jmp
; CHECK-NEXT:    movq -0x30(%rbp), %rax
; CHECK:         leaq -0x20(%rbp), %rax
; CHECK-NEXT:    movq %rax, -0x30(%rbp)

; CHECK-LABEL: <memset_byte>:
; CHECK:         movzbl %sil, %esi
; CHECK-NEXT:    callq
; CHECK-NEXT:      R_X86_64_PLT32 memset-0x4

; CHECK-LABEL: <reuse>:
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    movq %rsp, %rbp
; CHECK-NEXT:    addl $0x1, %edi
; CHECK-NEXT:    movq %rdi, -0x10(%rbp)
; CHECK-NEXT:    movl %edi, -0x4(%rbp)
; CHECK-NEXT:    movl -0x4(%rbp), %eax
; CHECK-NEXT:    movl -0x10(%rbp), %r10d
; CHECK-NEXT:    imull %eax, %r10d

; CHECK-LABEL: <lone_br>:
; CHECK:         jmp 0x[[#%x,NEXT:]] <lone_br+0xa>
; CHECK-NEXT:    [[#%x,NEXT]]: movl -0x8(%rbp), %eax
; CHECK-NEXT:    addl $0x1, %eax
; CHECK-NEXT:    movq %rax, -0x10(%rbp)
; CHECK-NEXT:    movl -0x10(%rbp), %eax
; CHECK-NEXT:    leave

; CHECK-LABEL: <chain>:
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    movq %rsp, %rbp
; CHECK-NEXT:    movl 0x14(%rdi), %edi
; CHECK-NEXT:    addl $0x1, %edi

; SYM:      FUNC    GLOBAL DEFAULT {{.*}} direct
; SYM:      NOTYPE  GLOBAL DEFAULT   UND ext
; SYM:      FUNC    GLOBAL DEFAULT {{.*}} fallback
; SYM:      FUNC    GLOBAL DEFAULT {{.*}} after
