; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=aarch64-unknown-linux-gnu < %s | FileCheck %s

; These test cases are inspired by C++2a std::midpoint().
; See https://bugs.llvm.org/show_bug.cgi?id=40965

; ---------------------------------------------------------------------------- ;
; 32-bit width
; ---------------------------------------------------------------------------- ;

; Values come from regs

define i32 @scalar_i32_signed_reg_reg(i32 %a1, i32 %a2) nounwind {
; CHECK-LABEL: scalar_i32_signed_reg_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    subs w9, w0, w1
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    cneg w9, w9, le
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %t3 = icmp sgt i32 %a1, %a2 ; signed
  %t4 = select i1 %t3, i32 -1, i32 1
  %t5 = select i1 %t3, i32 %a2, i32 %a1
  %t6 = select i1 %t3, i32 %a1, i32 %a2
  %t7 = sub i32 %t6, %t5
  %t8 = lshr i32 %t7, 1
  %t9 = mul nsw i32 %t8, %t4 ; signed
  %a10 = add nsw i32 %t9, %a1 ; signed
  ret i32 %a10
}

define i32 @scalar_i32_unsigned_reg_reg(i32 %a1, i32 %a2) nounwind {
; CHECK-LABEL: scalar_i32_unsigned_reg_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    subs w9, w0, w1
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    cneg w9, w9, ls
; CHECK-NEXT:    cneg w8, w8, ls
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %t3 = icmp ugt i32 %a1, %a2
  %t4 = select i1 %t3, i32 -1, i32 1
  %t5 = select i1 %t3, i32 %a2, i32 %a1
  %t6 = select i1 %t3, i32 %a1, i32 %a2
  %t7 = sub i32 %t6, %t5
  %t8 = lshr i32 %t7, 1
  %t9 = mul i32 %t8, %t4
  %a10 = add i32 %t9, %a1
  ret i32 %a10
}

; Values are loaded. Only check signed case.

define i32 @scalar_i32_signed_mem_reg(ptr %a1_addr, i32 %a2) nounwind {
; CHECK-LABEL: scalar_i32_signed_mem_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ldr w9, [x0]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w10, w9, w1
; CHECK-NEXT:    cneg w10, w10, le
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w10, w10, #1
; CHECK-NEXT:    madd w0, w10, w8, w9
; CHECK-NEXT:    ret
  %a1 = load i32, ptr %a1_addr
  %t3 = icmp sgt i32 %a1, %a2 ; signed
  %t4 = select i1 %t3, i32 -1, i32 1
  %t5 = select i1 %t3, i32 %a2, i32 %a1
  %t6 = select i1 %t3, i32 %a1, i32 %a2
  %t7 = sub i32 %t6, %t5
  %t8 = lshr i32 %t7, 1
  %t9 = mul nsw i32 %t8, %t4 ; signed
  %a10 = add nsw i32 %t9, %a1 ; signed
  ret i32 %a10
}

define i32 @scalar_i32_signed_reg_mem(i32 %a1, ptr %a2_addr) nounwind {
; CHECK-LABEL: scalar_i32_signed_reg_mem:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ldr w9, [x1]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w0, w9
; CHECK-NEXT:    cneg w9, w9, le
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %a2 = load i32, ptr %a2_addr
  %t3 = icmp sgt i32 %a1, %a2 ; signed
  %t4 = select i1 %t3, i32 -1, i32 1
  %t5 = select i1 %t3, i32 %a2, i32 %a1
  %t6 = select i1 %t3, i32 %a1, i32 %a2
  %t7 = sub i32 %t6, %t5
  %t8 = lshr i32 %t7, 1
  %t9 = mul nsw i32 %t8, %t4 ; signed
  %a10 = add nsw i32 %t9, %a1 ; signed
  ret i32 %a10
}

