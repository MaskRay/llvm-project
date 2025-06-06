; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+sse4.2,+gfni | FileCheck %s --check-prefixes=GFNISSE
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+avx,+gfni | FileCheck %s --check-prefixes=GFNIAVX,GFNIAVX1OR2,GFNIAVX1
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+avx2,+gfni | FileCheck %s --check-prefixes=GFNIAVX,GFNIAVX1OR2,GFNIAVX2
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+avx512vl,+gfni | FileCheck %s --check-prefixes=GFNIAVX,GFNIAVX512,GFNIAVX512VL
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+avx512bw,+avx512vl,+gfni | FileCheck %s --check-prefixes=GFNIAVX,GFNIAVX512,GFNIAVX512BW

define <16 x i8> @testv16i8(<16 x i8> %in) nounwind {
; GFNISSE-LABEL: testv16i8:
; GFNISSE:       # %bb.0:
; GFNISSE-NEXT:    gf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %xmm0
; GFNISSE-NEXT:    pxor %xmm1, %xmm1
; GFNISSE-NEXT:    psubb %xmm0, %xmm1
; GFNISSE-NEXT:    pand %xmm1, %xmm0
; GFNISSE-NEXT:    gf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %xmm0
; GFNISSE-NEXT:    retq
;
; GFNIAVX1OR2-LABEL: testv16i8:
; GFNIAVX1OR2:       # %bb.0:
; GFNIAVX1OR2-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %xmm0, %xmm0
; GFNIAVX1OR2-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX1OR2-NEXT:    vpsubb %xmm0, %xmm1, %xmm1
; GFNIAVX1OR2-NEXT:    vpand %xmm1, %xmm0, %xmm0
; GFNIAVX1OR2-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %xmm0, %xmm0
; GFNIAVX1OR2-NEXT:    retq
;
; GFNIAVX512-LABEL: testv16i8:
; GFNIAVX512:       # %bb.0:
; GFNIAVX512-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to2}, %xmm0, %xmm0
; GFNIAVX512-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX512-NEXT:    vpsubb %xmm0, %xmm1, %xmm1
; GFNIAVX512-NEXT:    vpand %xmm1, %xmm0, %xmm0
; GFNIAVX512-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to2}, %xmm0, %xmm0
; GFNIAVX512-NEXT:    retq
  %out = call <16 x i8> @llvm.ctlz.v16i8(<16 x i8> %in, i1 0)
  ret <16 x i8> %out
}

define <16 x i8> @testv16i8u(<16 x i8> %in) nounwind {
; GFNISSE-LABEL: testv16i8u:
; GFNISSE:       # %bb.0:
; GFNISSE-NEXT:    gf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %xmm0
; GFNISSE-NEXT:    pxor %xmm1, %xmm1
; GFNISSE-NEXT:    psubb %xmm0, %xmm1
; GFNISSE-NEXT:    pand %xmm1, %xmm0
; GFNISSE-NEXT:    gf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %xmm0
; GFNISSE-NEXT:    retq
;
; GFNIAVX1OR2-LABEL: testv16i8u:
; GFNIAVX1OR2:       # %bb.0:
; GFNIAVX1OR2-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %xmm0, %xmm0
; GFNIAVX1OR2-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX1OR2-NEXT:    vpsubb %xmm0, %xmm1, %xmm1
; GFNIAVX1OR2-NEXT:    vpand %xmm1, %xmm0, %xmm0
; GFNIAVX1OR2-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %xmm0, %xmm0
; GFNIAVX1OR2-NEXT:    retq
;
; GFNIAVX512-LABEL: testv16i8u:
; GFNIAVX512:       # %bb.0:
; GFNIAVX512-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to2}, %xmm0, %xmm0
; GFNIAVX512-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX512-NEXT:    vpsubb %xmm0, %xmm1, %xmm1
; GFNIAVX512-NEXT:    vpand %xmm1, %xmm0, %xmm0
; GFNIAVX512-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to2}, %xmm0, %xmm0
; GFNIAVX512-NEXT:    retq
  %out = call <16 x i8> @llvm.ctlz.v16i8(<16 x i8> %in, i1 -1)
  ret <16 x i8> %out
}

