; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=aarch64 -mattr=+sve -o - | FileCheck %s
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"

;============ v2f32

define <vscale x 2 x float> @test_copysign_v2f32_v2f32(<vscale x 2 x float> %a, <vscale x 2 x float> %b) #0 {
; CHECK-LABEL: test_copysign_v2f32_v2f32:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and z1.s, z1.s, #0x80000000
; CHECK-NEXT:    and z0.s, z0.s, #0x7fffffff
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %r = call <vscale x 2 x float> @llvm.copysign.v2f32(<vscale x 2 x float> %a, <vscale x 2 x float> %b)
  ret <vscale x 2 x float> %r
}

define <vscale x 2 x float> @test_copysign_v2f32_v2f64(<vscale x 2 x float> %a, <vscale x 2 x double> %b) #0 {
; CHECK-LABEL: test_copysign_v2f32_v2f64:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ptrue p0.d
; CHECK-NEXT:    and z0.s, z0.s, #0x7fffffff
; CHECK-NEXT:    fcvt z1.s, p0/m, z1.d
; CHECK-NEXT:    and z1.s, z1.s, #0x80000000
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %tmp0 = fptrunc <vscale x 2 x double> %b to <vscale x 2 x float>
  %r = call <vscale x 2 x float> @llvm.copysign.v2f32(<vscale x 2 x float> %a, <vscale x 2 x float> %tmp0)
  ret <vscale x 2 x float> %r
}

declare <vscale x 2 x float> @llvm.copysign.v2f32(<vscale x 2 x float> %a, <vscale x 2 x float> %b) #0

;============ v4f32

define <vscale x 4 x float> @test_copysign_v4f32_v4f32(<vscale x 4 x float> %a, <vscale x 4 x float> %b) #0 {
; CHECK-LABEL: test_copysign_v4f32_v4f32:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and z1.s, z1.s, #0x80000000
; CHECK-NEXT:    and z0.s, z0.s, #0x7fffffff
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %r = call <vscale x 4 x float> @llvm.copysign.v4f32(<vscale x 4 x float> %a, <vscale x 4 x float> %b)
  ret <vscale x 4 x float> %r
}

; SplitVecOp #1
define <vscale x 4 x float> @test_copysign_v4f32_v4f64(<vscale x 4 x float> %a, <vscale x 4 x double> %b) #0 {
; CHECK-LABEL: test_copysign_v4f32_v4f64:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ptrue p0.d
; CHECK-NEXT:    and z0.s, z0.s, #0x7fffffff
; CHECK-NEXT:    fcvt z2.s, p0/m, z2.d
; CHECK-NEXT:    fcvt z1.s, p0/m, z1.d
; CHECK-NEXT:    uzp1 z1.s, z1.s, z2.s
; CHECK-NEXT:    and z1.s, z1.s, #0x80000000
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %tmp0 = fptrunc <vscale x 4 x double> %b to <vscale x 4 x float>
  %r = call <vscale x 4 x float> @llvm.copysign.v4f32(<vscale x 4 x float> %a, <vscale x 4 x float> %tmp0)
  ret <vscale x 4 x float> %r
}

declare <vscale x 4 x float> @llvm.copysign.v4f32(<vscale x 4 x float> %a, <vscale x 4 x float> %b) #0

;============ v2f64

define <vscale x 2 x double> @test_copysign_v2f64_v232(<vscale x 2 x double> %a, <vscale x 2 x float> %b) #0 {
; CHECK-LABEL: test_copysign_v2f64_v232:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ptrue p0.d
; CHECK-NEXT:    and z0.d, z0.d, #0x7fffffffffffffff
; CHECK-NEXT:    fcvt z1.d, p0/m, z1.s
; CHECK-NEXT:    and z1.d, z1.d, #0x8000000000000000
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %tmp0 = fpext <vscale x 2 x float> %b to <vscale x 2 x double>
  %r = call <vscale x 2 x double> @llvm.copysign.v2f64(<vscale x 2 x double> %a, <vscale x 2 x double> %tmp0)
  ret <vscale x 2 x double> %r
}

define <vscale x 2 x double> @test_copysign_v2f64_v2f64(<vscale x 2 x double> %a, <vscale x 2 x double> %b) #0 {
; CHECK-LABEL: test_copysign_v2f64_v2f64:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and z1.d, z1.d, #0x8000000000000000
; CHECK-NEXT:    and z0.d, z0.d, #0x7fffffffffffffff
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %r = call <vscale x 2 x double> @llvm.copysign.v2f64(<vscale x 2 x double> %a, <vscale x 2 x double> %b)
  ret <vscale x 2 x double> %r
}