define i32 @scalar_i32_signed_mem_mem(ptr %a1_addr, ptr %a2_addr) nounwind {
; CHECK-LABEL: scalar_i32_signed_mem_mem:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ldr w9, [x0]
; CHECK-NEXT:    ldr w10, [x1]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w10, w9, w10
; CHECK-NEXT:    cneg w10, w10, le
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w10, w10, #1
; CHECK-NEXT:    madd w0, w10, w8, w9
; CHECK-NEXT:    ret
  %a1 = load i32, ptr %a1_addr
  %a2 = load i32, ptr %a2_addr
  %t3 = icmp sgt i32 %a1, %a2 ; signed
  %t4 = select i1 %t3, i32 -1, i32 1
  %t5 = select i1 %t3, i32 %a2, i32 %a1
  %t6 = select i1 %t3, i32 %a1, i32 %a2
  %t7 = sub i32 %t6, %t5
  %t8 = lshr i32 %t7, 1
  %t9 = mul nsw i32 %t8, %t4 ; signed
  %a10 = add nsw i32 %t9, %a1 ; signed
  ret i32 %a10
}

; ---------------------------------------------------------------------------- ;
; 64-bit width
; ---------------------------------------------------------------------------- ;

; Values come from regs

define i64 @scalar_i64_signed_reg_reg(i64 %a1, i64 %a2) nounwind {
; CHECK-LABEL: scalar_i64_signed_reg_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    subs x9, x0, x1
; CHECK-NEXT:    mov x8, #-1 // =0xffffffffffffffff
; CHECK-NEXT:    cneg x9, x9, le
; CHECK-NEXT:    cneg x8, x8, le
; CHECK-NEXT:    lsr x9, x9, #1
; CHECK-NEXT:    madd x0, x9, x8, x0
; CHECK-NEXT:    ret
  %t3 = icmp sgt i64 %a1, %a2 ; signed
  %t4 = select i1 %t3, i64 -1, i64 1
  %t5 = select i1 %t3, i64 %a2, i64 %a1
  %t6 = select i1 %t3, i64 %a1, i64 %a2
  %t7 = sub i64 %t6, %t5
  %t8 = lshr i64 %t7, 1
  %t9 = mul nsw i64 %t8, %t4 ; signed
  %a10 = add nsw i64 %t9, %a1 ; signed
  ret i64 %a10
}

define i64 @scalar_i64_unsigned_reg_reg(i64 %a1, i64 %a2) nounwind {
; CHECK-LABEL: scalar_i64_unsigned_reg_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    subs x9, x0, x1
; CHECK-NEXT:    mov x8, #-1 // =0xffffffffffffffff
; CHECK-NEXT:    cneg x9, x9, ls
; CHECK-NEXT:    cneg x8, x8, ls
; CHECK-NEXT:    lsr x9, x9, #1
; CHECK-NEXT:    madd x0, x9, x8, x0
; CHECK-NEXT:    ret
  %t3 = icmp ugt i64 %a1, %a2
  %t4 = select i1 %t3, i64 -1, i64 1
  %t5 = select i1 %t3, i64 %a2, i64 %a1
  %t6 = select i1 %t3, i64 %a1, i64 %a2
  %t7 = sub i64 %t6, %t5
  %t8 = lshr i64 %t7, 1
  %t9 = mul i64 %t8, %t4
  %a10 = add i64 %t9, %a1
  ret i64 %a10
}

; Values are loaded. Only check signed case.

define i64 @scalar_i64_signed_mem_reg(ptr %a1_addr, i64 %a2) nounwind {
; CHECK-LABEL: scalar_i64_signed_mem_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ldr x9, [x0]
; CHECK-NEXT:    mov x8, #-1 // =0xffffffffffffffff
; CHECK-NEXT:    subs x10, x9, x1
; CHECK-NEXT:    cneg x10, x10, le
; CHECK-NEXT:    cneg x8, x8, le
; CHECK-NEXT:    lsr x10, x10, #1
; CHECK-NEXT:    madd x0, x10, x8, x9
; CHECK-NEXT:    ret
  %a1 = load i64, ptr %a1_addr
  %t3 = icmp sgt i64 %a1, %a2 ; signed
  %t4 = select i1 %t3, i64 -1, i64 1
  %t5 = select i1 %t3, i64 %a2, i64 %a1
  %t6 = select i1 %t3, i64 %a1, i64 %a2
  %t7 = sub i64 %t6, %t5
  %t8 = lshr i64 %t7, 1
  %t9 = mul nsw i64 %t8, %t4 ; signed
  %a10 = add nsw i64 %t9, %a1 ; signed
  ret i64 %a10
}