define <32 x i8> @testv32i8(<32 x i8> %in) nounwind {
; GFNISSE-LABEL: testv32i8:
; GFNISSE:       # %bb.0:
; GFNISSE-NEXT:    movdqa {{.*#+}} xmm2 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm2, %xmm0
; GFNISSE-NEXT:    pxor %xmm3, %xmm3
; GFNISSE-NEXT:    pxor %xmm4, %xmm4
; GFNISSE-NEXT:    psubb %xmm0, %xmm4
; GFNISSE-NEXT:    pand %xmm4, %xmm0
; GFNISSE-NEXT:    movdqa {{.*#+}} xmm4 = [12307476859704049664,12307476859704049664]
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm4, %xmm0
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm2, %xmm1
; GFNISSE-NEXT:    psubb %xmm1, %xmm3
; GFNISSE-NEXT:    pand %xmm3, %xmm1
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm4, %xmm1
; GFNISSE-NEXT:    retq
;
; GFNIAVX1-LABEL: testv32i8:
; GFNIAVX1:       # %bb.0:
; GFNIAVX1-NEXT:    vextractf128 $1, %ymm0, %xmm1
; GFNIAVX1-NEXT:    vmovddup {{.*#+}} xmm2 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNIAVX1-NEXT:    # xmm2 = mem[0,0]
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm2, %xmm1, %xmm1
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm2, %xmm0, %xmm0
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm1, %ymm0, %ymm2
; GFNIAVX1-NEXT:    vpxor %xmm3, %xmm3, %xmm3
; GFNIAVX1-NEXT:    vpsubb %xmm1, %xmm3, %xmm1
; GFNIAVX1-NEXT:    vpsubb %xmm0, %xmm3, %xmm0
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm1, %ymm0, %ymm0
; GFNIAVX1-NEXT:    vandps %ymm0, %ymm2, %ymm0
; GFNIAVX1-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %ymm0, %ymm0
; GFNIAVX1-NEXT:    retq
;
; GFNIAVX2-LABEL: testv32i8:
; GFNIAVX2:       # %bb.0:
; GFNIAVX2-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %ymm0, %ymm0
; GFNIAVX2-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX2-NEXT:    vpsubb %ymm0, %ymm1, %ymm1
; GFNIAVX2-NEXT:    vpand %ymm1, %ymm0, %ymm0
; GFNIAVX2-NEXT:    vpbroadcastq {{.*#+}} ymm1 = [12307476859704049664,12307476859704049664,12307476859704049664,12307476859704049664]
; GFNIAVX2-NEXT:    vgf2p8affineqb $8, %ymm1, %ymm0, %ymm0
; GFNIAVX2-NEXT:    retq
;
; GFNIAVX512-LABEL: testv32i8:
; GFNIAVX512:       # %bb.0:
; GFNIAVX512-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to4}, %ymm0, %ymm0
; GFNIAVX512-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX512-NEXT:    vpsubb %ymm0, %ymm1, %ymm1
; GFNIAVX512-NEXT:    vpand %ymm1, %ymm0, %ymm0
; GFNIAVX512-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to4}, %ymm0, %ymm0
; GFNIAVX512-NEXT:    retq
  %out = call <32 x i8> @llvm.ctlz.v32i8(<32 x i8> %in, i1 0)
  ret <32 x i8> %out
}

define <32 x i8> @testv32i8u(<32 x i8> %in) nounwind {
; GFNISSE-LABEL: testv32i8u:
; GFNISSE:       # %bb.0:
; GFNISSE-NEXT:    movdqa {{.*#+}} xmm2 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm2, %xmm0
; GFNISSE-NEXT:    pxor %xmm3, %xmm3
; GFNISSE-NEXT:    pxor %xmm4, %xmm4
; GFNISSE-NEXT:    psubb %xmm0, %xmm4
; GFNISSE-NEXT:    pand %xmm4, %xmm0
; GFNISSE-NEXT:    movdqa {{.*#+}} xmm4 = [12307476859704049664,12307476859704049664]
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm4, %xmm0
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm2, %xmm1
; GFNISSE-NEXT:    psubb %xmm1, %xmm3
; GFNISSE-NEXT:    pand %xmm3, %xmm1
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm4, %xmm1
; GFNISSE-NEXT:    retq
;
; GFNIAVX1-LABEL: testv32i8u:
; GFNIAVX1:       # %bb.0:
; GFNIAVX1-NEXT:    vextractf128 $1, %ymm0, %xmm1
; GFNIAVX1-NEXT:    vmovddup {{.*#+}} xmm2 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNIAVX1-NEXT:    # xmm2 = mem[0,0]
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm2, %xmm1, %xmm1
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm2, %xmm0, %xmm0
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm1, %ymm0, %ymm2
; GFNIAVX1-NEXT:    vpxor %xmm3, %xmm3, %xmm3
; GFNIAVX1-NEXT:    vpsubb %xmm1, %xmm3, %xmm1
; GFNIAVX1-NEXT:    vpsubb %xmm0, %xmm3, %xmm0
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm1, %ymm0, %ymm0
; GFNIAVX1-NEXT:    vandps %ymm0, %ymm2, %ymm0
; GFNIAVX1-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %ymm0, %ymm0
; GFNIAVX1-NEXT:    retq
;
; GFNIAVX2-LABEL: testv32i8u:
; GFNIAVX2:       # %bb.0:
; GFNIAVX2-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip), %ymm0, %ymm0
; GFNIAVX2-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX2-NEXT:    vpsubb %ymm0, %ymm1, %ymm1
; GFNIAVX2-NEXT:    vpand %ymm1, %ymm0, %ymm0
; GFNIAVX2-NEXT:    vpbroadcastq {{.*#+}} ymm1 = [12307476859704049664,12307476859704049664,12307476859704049664,12307476859704049664]
; GFNIAVX2-NEXT:    vgf2p8affineqb $8, %ymm1, %ymm0, %ymm0
; GFNIAVX2-NEXT:    retq
;
; GFNIAVX512-LABEL: testv32i8u:
; GFNIAVX512:       # %bb.0:
; GFNIAVX512-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to4}, %ymm0, %ymm0
; GFNIAVX512-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX512-NEXT:    vpsubb %ymm0, %ymm1, %ymm1
; GFNIAVX512-NEXT:    vpand %ymm1, %ymm0, %ymm0
; GFNIAVX512-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to4}, %ymm0, %ymm0
; GFNIAVX512-NEXT:    retq
  %out = call <32 x i8> @llvm.ctlz.v32i8(<32 x i8> %in, i1 -1)
  ret <32 x i8> %out
}

define <64 x i8> @testv64i8(<64 x i8> %in) nounwind {
; GFNISSE-LABEL: testv64i8:
; GFNISSE:       # %bb.0:
; GFNISSE-NEXT:    movdqa {{.*#+}} xmm4 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm4, %xmm0
; GFNISSE-NEXT:    pxor %xmm5, %xmm5
; GFNISSE-NEXT:    pxor %xmm6, %xmm6
; GFNISSE-NEXT:    psubb %xmm0, %xmm6
; GFNISSE-NEXT:    pand %xmm6, %xmm0
; GFNISSE-NEXT:    movdqa {{.*#+}} xmm6 = [12307476859704049664,12307476859704049664]
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm6, %xmm0
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm4, %xmm1
; GFNISSE-NEXT:    pxor %xmm7, %xmm7
; GFNISSE-NEXT:    psubb %xmm1, %xmm7
; GFNISSE-NEXT:    pand %xmm7, %xmm1
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm6, %xmm1
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm4, %xmm2
; GFNISSE-NEXT:    pxor %xmm7, %xmm7
; GFNISSE-NEXT:    psubb %xmm2, %xmm7
; GFNISSE-NEXT:    pand %xmm7, %xmm2
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm6, %xmm2
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm4, %xmm3
; GFNISSE-NEXT:    psubb %xmm3, %xmm5
; GFNISSE-NEXT:    pand %xmm5, %xmm3
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm6, %xmm3
; GFNISSE-NEXT:    retq
;
; GFNIAVX1-LABEL: testv64i8:
; GFNIAVX1:       # %bb.0:
; GFNIAVX1-NEXT:    vextractf128 $1, %ymm0, %xmm2
; GFNIAVX1-NEXT:    vmovddup {{.*#+}} xmm3 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNIAVX1-NEXT:    # xmm3 = mem[0,0]
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm3, %xmm2, %xmm2
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm3, %xmm0, %xmm0
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm2, %ymm0, %ymm4
; GFNIAVX1-NEXT:    vpxor %xmm5, %xmm5, %xmm5
; GFNIAVX1-NEXT:    vpsubb %xmm2, %xmm5, %xmm2
; GFNIAVX1-NEXT:    vpsubb %xmm0, %xmm5, %xmm0
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm2, %ymm0, %ymm0
; GFNIAVX1-NEXT:    vandps %ymm0, %ymm4, %ymm0
; GFNIAVX1-NEXT:    vbroadcastsd {{.*#+}} ymm2 = [12307476859704049664,12307476859704049664,12307476859704049664,12307476859704049664]
; GFNIAVX1-NEXT:    vgf2p8affineqb $8, %ymm2, %ymm0, %ymm0
; GFNIAVX1-NEXT:    vextractf128 $1, %ymm1, %xmm4
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm3, %xmm4, %xmm4
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm3, %xmm1, %xmm1
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm4, %ymm1, %ymm3
; GFNIAVX1-NEXT:    vpsubb %xmm4, %xmm5, %xmm4
; GFNIAVX1-NEXT:    vpsubb %xmm1, %xmm5, %xmm1
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm4, %ymm1, %ymm1
; GFNIAVX1-NEXT:    vandps %ymm1, %ymm3, %ymm1
; GFNIAVX1-NEXT:    vgf2p8affineqb $8, %ymm2, %ymm1, %ymm1
; GFNIAVX1-NEXT:    retq
;
; GFNIAVX2-LABEL: testv64i8:
; GFNIAVX2:       # %bb.0:
; GFNIAVX2-NEXT:    vpbroadcastq {{.*#+}} ymm2 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNIAVX2-NEXT:    vgf2p8affineqb $0, %ymm2, %ymm0, %ymm0
; GFNIAVX2-NEXT:    vpxor %xmm3, %xmm3, %xmm3
; GFNIAVX2-NEXT:    vpsubb %ymm0, %ymm3, %ymm4
; GFNIAVX2-NEXT:    vpand %ymm4, %ymm0, %ymm0
; GFNIAVX2-NEXT:    vpbroadcastq {{.*#+}} ymm4 = [12307476859704049664,12307476859704049664,12307476859704049664,12307476859704049664]
; GFNIAVX2-NEXT:    vgf2p8affineqb $8, %ymm4, %ymm0, %ymm0
; GFNIAVX2-NEXT:    vgf2p8affineqb $0, %ymm2, %ymm1, %ymm1
; GFNIAVX2-NEXT:    vpsubb %ymm1, %ymm3, %ymm2
; GFNIAVX2-NEXT:    vpand %ymm2, %ymm1, %ymm1
; GFNIAVX2-NEXT:    vgf2p8affineqb $8, %ymm4, %ymm1, %ymm1
; GFNIAVX2-NEXT:    retq
;
; GFNIAVX512VL-LABEL: testv64i8:
; GFNIAVX512VL:       # %bb.0:
; GFNIAVX512VL-NEXT:    vextracti64x4 $1, %zmm0, %ymm1
; GFNIAVX512VL-NEXT:    vpbroadcastq {{.*#+}} ymm2 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNIAVX512VL-NEXT:    vgf2p8affineqb $0, %ymm2, %ymm1, %ymm1
; GFNIAVX512VL-NEXT:    vgf2p8affineqb $0, %ymm2, %ymm0, %ymm0
; GFNIAVX512VL-NEXT:    vinserti64x4 $1, %ymm1, %zmm0, %zmm2
; GFNIAVX512VL-NEXT:    vpxor %xmm3, %xmm3, %xmm3
; GFNIAVX512VL-NEXT:    vpsubb %ymm1, %ymm3, %ymm1
; GFNIAVX512VL-NEXT:    vpsubb %ymm0, %ymm3, %ymm0
; GFNIAVX512VL-NEXT:    vinserti64x4 $1, %ymm1, %zmm0, %zmm0
; GFNIAVX512VL-NEXT:    vpandq %zmm0, %zmm2, %zmm0
; GFNIAVX512VL-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to8}, %zmm0, %zmm0
; GFNIAVX512VL-NEXT:    retq
;
; GFNIAVX512BW-LABEL: testv64i8:
; GFNIAVX512BW:       # %bb.0:
; GFNIAVX512BW-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to8}, %zmm0, %zmm0
; GFNIAVX512BW-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX512BW-NEXT:    vpsubb %zmm0, %zmm1, %zmm1
; GFNIAVX512BW-NEXT:    vpandq %zmm1, %zmm0, %zmm0
; GFNIAVX512BW-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to8}, %zmm0, %zmm0
; GFNIAVX512BW-NEXT:    retq
  %out = call <64 x i8> @llvm.ctlz.v64i8(<64 x i8> %in, i1 0)
  ret <64 x i8> %out
}

define <64 x i8> @testv64i8u(<64 x i8> %in) nounwind {
; GFNISSE-LABEL: testv64i8u:
; GFNISSE:       # %bb.0:
; GFNISSE-NEXT:    movdqa {{.*#+}} xmm4 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm4, %xmm0
; GFNISSE-NEXT:    pxor %xmm5, %xmm5
; GFNISSE-NEXT:    pxor %xmm6, %xmm6
; GFNISSE-NEXT:    psubb %xmm0, %xmm6
; GFNISSE-NEXT:    pand %xmm6, %xmm0
; GFNISSE-NEXT:    movdqa {{.*#+}} xmm6 = [12307476859704049664,12307476859704049664]
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm6, %xmm0
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm4, %xmm1
; GFNISSE-NEXT:    pxor %xmm7, %xmm7
; GFNISSE-NEXT:    psubb %xmm1, %xmm7
; GFNISSE-NEXT:    pand %xmm7, %xmm1
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm6, %xmm1
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm4, %xmm2
; GFNISSE-NEXT:    pxor %xmm7, %xmm7
; GFNISSE-NEXT:    psubb %xmm2, %xmm7
; GFNISSE-NEXT:    pand %xmm7, %xmm2
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm6, %xmm2
; GFNISSE-NEXT:    gf2p8affineqb $0, %xmm4, %xmm3
; GFNISSE-NEXT:    psubb %xmm3, %xmm5
; GFNISSE-NEXT:    pand %xmm5, %xmm3
; GFNISSE-NEXT:    gf2p8affineqb $8, %xmm6, %xmm3
; GFNISSE-NEXT:    retq
;
; GFNIAVX1-LABEL: testv64i8u:
; GFNIAVX1:       # %bb.0:
; GFNIAVX1-NEXT:    vextractf128 $1, %ymm0, %xmm2
; GFNIAVX1-NEXT:    vmovddup {{.*#+}} xmm3 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNIAVX1-NEXT:    # xmm3 = mem[0,0]
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm3, %xmm2, %xmm2
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm3, %xmm0, %xmm0
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm2, %ymm0, %ymm4
; GFNIAVX1-NEXT:    vpxor %xmm5, %xmm5, %xmm5
; GFNIAVX1-NEXT:    vpsubb %xmm2, %xmm5, %xmm2
; GFNIAVX1-NEXT:    vpsubb %xmm0, %xmm5, %xmm0
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm2, %ymm0, %ymm0
; GFNIAVX1-NEXT:    vandps %ymm0, %ymm4, %ymm0
; GFNIAVX1-NEXT:    vbroadcastsd {{.*#+}} ymm2 = [12307476859704049664,12307476859704049664,12307476859704049664,12307476859704049664]
; GFNIAVX1-NEXT:    vgf2p8affineqb $8, %ymm2, %ymm0, %ymm0
; GFNIAVX1-NEXT:    vextractf128 $1, %ymm1, %xmm4
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm3, %xmm4, %xmm4
; GFNIAVX1-NEXT:    vgf2p8affineqb $0, %xmm3, %xmm1, %xmm1
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm4, %ymm1, %ymm3
; GFNIAVX1-NEXT:    vpsubb %xmm4, %xmm5, %xmm4
; GFNIAVX1-NEXT:    vpsubb %xmm1, %xmm5, %xmm1
; GFNIAVX1-NEXT:    vinsertf128 $1, %xmm4, %ymm1, %ymm1
; GFNIAVX1-NEXT:    vandps %ymm1, %ymm3, %ymm1
; GFNIAVX1-NEXT:    vgf2p8affineqb $8, %ymm2, %ymm1, %ymm1
; GFNIAVX1-NEXT:    retq
;
; GFNIAVX2-LABEL: testv64i8u:
; GFNIAVX2:       # %bb.0:
; GFNIAVX2-NEXT:    vpbroadcastq {{.*#+}} ymm2 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNIAVX2-NEXT:    vgf2p8affineqb $0, %ymm2, %ymm0, %ymm0
; GFNIAVX2-NEXT:    vpxor %xmm3, %xmm3, %xmm3
; GFNIAVX2-NEXT:    vpsubb %ymm0, %ymm3, %ymm4
; GFNIAVX2-NEXT:    vpand %ymm4, %ymm0, %ymm0
; GFNIAVX2-NEXT:    vpbroadcastq {{.*#+}} ymm4 = [12307476859704049664,12307476859704049664,12307476859704049664,12307476859704049664]
; GFNIAVX2-NEXT:    vgf2p8affineqb $8, %ymm4, %ymm0, %ymm0
; GFNIAVX2-NEXT:    vgf2p8affineqb $0, %ymm2, %ymm1, %ymm1
; GFNIAVX2-NEXT:    vpsubb %ymm1, %ymm3, %ymm2
; GFNIAVX2-NEXT:    vpand %ymm2, %ymm1, %ymm1
; GFNIAVX2-NEXT:    vgf2p8affineqb $8, %ymm4, %ymm1, %ymm1
; GFNIAVX2-NEXT:    retq
;
; GFNIAVX512VL-LABEL: testv64i8u:
; GFNIAVX512VL:       # %bb.0:
; GFNIAVX512VL-NEXT:    vextracti64x4 $1, %zmm0, %ymm1
; GFNIAVX512VL-NEXT:    vpbroadcastq {{.*#+}} ymm2 = [1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128]
; GFNIAVX512VL-NEXT:    vgf2p8affineqb $0, %ymm2, %ymm1, %ymm1
; GFNIAVX512VL-NEXT:    vgf2p8affineqb $0, %ymm2, %ymm0, %ymm0
; GFNIAVX512VL-NEXT:    vinserti64x4 $1, %ymm1, %zmm0, %zmm2
; GFNIAVX512VL-NEXT:    vpxor %xmm3, %xmm3, %xmm3
; GFNIAVX512VL-NEXT:    vpsubb %ymm1, %ymm3, %ymm1
; GFNIAVX512VL-NEXT:    vpsubb %ymm0, %ymm3, %ymm0
; GFNIAVX512VL-NEXT:    vinserti64x4 $1, %ymm1, %zmm0, %zmm0
; GFNIAVX512VL-NEXT:    vpandq %zmm0, %zmm2, %zmm0
; GFNIAVX512VL-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to8}, %zmm0, %zmm0
; GFNIAVX512VL-NEXT:    retq
;
; GFNIAVX512BW-LABEL: testv64i8u:
; GFNIAVX512BW:       # %bb.0:
; GFNIAVX512BW-NEXT:    vgf2p8affineqb $0, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to8}, %zmm0, %zmm0
; GFNIAVX512BW-NEXT:    vpxor %xmm1, %xmm1, %xmm1
; GFNIAVX512BW-NEXT:    vpsubb %zmm0, %zmm1, %zmm1
; GFNIAVX512BW-NEXT:    vpandq %zmm1, %zmm0, %zmm0
; GFNIAVX512BW-NEXT:    vgf2p8affineqb $8, {{\.?LCPI[0-9]+_[0-9]+}}(%rip){1to8}, %zmm0, %zmm0
; GFNIAVX512BW-NEXT:    retq
  %out = call <64 x i8> @llvm.ctlz.v64i8(<64 x i8> %in, i1 -1)
  ret <64 x i8> %out
}
;; NOTE: These prefixes are unused and the list is autogenerated. Do not add tests below this line:
; GFNIAVX: {{.*}}