declare <vscale x 2 x double> @llvm.copysign.v2f64(<vscale x 2 x double> %a, <vscale x 2 x double> %b) #0

;============ v4f64

; SplitVecRes mismatched
define <vscale x 4 x double> @test_copysign_v4f64_v4f32(<vscale x 4 x double> %a, <vscale x 4 x float> %b) #0 {
; CHECK-LABEL: test_copysign_v4f64_v4f32:
; CHECK:       // %bb.0:
; CHECK-NEXT:    uunpklo z3.d, z2.s
; CHECK-NEXT:    uunpkhi z2.d, z2.s
; CHECK-NEXT:    ptrue p0.d
; CHECK-NEXT:    and z0.d, z0.d, #0x7fffffffffffffff
; CHECK-NEXT:    and z1.d, z1.d, #0x7fffffffffffffff
; CHECK-NEXT:    fcvt z3.d, p0/m, z3.s
; CHECK-NEXT:    fcvt z2.d, p0/m, z2.s
; CHECK-NEXT:    and z3.d, z3.d, #0x8000000000000000
; CHECK-NEXT:    and z2.d, z2.d, #0x8000000000000000
; CHECK-NEXT:    orr z0.d, z0.d, z3.d
; CHECK-NEXT:    orr z1.d, z1.d, z2.d
; CHECK-NEXT:    ret
  %tmp0 = fpext <vscale x 4 x float> %b to <vscale x 4 x double>
  %r = call <vscale x 4 x double> @llvm.copysign.v4f64(<vscale x 4 x double> %a, <vscale x 4 x double> %tmp0)
  ret <vscale x 4 x double> %r
}

; SplitVecRes same
define <vscale x 4 x double> @test_copysign_v4f64_v4f64(<vscale x 4 x double> %a, <vscale x 4 x double> %b) #0 {
; CHECK-LABEL: test_copysign_v4f64_v4f64:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and z2.d, z2.d, #0x8000000000000000
; CHECK-NEXT:    and z0.d, z0.d, #0x7fffffffffffffff
; CHECK-NEXT:    and z3.d, z3.d, #0x8000000000000000
; CHECK-NEXT:    and z1.d, z1.d, #0x7fffffffffffffff
; CHECK-NEXT:    orr z0.d, z0.d, z2.d
; CHECK-NEXT:    orr z1.d, z1.d, z3.d
; CHECK-NEXT:    ret
  %r = call <vscale x 4 x double> @llvm.copysign.v4f64(<vscale x 4 x double> %a, <vscale x 4 x double> %b)
  ret <vscale x 4 x double> %r
}

declare <vscale x 4 x double> @llvm.copysign.v4f64(<vscale x 4 x double> %a, <vscale x 4 x double> %b) #0

;============ v4f16

define <vscale x 4 x half> @test_copysign_v4f16_v4f16(<vscale x 4 x half> %a, <vscale x 4 x half> %b) #0 {
; CHECK-LABEL: test_copysign_v4f16_v4f16:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and z1.h, z1.h, #0x8000
; CHECK-NEXT:    and z0.h, z0.h, #0x7fff
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %r = call <vscale x 4 x half> @llvm.copysign.v4f16(<vscale x 4 x half> %a, <vscale x 4 x half> %b)
  ret <vscale x 4 x half> %r
}

define <vscale x 4 x half> @test_copysign_v4f16_v4f32(<vscale x 4 x half> %a, <vscale x 4 x float> %b) #0 {
; CHECK-LABEL: test_copysign_v4f16_v4f32:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ptrue p0.s
; CHECK-NEXT:    and z0.h, z0.h, #0x7fff
; CHECK-NEXT:    fcvt z1.h, p0/m, z1.s
; CHECK-NEXT:    and z1.h, z1.h, #0x8000
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %tmp0 = fptrunc <vscale x 4 x float> %b to <vscale x 4 x half>
  %r = call <vscale x 4 x half> @llvm.copysign.v4f16(<vscale x 4 x half> %a, <vscale x 4 x half> %tmp0)
  ret <vscale x 4 x half> %r
}