define i64 @scalar_i64_signed_reg_mem(i64 %a1, ptr %a2_addr) nounwind {
; CHECK-LABEL: scalar_i64_signed_reg_mem:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ldr x9, [x1]
; CHECK-NEXT:    mov x8, #-1 // =0xffffffffffffffff
; CHECK-NEXT:    subs x9, x0, x9
; CHECK-NEXT:    cneg x9, x9, le
; CHECK-NEXT:    cneg x8, x8, le
; CHECK-NEXT:    lsr x9, x9, #1
; CHECK-NEXT:    madd x0, x9, x8, x0
; CHECK-NEXT:    ret
  %a2 = load i64, ptr %a2_addr
  %t3 = icmp sgt i64 %a1, %a2 ; signed
  %t4 = select i1 %t3, i64 -1, i64 1
  %t5 = select i1 %t3, i64 %a2, i64 %a1
  %t6 = select i1 %t3, i64 %a1, i64 %a2
  %t7 = sub i64 %t6, %t5
  %t8 = lshr i64 %t7, 1
  %t9 = mul nsw i64 %t8, %t4 ; signed
  %a10 = add nsw i64 %t9, %a1 ; signed
  ret i64 %a10
}

define i64 @scalar_i64_signed_mem_mem(ptr %a1_addr, ptr %a2_addr) nounwind {
; CHECK-LABEL: scalar_i64_signed_mem_mem:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ldr x9, [x0]
; CHECK-NEXT:    ldr x10, [x1]
; CHECK-NEXT:    mov x8, #-1 // =0xffffffffffffffff
; CHECK-NEXT:    subs x10, x9, x10
; CHECK-NEXT:    cneg x10, x10, le
; CHECK-NEXT:    cneg x8, x8, le
; CHECK-NEXT:    lsr x10, x10, #1
; CHECK-NEXT:    madd x0, x10, x8, x9
; CHECK-NEXT:    ret
  %a1 = load i64, ptr %a1_addr
  %a2 = load i64, ptr %a2_addr
  %t3 = icmp sgt i64 %a1, %a2 ; signed
  %t4 = select i1 %t3, i64 -1, i64 1
  %t5 = select i1 %t3, i64 %a2, i64 %a1
  %t6 = select i1 %t3, i64 %a1, i64 %a2
  %t7 = sub i64 %t6, %t5
  %t8 = lshr i64 %t7, 1
  %t9 = mul nsw i64 %t8, %t4 ; signed
  %a10 = add nsw i64 %t9, %a1 ; signed
  ret i64 %a10
}

; ---------------------------------------------------------------------------- ;
; 16-bit width
; ---------------------------------------------------------------------------- ;

; Values come from regs

define i16 @scalar_i16_signed_reg_reg(i16 %a1, i16 %a2) nounwind {
; CHECK-LABEL: scalar_i16_signed_reg_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    sxth w9, w1
; CHECK-NEXT:    sxth w10, w0
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w10, w9
; CHECK-NEXT:    cneg w9, w9, mi
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %t3 = icmp sgt i16 %a1, %a2 ; signed
  %t4 = select i1 %t3, i16 -1, i16 1
  %t5 = select i1 %t3, i16 %a2, i16 %a1
  %t6 = select i1 %t3, i16 %a1, i16 %a2
  %t7 = sub i16 %t6, %t5
  %t8 = lshr i16 %t7, 1
  %t9 = mul nsw i16 %t8, %t4 ; signed
  %a10 = add nsw i16 %t9, %a1 ; signed
  ret i16 %a10
}

define i16 @scalar_i16_unsigned_reg_reg(i16 %a1, i16 %a2) nounwind {
; CHECK-LABEL: scalar_i16_unsigned_reg_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and w9, w1, #0xffff
; CHECK-NEXT:    and w10, w0, #0xffff
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w10, w9
; CHECK-NEXT:    cneg w9, w9, mi
; CHECK-NEXT:    cneg w8, w8, ls
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %t3 = icmp ugt i16 %a1, %a2
  %t4 = select i1 %t3, i16 -1, i16 1
  %t5 = select i1 %t3, i16 %a2, i16 %a1
  %t6 = select i1 %t3, i16 %a1, i16 %a2
  %t7 = sub i16 %t6, %t5
  %t8 = lshr i16 %t7, 1
  %t9 = mul i16 %t8, %t4
  %a10 = add i16 %t9, %a1
  ret i16 %a10
}