define <vscale x 4 x half> @test_copysign_v4f16_v4f64(<vscale x 4 x half> %a, <vscale x 4 x double> %b) #0 {
; CHECK-LABEL: test_copysign_v4f16_v4f64:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ptrue p0.d
; CHECK-NEXT:    and z0.h, z0.h, #0x7fff
; CHECK-NEXT:    fcvt z2.h, p0/m, z2.d
; CHECK-NEXT:    fcvt z1.h, p0/m, z1.d
; CHECK-NEXT:    uzp1 z1.s, z1.s, z2.s
; CHECK-NEXT:    and z1.h, z1.h, #0x8000
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %tmp0 = fptrunc <vscale x 4 x double> %b to <vscale x 4 x half>
  %r = call <vscale x 4 x half> @llvm.copysign.v4f16(<vscale x 4 x half> %a, <vscale x 4 x half> %tmp0)
  ret <vscale x 4 x half> %r
}

declare <vscale x 4 x half> @llvm.copysign.v4f16(<vscale x 4 x half> %a, <vscale x 4 x half> %b) #0

;============ v8f16

define <vscale x 8 x half> @test_copysign_v8f16_v8f16(<vscale x 8 x half> %a, <vscale x 8 x half> %b) #0 {
; CHECK-LABEL: test_copysign_v8f16_v8f16:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and z1.h, z1.h, #0x8000
; CHECK-NEXT:    and z0.h, z0.h, #0x7fff
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %r = call <vscale x 8 x half> @llvm.copysign.v8f16(<vscale x 8 x half> %a, <vscale x 8 x half> %b)
  ret <vscale x 8 x half> %r
}

define <vscale x 8 x half> @test_copysign_v8f16_v8f32(<vscale x 8 x half> %a, <vscale x 8 x float> %b) #0 {
; CHECK-LABEL: test_copysign_v8f16_v8f32:
; CHECK:       // %bb.0:
; CHECK-NEXT:    ptrue p0.s
; CHECK-NEXT:    and z0.h, z0.h, #0x7fff
; CHECK-NEXT:    fcvt z2.h, p0/m, z2.s
; CHECK-NEXT:    fcvt z1.h, p0/m, z1.s
; CHECK-NEXT:    uzp1 z1.h, z1.h, z2.h
; CHECK-NEXT:    and z1.h, z1.h, #0x8000
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    ret
  %tmp0 = fptrunc <vscale x 8 x float> %b to <vscale x 8 x half>
  %r = call <vscale x 8 x half> @llvm.copysign.v8f16(<vscale x 8 x half> %a, <vscale x 8 x half> %tmp0)
  ret <vscale x 8 x half> %r
}


;========== FCOPYSIGN_EXTEND_ROUND

define <vscale x 4 x half> @test_copysign_nxv4f32_nxv4f16(<vscale x 4 x float> %a, <vscale x 4 x float> %b) #0 {
; CHECK-LABEL: test_copysign_nxv4f32_nxv4f16:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and z1.s, z1.s, #0x80000000
; CHECK-NEXT:    and z0.s, z0.s, #0x7fffffff
; CHECK-NEXT:    ptrue p0.s
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    fcvt z0.h, p0/m, z0.s
; CHECK-NEXT:    ret
  %t1 = call <vscale x 4 x float> @llvm.copysign.v4f32(<vscale x 4 x float> %a, <vscale x 4 x float> %b)
  %t2 = fptrunc <vscale x 4 x float> %t1 to <vscale x 4 x half>
  ret <vscale x 4 x half> %t2
}

define <vscale x 2 x float> @test_copysign_nxv2f64_nxv2f32(<vscale x 2 x double> %a, <vscale x 2 x double> %b) #0 {
; CHECK-LABEL: test_copysign_nxv2f64_nxv2f32:
; CHECK:       // %bb.0:
; CHECK-NEXT:    and z1.d, z1.d, #0x8000000000000000
; CHECK-NEXT:    and z0.d, z0.d, #0x7fffffffffffffff
; CHECK-NEXT:    ptrue p0.d
; CHECK-NEXT:    orr z0.d, z0.d, z1.d
; CHECK-NEXT:    fcvt z0.s, p0/m, z0.d
; CHECK-NEXT:    ret
  %t1 = call <vscale x 2 x double> @llvm.copysign.v2f64(<vscale x 2 x double> %a, <vscale x 2 x double> %b)
  %t2 = fptrunc <vscale x 2 x double> %t1 to <vscale x 2 x float>
  ret <vscale x 2 x float> %t2
}

declare <vscale x 8 x half> @llvm.copysign.v8f16(<vscale x 8 x half> %a, <vscale x 8 x half> %b) #0

attributes #0 = { nounwind }