; Values are loaded. Only check signed case.

define i16 @scalar_i16_signed_mem_reg(ptr %a1_addr, i16 %a2) nounwind {
; CHECK-LABEL: scalar_i16_signed_mem_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    sxth w9, w1
; CHECK-NEXT:    ldrsh w10, [x0]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w10, w9
; CHECK-NEXT:    cneg w9, w9, mi
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w10
; CHECK-NEXT:    ret
  %a1 = load i16, ptr %a1_addr
  %t3 = icmp sgt i16 %a1, %a2 ; signed
  %t4 = select i1 %t3, i16 -1, i16 1
  %t5 = select i1 %t3, i16 %a2, i16 %a1
  %t6 = select i1 %t3, i16 %a1, i16 %a2
  %t7 = sub i16 %t6, %t5
  %t8 = lshr i16 %t7, 1
  %t9 = mul nsw i16 %t8, %t4 ; signed
  %a10 = add nsw i16 %t9, %a1 ; signed
  ret i16 %a10
}

define i16 @scalar_i16_signed_reg_mem(i16 %a1, ptr %a2_addr) nounwind {
; CHECK-LABEL: scalar_i16_signed_reg_mem:
; CHECK:       // %bb.0:
; CHECK-NEXT:    sxth w9, w0
; CHECK-NEXT:    ldrsh w10, [x1]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w9, w10
; CHECK-NEXT:    cneg w9, w9, mi
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %a2 = load i16, ptr %a2_addr
  %t3 = icmp sgt i16 %a1, %a2 ; signed
  %t4 = select i1 %t3, i16 -1, i16 1
  %t5 = select i1 %t3, i16 %a2, i16 %a1
  %t6 = select i1 %t3, i16 %a1, i16 %a2
  %t7 = sub i16 %t6, %t5
  %t8 = lshr i16 %t7, 1
  %t9 = mul nsw i16 %t8, %t4 ; signed
  %a10 = add nsw i16 %t9, %a1 ; signed
  ret i16 %a10
}

define i16 @scalar_i16_signed_mem_mem(ptr %a1_addr, ptr %a2_addr) nounwind {
; CHECK-LABEL: scalar_i16_signed_mem_mem:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ldrsh w9, [x0]
; CHECK-NEXT:    ldrsh w10, [x1]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w10, w9, w10
; CHECK-NEXT:    cneg w10, w10, mi
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w10, w10, #1
; CHECK-NEXT:    madd w0, w10, w8, w9
; CHECK-NEXT:    ret
  %a1 = load i16, ptr %a1_addr
  %a2 = load i16, ptr %a2_addr
  %t3 = icmp sgt i16 %a1, %a2 ; signed
  %t4 = select i1 %t3, i16 -1, i16 1
  %t5 = select i1 %t3, i16 %a2, i16 %a1
  %t6 = select i1 %t3, i16 %a1, i16 %a2
  %t7 = sub i16 %t6, %t5
  %t8 = lshr i16 %t7, 1
  %t9 = mul nsw i16 %t8, %t4 ; signed
  %a10 = add nsw i16 %t9, %a1 ; signed
  ret i16 %a10
}

; ---------------------------------------------------------------------------- ;
; 8-bit width
; ---------------------------------------------------------------------------- ;

; Values come from regs

define i8 @scalar_i8_signed_reg_reg(i8 %a1, i8 %a2) nounwind {
; CHECK-LABEL: scalar_i8_signed_reg_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    sxtb w9, w1
; CHECK-NEXT:    sxtb w10, w0
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w10, w9
; CHECK-NEXT:    cneg w9, w9, mi
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %t3 = icmp sgt i8 %a1, %a2 ; signed
  %t4 = select i1 %t3, i8 -1, i8 1
  %t5 = select i1 %t3, i8 %a2, i8 %a1
  %t6 = select i1 %t3, i8 %a1, i8 %a2
  %t7 = sub i8 %t6, %t5
  %t8 = lshr i8 %t7, 1
  %t9 = mul nsw i8 %t8, %t4 ; signed
  %a10 = add nsw i8 %t9, %a1 ; signed
  ret i8 %a10
}

define i8 @scalar_i8_unsigned_reg_reg(i8 %a1, i8 %a2) nounwind {
; CHECK-LABEL: scalar_i8_unsigned_reg_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and w9, w1, #0xff
; CHECK-NEXT:    and w10, w0, #0xff
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w10, w9
; CHECK-NEXT:    cneg w9, w9, mi
; CHECK-NEXT:    cneg w8, w8, ls
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %t3 = icmp ugt i8 %a1, %a2
  %t4 = select i1 %t3, i8 -1, i8 1
  %t5 = select i1 %t3, i8 %a2, i8 %a1
  %t6 = select i1 %t3, i8 %a1, i8 %a2
  %t7 = sub i8 %t6, %t5
  %t8 = lshr i8 %t7, 1
  %t9 = mul i8 %t8, %t4
  %a10 = add i8 %t9, %a1
  ret i8 %a10
}

; Values are loaded. Only check signed case.

define i8 @scalar_i8_signed_mem_reg(ptr %a1_addr, i8 %a2) nounwind {
; CHECK-LABEL: scalar_i8_signed_mem_reg:
; CHECK:       // %bb.0:
; CHECK-NEXT:    sxtb w9, w1
; CHECK-NEXT:    ldrsb w10, [x0]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w10, w9
; CHECK-NEXT:    cneg w9, w9, mi
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w10
; CHECK-NEXT:    ret
  %a1 = load i8, ptr %a1_addr
  %t3 = icmp sgt i8 %a1, %a2 ; signed
  %t4 = select i1 %t3, i8 -1, i8 1
  %t5 = select i1 %t3, i8 %a2, i8 %a1
  %t6 = select i1 %t3, i8 %a1, i8 %a2
  %t7 = sub i8 %t6, %t5
  %t8 = lshr i8 %t7, 1
  %t9 = mul nsw i8 %t8, %t4 ; signed
  %a10 = add nsw i8 %t9, %a1 ; signed
  ret i8 %a10
}

define i8 @scalar_i8_signed_reg_mem(i8 %a1, ptr %a2_addr) nounwind {
; CHECK-LABEL: scalar_i8_signed_reg_mem:
; CHECK:       // %bb.0:
; CHECK-NEXT:    sxtb w9, w0
; CHECK-NEXT:    ldrsb w10, [x1]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w9, w9, w10
; CHECK-NEXT:    cneg w9, w9, mi
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w9, w9, #1
; CHECK-NEXT:    madd w0, w9, w8, w0
; CHECK-NEXT:    ret
  %a2 = load i8, ptr %a2_addr
  %t3 = icmp sgt i8 %a1, %a2 ; signed
  %t4 = select i1 %t3, i8 -1, i8 1
  %t5 = select i1 %t3, i8 %a2, i8 %a1
  %t6 = select i1 %t3, i8 %a1, i8 %a2
  %t7 = sub i8 %t6, %t5
  %t8 = lshr i8 %t7, 1
  %t9 = mul nsw i8 %t8, %t4 ; signed
  %a10 = add nsw i8 %t9, %a1 ; signed
  ret i8 %a10
}

define i8 @scalar_i8_signed_mem_mem(ptr %a1_addr, ptr %a2_addr) nounwind {
; CHECK-LABEL: scalar_i8_signed_mem_mem:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ldrsb w9, [x0]
; CHECK-NEXT:    ldrsb w10, [x1]
; CHECK-NEXT:    mov w8, #-1 // =0xffffffff
; CHECK-NEXT:    subs w10, w9, w10
; CHECK-NEXT:    cneg w10, w10, mi
; CHECK-NEXT:    cneg w8, w8, le
; CHECK-NEXT:    lsr w10, w10, #1
; CHECK-NEXT:    madd w0, w10, w8, w9
; CHECK-NEXT:    ret
  %a1 = load i8, ptr %a1_addr
  %a2 = load i8, ptr %a2_addr
  %t3 = icmp sgt i8 %a1, %a2 ; signed
  %t4 = select i1 %t3, i8 -1, i8 1
  %t5 = select i1 %t3, i8 %a2, i8 %a1
  %t6 = select i1 %t3, i8 %a1, i8 %a2
  %t7 = sub i8 %t6, %t5
  %t8 = lshr i8 %t7, 1
  %t9 = mul nsw i8 %t8, %t4 ; signed
  %a10 = add nsw i8 %t9, %a1 ; signed
  ret i8 %a10
}
