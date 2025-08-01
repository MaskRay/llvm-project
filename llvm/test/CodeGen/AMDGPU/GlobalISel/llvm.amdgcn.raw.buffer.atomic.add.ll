; NOTE: Assertions have been autogenerated by utils/update_mir_test_checks.py
; RUN: llc -global-isel -mtriple=amdgcn-mesa-mesa3d -mcpu=fiji -stop-after=instruction-select -o - %s | FileCheck -check-prefixes=GFX8 %s
; RUN: llc -global-isel -mtriple=amdgcn-mesa-mesa3d -mcpu=gfx1200 -stop-after=instruction-select -o - %s | FileCheck --check-prefixes=GFX12 %s

; Natural mapping
define amdgpu_ps float @raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset(i32 %val, <4 x i32> inreg %rsrc, i32 %voffset, i32 inreg %soffset) {
  ; GFX8-LABEL: name: raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset
  ; GFX8: bb.1 (%ir-block.0):
  ; GFX8-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX8-NEXT:   [[COPY1:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX8-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX8-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX8-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX8-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY5:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX8-NEXT:   [[COPY6:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX8-NEXT:   [[BUFFER_ATOMIC_ADD_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_OFFEN_RTN [[COPY]], [[COPY5]], [[REG_SEQUENCE]], [[COPY6]], 0, 1, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX8-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_OFFEN_RTN]]
  ; GFX8-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  ;
  ; GFX12-LABEL: name: raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset
  ; GFX12: bb.1 (%ir-block.0):
  ; GFX12-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX12-NEXT:   [[COPY1:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX12-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX12-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX12-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX12-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY5:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX12-NEXT:   [[COPY6:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX12-NEXT:   [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN [[COPY]], [[COPY5]], [[REG_SEQUENCE]], [[COPY6]], 0, 1, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX12-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN]]
  ; GFX12-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  %ret = call i32 @llvm.amdgcn.raw.buffer.atomic.add.i32(i32 %val, <4 x i32> %rsrc, i32 %voffset, i32 %soffset, i32 0)
  %cast = bitcast i32 %ret to float
  ret float %cast
}

define amdgpu_ps float @raw_buffer_atomic_add_i32_noret__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset(i32 %val, <4 x i32> inreg %rsrc, i32 %voffset, i32 inreg %soffset) {
  ; GFX8-LABEL: name: raw_buffer_atomic_add_i32_noret__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset
  ; GFX8: bb.1 (%ir-block.0):
  ; GFX8-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX8-NEXT:   [[COPY1:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX8-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX8-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX8-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX8-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY5:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX8-NEXT:   [[COPY6:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX8-NEXT:   [[BUFFER_ATOMIC_ADD_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_OFFEN_RTN [[COPY]], [[COPY5]], [[REG_SEQUENCE]], [[COPY6]], 0, 1, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX8-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_OFFEN_RTN]]
  ; GFX8-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  ;
  ; GFX12-LABEL: name: raw_buffer_atomic_add_i32_noret__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset
  ; GFX12: bb.1 (%ir-block.0):
  ; GFX12-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX12-NEXT:   [[COPY1:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX12-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX12-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX12-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX12-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY5:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX12-NEXT:   [[COPY6:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX12-NEXT:   [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN [[COPY]], [[COPY5]], [[REG_SEQUENCE]], [[COPY6]], 0, 1, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX12-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN]]
  ; GFX12-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  %ret = call i32 @llvm.amdgcn.raw.buffer.atomic.add.i32(i32 %val, <4 x i32> %rsrc, i32 %voffset, i32 %soffset, i32 0)
  %cast = bitcast i32 %ret to float
  ret float %cast
}

define amdgpu_ps <2 x float> @raw_buffer_atomic_add_i64__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset(i64 %val, <4 x i32> inreg %rsrc, i32 %voffset, i32 inreg %soffset) {
  ; GFX8-LABEL: name: raw_buffer_atomic_add_i64__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset
  ; GFX8: bb.1 (%ir-block.0):
  ; GFX8-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1, $vgpr2
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX8-NEXT:   [[COPY1:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX8-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:vreg_64 = REG_SEQUENCE [[COPY]], %subreg.sub0, [[COPY1]], %subreg.sub1
  ; GFX8-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX8-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX8-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX8-NEXT:   [[COPY5:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX8-NEXT:   [[REG_SEQUENCE1:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY2]], %subreg.sub0, [[COPY3]], %subreg.sub1, [[COPY4]], %subreg.sub2, [[COPY5]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY6:%[0-9]+]]:vgpr_32 = COPY $vgpr2
  ; GFX8-NEXT:   [[COPY7:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX8-NEXT:   [[BUFFER_ATOMIC_ADD_X2_OFFEN_RTN:%[0-9]+]]:vreg_64 = BUFFER_ATOMIC_ADD_X2_OFFEN_RTN [[REG_SEQUENCE]], [[COPY6]], [[REG_SEQUENCE1]], [[COPY7]], 0, 1, implicit $exec :: (volatile dereferenceable load store (s64), align 1, addrspace 8)
  ; GFX8-NEXT:   [[COPY8:%[0-9]+]]:vgpr_32 = COPY [[BUFFER_ATOMIC_ADD_X2_OFFEN_RTN]].sub0
  ; GFX8-NEXT:   [[COPY9:%[0-9]+]]:vgpr_32 = COPY [[BUFFER_ATOMIC_ADD_X2_OFFEN_RTN]].sub1
  ; GFX8-NEXT:   $vgpr0 = COPY [[COPY8]]
  ; GFX8-NEXT:   $vgpr1 = COPY [[COPY9]]
  ; GFX8-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0, implicit $vgpr1
  ;
  ; GFX12-LABEL: name: raw_buffer_atomic_add_i64__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset
  ; GFX12: bb.1 (%ir-block.0):
  ; GFX12-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1, $vgpr2
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX12-NEXT:   [[COPY1:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX12-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:vreg_64 = REG_SEQUENCE [[COPY]], %subreg.sub0, [[COPY1]], %subreg.sub1
  ; GFX12-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX12-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX12-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX12-NEXT:   [[COPY5:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX12-NEXT:   [[REG_SEQUENCE1:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY2]], %subreg.sub0, [[COPY3]], %subreg.sub1, [[COPY4]], %subreg.sub2, [[COPY5]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY6:%[0-9]+]]:vgpr_32 = COPY $vgpr2
  ; GFX12-NEXT:   [[COPY7:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX12-NEXT:   [[BUFFER_ATOMIC_ADD_X2_VBUFFER_OFFEN_RTN:%[0-9]+]]:vreg_64 = BUFFER_ATOMIC_ADD_X2_VBUFFER_OFFEN_RTN [[REG_SEQUENCE]], [[COPY6]], [[REG_SEQUENCE1]], [[COPY7]], 0, 1, implicit $exec :: (volatile dereferenceable load store (s64), align 1, addrspace 8)
  ; GFX12-NEXT:   [[COPY8:%[0-9]+]]:vgpr_32 = COPY [[BUFFER_ATOMIC_ADD_X2_VBUFFER_OFFEN_RTN]].sub0
  ; GFX12-NEXT:   [[COPY9:%[0-9]+]]:vgpr_32 = COPY [[BUFFER_ATOMIC_ADD_X2_VBUFFER_OFFEN_RTN]].sub1
  ; GFX12-NEXT:   $vgpr0 = COPY [[COPY8]]
  ; GFX12-NEXT:   $vgpr1 = COPY [[COPY9]]
  ; GFX12-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0, implicit $vgpr1
  %ret = call i64 @llvm.amdgcn.raw.buffer.atomic.add.i64(i64 %val, <4 x i32> %rsrc, i32 %voffset, i32 %soffset, i32 0)
  %cast = bitcast i64 %ret to <2 x float>
  ret <2 x float> %cast
}

define amdgpu_ps void @raw_buffer_atomic_add_i64_noret__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset(i64 %val, <4 x i32> inreg %rsrc, i32 %voffset, i32 inreg %soffset) {
  ; GFX8-LABEL: name: raw_buffer_atomic_add_i64_noret__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset
  ; GFX8: bb.1 (%ir-block.0):
  ; GFX8-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1, $vgpr2
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX8-NEXT:   [[COPY1:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX8-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:vreg_64 = REG_SEQUENCE [[COPY]], %subreg.sub0, [[COPY1]], %subreg.sub1
  ; GFX8-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX8-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX8-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX8-NEXT:   [[COPY5:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX8-NEXT:   [[REG_SEQUENCE1:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY2]], %subreg.sub0, [[COPY3]], %subreg.sub1, [[COPY4]], %subreg.sub2, [[COPY5]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY6:%[0-9]+]]:vgpr_32 = COPY $vgpr2
  ; GFX8-NEXT:   [[COPY7:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX8-NEXT:   BUFFER_ATOMIC_ADD_X2_OFFEN [[REG_SEQUENCE]], [[COPY6]], [[REG_SEQUENCE1]], [[COPY7]], 0, 0, implicit $exec :: (volatile dereferenceable load store (s64), align 1, addrspace 8)
  ; GFX8-NEXT:   S_ENDPGM 0
  ;
  ; GFX12-LABEL: name: raw_buffer_atomic_add_i64_noret__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset
  ; GFX12: bb.1 (%ir-block.0):
  ; GFX12-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1, $vgpr2
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX12-NEXT:   [[COPY1:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX12-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:vreg_64 = REG_SEQUENCE [[COPY]], %subreg.sub0, [[COPY1]], %subreg.sub1
  ; GFX12-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX12-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX12-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX12-NEXT:   [[COPY5:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX12-NEXT:   [[REG_SEQUENCE1:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY2]], %subreg.sub0, [[COPY3]], %subreg.sub1, [[COPY4]], %subreg.sub2, [[COPY5]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY6:%[0-9]+]]:vgpr_32 = COPY $vgpr2
  ; GFX12-NEXT:   [[COPY7:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX12-NEXT:   BUFFER_ATOMIC_ADD_X2_VBUFFER_OFFEN [[REG_SEQUENCE]], [[COPY6]], [[REG_SEQUENCE1]], [[COPY7]], 0, 0, implicit $exec :: (volatile dereferenceable load store (s64), align 1, addrspace 8)
  ; GFX12-NEXT:   S_ENDPGM 0
  %ret = call i64 @llvm.amdgcn.raw.buffer.atomic.add.i64(i64 %val, <4 x i32> %rsrc, i32 %voffset, i32 %soffset, i32 0)
  ret void
}

; All operands need regbank legalization
define amdgpu_ps float @raw_buffer_atomic_add_i32__sgpr_val__vgpr_rsrc__sgpr_voffset__vgpr_soffset(i32 inreg %val, <4 x i32> %rsrc, i32 inreg %voffset, i32 %soffset) {
  ; GFX8-LABEL: name: raw_buffer_atomic_add_i32__sgpr_val__vgpr_rsrc__sgpr_voffset__vgpr_soffset
  ; GFX8: bb.1 (%ir-block.0):
  ; GFX8-NEXT:   successors: %bb.2(0x80000000)
  ; GFX8-NEXT:   liveins: $sgpr2, $sgpr3, $vgpr0, $vgpr1, $vgpr2, $vgpr3, $vgpr4
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[COPY:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX8-NEXT:   [[COPY1:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX8-NEXT:   [[COPY2:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX8-NEXT:   [[COPY3:%[0-9]+]]:vgpr_32 = COPY $vgpr2
  ; GFX8-NEXT:   [[COPY4:%[0-9]+]]:vgpr_32 = COPY $vgpr3
  ; GFX8-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:vreg_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY5:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX8-NEXT:   [[COPY6:%[0-9]+]]:vgpr_32 = COPY $vgpr4
  ; GFX8-NEXT:   [[COPY7:%[0-9]+]]:vgpr_32 = COPY [[COPY]]
  ; GFX8-NEXT:   [[COPY8:%[0-9]+]]:vgpr_32 = COPY [[COPY5]]
  ; GFX8-NEXT:   [[S_MOV_B64_:%[0-9]+]]:sreg_64_xexec = S_MOV_B64 $exec
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT: bb.2:
  ; GFX8-NEXT:   successors: %bb.3(0x80000000)
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY1]], implicit $exec
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_1:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY2]], implicit $exec
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_2:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY3]], implicit $exec
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_3:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY4]], implicit $exec
  ; GFX8-NEXT:   [[REG_SEQUENCE1:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[V_READFIRSTLANE_B32_]], %subreg.sub0, [[V_READFIRSTLANE_B32_1]], %subreg.sub1, [[V_READFIRSTLANE_B32_2]], %subreg.sub2, [[V_READFIRSTLANE_B32_3]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY9:%[0-9]+]]:vreg_64 = COPY [[REG_SEQUENCE]].sub0_sub1
  ; GFX8-NEXT:   [[COPY10:%[0-9]+]]:vreg_64 = COPY [[REG_SEQUENCE]].sub2_sub3
  ; GFX8-NEXT:   [[COPY11:%[0-9]+]]:sreg_64 = COPY [[REG_SEQUENCE1]].sub0_sub1
  ; GFX8-NEXT:   [[COPY12:%[0-9]+]]:sreg_64 = COPY [[REG_SEQUENCE1]].sub2_sub3
  ; GFX8-NEXT:   [[V_CMP_EQ_U64_e64_:%[0-9]+]]:sreg_64_xexec = V_CMP_EQ_U64_e64 [[COPY11]], [[COPY9]], implicit $exec
  ; GFX8-NEXT:   [[V_CMP_EQ_U64_e64_1:%[0-9]+]]:sreg_64_xexec = V_CMP_EQ_U64_e64 [[COPY12]], [[COPY10]], implicit $exec
  ; GFX8-NEXT:   [[S_AND_B64_:%[0-9]+]]:sreg_64_xexec = S_AND_B64 [[V_CMP_EQ_U64_e64_]], [[V_CMP_EQ_U64_e64_1]], implicit-def dead $scc
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_4:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY6]], implicit $exec
  ; GFX8-NEXT:   [[V_CMP_EQ_U32_e64_:%[0-9]+]]:sreg_64_xexec = V_CMP_EQ_U32_e64 [[V_READFIRSTLANE_B32_4]], [[COPY6]], implicit $exec
  ; GFX8-NEXT:   [[S_AND_B64_1:%[0-9]+]]:sreg_64_xexec = S_AND_B64 [[S_AND_B64_]], [[V_CMP_EQ_U32_e64_]], implicit-def dead $scc
  ; GFX8-NEXT:   [[S_AND_SAVEEXEC_B64_:%[0-9]+]]:sreg_64_xexec = S_AND_SAVEEXEC_B64 killed [[S_AND_B64_1]], implicit-def $exec, implicit-def $scc, implicit $exec
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT: bb.3:
  ; GFX8-NEXT:   successors: %bb.4(0x40000000), %bb.2(0x40000000)
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[BUFFER_ATOMIC_ADD_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_OFFEN_RTN [[COPY7]], [[COPY8]], [[REG_SEQUENCE1]], [[V_READFIRSTLANE_B32_4]], 0, 1, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX8-NEXT:   $exec = S_XOR_B64_term $exec, [[S_AND_SAVEEXEC_B64_]], implicit-def $scc
  ; GFX8-NEXT:   SI_WATERFALL_LOOP %bb.2, implicit $exec
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT: bb.4:
  ; GFX8-NEXT:   successors: %bb.5(0x80000000)
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   $exec = S_MOV_B64_term [[S_MOV_B64_]]
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT: bb.5:
  ; GFX8-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_OFFEN_RTN]]
  ; GFX8-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  ;
  ; GFX12-LABEL: name: raw_buffer_atomic_add_i32__sgpr_val__vgpr_rsrc__sgpr_voffset__vgpr_soffset
  ; GFX12: bb.1 (%ir-block.0):
  ; GFX12-NEXT:   successors: %bb.2(0x80000000)
  ; GFX12-NEXT:   liveins: $sgpr2, $sgpr3, $vgpr0, $vgpr1, $vgpr2, $vgpr3, $vgpr4
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[COPY:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX12-NEXT:   [[COPY1:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX12-NEXT:   [[COPY2:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX12-NEXT:   [[COPY3:%[0-9]+]]:vgpr_32 = COPY $vgpr2
  ; GFX12-NEXT:   [[COPY4:%[0-9]+]]:vgpr_32 = COPY $vgpr3
  ; GFX12-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:vreg_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY5:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX12-NEXT:   [[COPY6:%[0-9]+]]:vgpr_32 = COPY $vgpr4
  ; GFX12-NEXT:   [[COPY7:%[0-9]+]]:vgpr_32 = COPY [[COPY]]
  ; GFX12-NEXT:   [[COPY8:%[0-9]+]]:vgpr_32 = COPY [[COPY5]]
  ; GFX12-NEXT:   [[S_MOV_B32_:%[0-9]+]]:sreg_32_xm0_xexec = S_MOV_B32 $exec_lo
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT: bb.2:
  ; GFX12-NEXT:   successors: %bb.3(0x80000000)
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY1]], implicit $exec
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_1:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY2]], implicit $exec
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_2:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY3]], implicit $exec
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_3:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY4]], implicit $exec
  ; GFX12-NEXT:   [[REG_SEQUENCE1:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[V_READFIRSTLANE_B32_]], %subreg.sub0, [[V_READFIRSTLANE_B32_1]], %subreg.sub1, [[V_READFIRSTLANE_B32_2]], %subreg.sub2, [[V_READFIRSTLANE_B32_3]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY9:%[0-9]+]]:vreg_64 = COPY [[REG_SEQUENCE]].sub0_sub1
  ; GFX12-NEXT:   [[COPY10:%[0-9]+]]:vreg_64 = COPY [[REG_SEQUENCE]].sub2_sub3
  ; GFX12-NEXT:   [[COPY11:%[0-9]+]]:sreg_64 = COPY [[REG_SEQUENCE1]].sub0_sub1
  ; GFX12-NEXT:   [[COPY12:%[0-9]+]]:sreg_64 = COPY [[REG_SEQUENCE1]].sub2_sub3
  ; GFX12-NEXT:   [[V_CMP_EQ_U64_e64_:%[0-9]+]]:sreg_32_xm0_xexec = V_CMP_EQ_U64_e64 [[COPY11]], [[COPY9]], implicit $exec
  ; GFX12-NEXT:   [[V_CMP_EQ_U64_e64_1:%[0-9]+]]:sreg_32_xm0_xexec = V_CMP_EQ_U64_e64 [[COPY12]], [[COPY10]], implicit $exec
  ; GFX12-NEXT:   [[S_AND_B32_:%[0-9]+]]:sreg_32_xm0_xexec = S_AND_B32 [[V_CMP_EQ_U64_e64_]], [[V_CMP_EQ_U64_e64_1]], implicit-def dead $scc
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_4:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY6]], implicit $exec
  ; GFX12-NEXT:   [[V_CMP_EQ_U32_e64_:%[0-9]+]]:sreg_32_xm0_xexec = V_CMP_EQ_U32_e64 [[V_READFIRSTLANE_B32_4]], [[COPY6]], implicit $exec
  ; GFX12-NEXT:   [[S_AND_B32_1:%[0-9]+]]:sreg_32_xm0_xexec = S_AND_B32 [[S_AND_B32_]], [[V_CMP_EQ_U32_e64_]], implicit-def dead $scc
  ; GFX12-NEXT:   [[S_AND_SAVEEXEC_B32_:%[0-9]+]]:sreg_32_xm0_xexec = S_AND_SAVEEXEC_B32 killed [[S_AND_B32_1]], implicit-def $exec, implicit-def $scc, implicit $exec
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT: bb.3:
  ; GFX12-NEXT:   successors: %bb.4(0x40000000), %bb.2(0x40000000)
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN [[COPY7]], [[COPY8]], [[REG_SEQUENCE1]], [[V_READFIRSTLANE_B32_4]], 0, 1, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX12-NEXT:   $exec_lo = S_XOR_B32_term $exec_lo, [[S_AND_SAVEEXEC_B32_]], implicit-def $scc
  ; GFX12-NEXT:   SI_WATERFALL_LOOP %bb.2, implicit $exec
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT: bb.4:
  ; GFX12-NEXT:   successors: %bb.5(0x80000000)
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   $exec_lo = S_MOV_B32_term [[S_MOV_B32_]]
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT: bb.5:
  ; GFX12-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN]]
  ; GFX12-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  %ret = call i32 @llvm.amdgcn.raw.buffer.atomic.add.i32(i32 %val, <4 x i32> %rsrc, i32 %voffset, i32 %soffset, i32 0)
  %cast = bitcast i32 %ret to float
  ret float %cast
}

; All operands need regbank legalization
define amdgpu_ps void @raw_buffer_atomic_add_i32_noret__sgpr_val__vgpr_rsrc__sgpr_voffset__vgpr_soffset(i32 inreg %val, <4 x i32> %rsrc, i32 inreg %voffset, i32 %soffset) {
  ; GFX8-LABEL: name: raw_buffer_atomic_add_i32_noret__sgpr_val__vgpr_rsrc__sgpr_voffset__vgpr_soffset
  ; GFX8: bb.1 (%ir-block.0):
  ; GFX8-NEXT:   successors: %bb.2(0x80000000)
  ; GFX8-NEXT:   liveins: $sgpr2, $sgpr3, $vgpr0, $vgpr1, $vgpr2, $vgpr3, $vgpr4
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[COPY:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX8-NEXT:   [[COPY1:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX8-NEXT:   [[COPY2:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX8-NEXT:   [[COPY3:%[0-9]+]]:vgpr_32 = COPY $vgpr2
  ; GFX8-NEXT:   [[COPY4:%[0-9]+]]:vgpr_32 = COPY $vgpr3
  ; GFX8-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:vreg_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY5:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX8-NEXT:   [[COPY6:%[0-9]+]]:vgpr_32 = COPY $vgpr4
  ; GFX8-NEXT:   [[COPY7:%[0-9]+]]:vgpr_32 = COPY [[COPY]]
  ; GFX8-NEXT:   [[COPY8:%[0-9]+]]:vgpr_32 = COPY [[COPY5]]
  ; GFX8-NEXT:   [[S_MOV_B64_:%[0-9]+]]:sreg_64_xexec = S_MOV_B64 $exec
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT: bb.2:
  ; GFX8-NEXT:   successors: %bb.3(0x80000000)
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY1]], implicit $exec
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_1:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY2]], implicit $exec
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_2:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY3]], implicit $exec
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_3:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY4]], implicit $exec
  ; GFX8-NEXT:   [[REG_SEQUENCE1:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[V_READFIRSTLANE_B32_]], %subreg.sub0, [[V_READFIRSTLANE_B32_1]], %subreg.sub1, [[V_READFIRSTLANE_B32_2]], %subreg.sub2, [[V_READFIRSTLANE_B32_3]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY9:%[0-9]+]]:vreg_64 = COPY [[REG_SEQUENCE]].sub0_sub1
  ; GFX8-NEXT:   [[COPY10:%[0-9]+]]:vreg_64 = COPY [[REG_SEQUENCE]].sub2_sub3
  ; GFX8-NEXT:   [[COPY11:%[0-9]+]]:sreg_64 = COPY [[REG_SEQUENCE1]].sub0_sub1
  ; GFX8-NEXT:   [[COPY12:%[0-9]+]]:sreg_64 = COPY [[REG_SEQUENCE1]].sub2_sub3
  ; GFX8-NEXT:   [[V_CMP_EQ_U64_e64_:%[0-9]+]]:sreg_64_xexec = V_CMP_EQ_U64_e64 [[COPY11]], [[COPY9]], implicit $exec
  ; GFX8-NEXT:   [[V_CMP_EQ_U64_e64_1:%[0-9]+]]:sreg_64_xexec = V_CMP_EQ_U64_e64 [[COPY12]], [[COPY10]], implicit $exec
  ; GFX8-NEXT:   [[S_AND_B64_:%[0-9]+]]:sreg_64_xexec = S_AND_B64 [[V_CMP_EQ_U64_e64_]], [[V_CMP_EQ_U64_e64_1]], implicit-def dead $scc
  ; GFX8-NEXT:   [[V_READFIRSTLANE_B32_4:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY6]], implicit $exec
  ; GFX8-NEXT:   [[V_CMP_EQ_U32_e64_:%[0-9]+]]:sreg_64_xexec = V_CMP_EQ_U32_e64 [[V_READFIRSTLANE_B32_4]], [[COPY6]], implicit $exec
  ; GFX8-NEXT:   [[S_AND_B64_1:%[0-9]+]]:sreg_64_xexec = S_AND_B64 [[S_AND_B64_]], [[V_CMP_EQ_U32_e64_]], implicit-def dead $scc
  ; GFX8-NEXT:   [[S_AND_SAVEEXEC_B64_:%[0-9]+]]:sreg_64_xexec = S_AND_SAVEEXEC_B64 killed [[S_AND_B64_1]], implicit-def $exec, implicit-def $scc, implicit $exec
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT: bb.3:
  ; GFX8-NEXT:   successors: %bb.4(0x40000000), %bb.2(0x40000000)
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   BUFFER_ATOMIC_ADD_OFFEN [[COPY7]], [[COPY8]], [[REG_SEQUENCE1]], [[V_READFIRSTLANE_B32_4]], 0, 0, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX8-NEXT:   $exec = S_XOR_B64_term $exec, [[S_AND_SAVEEXEC_B64_]], implicit-def $scc
  ; GFX8-NEXT:   SI_WATERFALL_LOOP %bb.2, implicit $exec
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT: bb.4:
  ; GFX8-NEXT:   successors: %bb.5(0x80000000)
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   $exec = S_MOV_B64_term [[S_MOV_B64_]]
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT: bb.5:
  ; GFX8-NEXT:   S_ENDPGM 0
  ;
  ; GFX12-LABEL: name: raw_buffer_atomic_add_i32_noret__sgpr_val__vgpr_rsrc__sgpr_voffset__vgpr_soffset
  ; GFX12: bb.1 (%ir-block.0):
  ; GFX12-NEXT:   successors: %bb.2(0x80000000)
  ; GFX12-NEXT:   liveins: $sgpr2, $sgpr3, $vgpr0, $vgpr1, $vgpr2, $vgpr3, $vgpr4
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[COPY:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX12-NEXT:   [[COPY1:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX12-NEXT:   [[COPY2:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX12-NEXT:   [[COPY3:%[0-9]+]]:vgpr_32 = COPY $vgpr2
  ; GFX12-NEXT:   [[COPY4:%[0-9]+]]:vgpr_32 = COPY $vgpr3
  ; GFX12-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:vreg_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY5:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX12-NEXT:   [[COPY6:%[0-9]+]]:vgpr_32 = COPY $vgpr4
  ; GFX12-NEXT:   [[COPY7:%[0-9]+]]:vgpr_32 = COPY [[COPY]]
  ; GFX12-NEXT:   [[COPY8:%[0-9]+]]:vgpr_32 = COPY [[COPY5]]
  ; GFX12-NEXT:   [[S_MOV_B32_:%[0-9]+]]:sreg_32_xm0_xexec = S_MOV_B32 $exec_lo
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT: bb.2:
  ; GFX12-NEXT:   successors: %bb.3(0x80000000)
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY1]], implicit $exec
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_1:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY2]], implicit $exec
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_2:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY3]], implicit $exec
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_3:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY4]], implicit $exec
  ; GFX12-NEXT:   [[REG_SEQUENCE1:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[V_READFIRSTLANE_B32_]], %subreg.sub0, [[V_READFIRSTLANE_B32_1]], %subreg.sub1, [[V_READFIRSTLANE_B32_2]], %subreg.sub2, [[V_READFIRSTLANE_B32_3]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY9:%[0-9]+]]:vreg_64 = COPY [[REG_SEQUENCE]].sub0_sub1
  ; GFX12-NEXT:   [[COPY10:%[0-9]+]]:vreg_64 = COPY [[REG_SEQUENCE]].sub2_sub3
  ; GFX12-NEXT:   [[COPY11:%[0-9]+]]:sreg_64 = COPY [[REG_SEQUENCE1]].sub0_sub1
  ; GFX12-NEXT:   [[COPY12:%[0-9]+]]:sreg_64 = COPY [[REG_SEQUENCE1]].sub2_sub3
  ; GFX12-NEXT:   [[V_CMP_EQ_U64_e64_:%[0-9]+]]:sreg_32_xm0_xexec = V_CMP_EQ_U64_e64 [[COPY11]], [[COPY9]], implicit $exec
  ; GFX12-NEXT:   [[V_CMP_EQ_U64_e64_1:%[0-9]+]]:sreg_32_xm0_xexec = V_CMP_EQ_U64_e64 [[COPY12]], [[COPY10]], implicit $exec
  ; GFX12-NEXT:   [[S_AND_B32_:%[0-9]+]]:sreg_32_xm0_xexec = S_AND_B32 [[V_CMP_EQ_U64_e64_]], [[V_CMP_EQ_U64_e64_1]], implicit-def dead $scc
  ; GFX12-NEXT:   [[V_READFIRSTLANE_B32_4:%[0-9]+]]:sreg_32_xm0 = V_READFIRSTLANE_B32 [[COPY6]], implicit $exec
  ; GFX12-NEXT:   [[V_CMP_EQ_U32_e64_:%[0-9]+]]:sreg_32_xm0_xexec = V_CMP_EQ_U32_e64 [[V_READFIRSTLANE_B32_4]], [[COPY6]], implicit $exec
  ; GFX12-NEXT:   [[S_AND_B32_1:%[0-9]+]]:sreg_32_xm0_xexec = S_AND_B32 [[S_AND_B32_]], [[V_CMP_EQ_U32_e64_]], implicit-def dead $scc
  ; GFX12-NEXT:   [[S_AND_SAVEEXEC_B32_:%[0-9]+]]:sreg_32_xm0_xexec = S_AND_SAVEEXEC_B32 killed [[S_AND_B32_1]], implicit-def $exec, implicit-def $scc, implicit $exec
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT: bb.3:
  ; GFX12-NEXT:   successors: %bb.4(0x40000000), %bb.2(0x40000000)
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   BUFFER_ATOMIC_ADD_VBUFFER_OFFEN [[COPY7]], [[COPY8]], [[REG_SEQUENCE1]], [[V_READFIRSTLANE_B32_4]], 0, 0, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX12-NEXT:   $exec_lo = S_XOR_B32_term $exec_lo, [[S_AND_SAVEEXEC_B32_]], implicit-def $scc
  ; GFX12-NEXT:   SI_WATERFALL_LOOP %bb.2, implicit $exec
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT: bb.4:
  ; GFX12-NEXT:   successors: %bb.5(0x80000000)
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   $exec_lo = S_MOV_B32_term [[S_MOV_B32_]]
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT: bb.5:
  ; GFX12-NEXT:   S_ENDPGM 0
  %ret = call i32 @llvm.amdgcn.raw.buffer.atomic.add.i32(i32 %val, <4 x i32> %rsrc, i32 %voffset, i32 %soffset, i32 0)
  ret void
}

define amdgpu_ps float @raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset_voffset_add4095(i32 %val, <4 x i32> inreg %rsrc, i32 %voffset.base, i32 inreg %soffset) {
  ; GFX8-LABEL: name: raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset_voffset_add4095
  ; GFX8: bb.1 (%ir-block.0):
  ; GFX8-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX8-NEXT:   [[COPY1:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX8-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX8-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX8-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX8-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY5:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX8-NEXT:   [[COPY6:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX8-NEXT:   [[BUFFER_ATOMIC_ADD_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_OFFEN_RTN [[COPY]], [[COPY5]], [[REG_SEQUENCE]], [[COPY6]], 4095, 1, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX8-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_OFFEN_RTN]]
  ; GFX8-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  ;
  ; GFX12-LABEL: name: raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset_voffset_add4095
  ; GFX12: bb.1 (%ir-block.0):
  ; GFX12-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX12-NEXT:   [[COPY1:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX12-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX12-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX12-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX12-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY5:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX12-NEXT:   [[COPY6:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX12-NEXT:   [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN [[COPY]], [[COPY5]], [[REG_SEQUENCE]], [[COPY6]], 4095, 1, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX12-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN]]
  ; GFX12-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  %voffset = add i32 %voffset.base, 4095
  %ret = call i32 @llvm.amdgcn.raw.buffer.atomic.add.i32(i32 %val, <4 x i32> %rsrc, i32 %voffset, i32 %soffset, i32 0)
  %cast = bitcast i32 %ret to float
  ret float %cast
}

; Natural mapping + slc
define amdgpu_ps float @raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset_slc(i32 %val, <4 x i32> inreg %rsrc, i32 %voffset, i32 inreg %soffset) {
  ; GFX8-LABEL: name: raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset_slc
  ; GFX8: bb.1 (%ir-block.0):
  ; GFX8-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1
  ; GFX8-NEXT: {{  $}}
  ; GFX8-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX8-NEXT:   [[COPY1:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX8-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX8-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX8-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX8-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX8-NEXT:   [[COPY5:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX8-NEXT:   [[COPY6:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX8-NEXT:   [[BUFFER_ATOMIC_ADD_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_OFFEN_RTN [[COPY]], [[COPY5]], [[REG_SEQUENCE]], [[COPY6]], 0, 3, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX8-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_OFFEN_RTN]]
  ; GFX8-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  ;
  ; GFX12-LABEL: name: raw_buffer_atomic_add_i32__vgpr_val__sgpr_rsrc__vgpr_voffset__sgpr_soffset_slc
  ; GFX12: bb.1 (%ir-block.0):
  ; GFX12-NEXT:   liveins: $sgpr2, $sgpr3, $sgpr4, $sgpr5, $sgpr6, $vgpr0, $vgpr1
  ; GFX12-NEXT: {{  $}}
  ; GFX12-NEXT:   [[COPY:%[0-9]+]]:vgpr_32 = COPY $vgpr0
  ; GFX12-NEXT:   [[COPY1:%[0-9]+]]:sreg_32 = COPY $sgpr2
  ; GFX12-NEXT:   [[COPY2:%[0-9]+]]:sreg_32 = COPY $sgpr3
  ; GFX12-NEXT:   [[COPY3:%[0-9]+]]:sreg_32 = COPY $sgpr4
  ; GFX12-NEXT:   [[COPY4:%[0-9]+]]:sreg_32 = COPY $sgpr5
  ; GFX12-NEXT:   [[REG_SEQUENCE:%[0-9]+]]:sgpr_128 = REG_SEQUENCE [[COPY1]], %subreg.sub0, [[COPY2]], %subreg.sub1, [[COPY3]], %subreg.sub2, [[COPY4]], %subreg.sub3
  ; GFX12-NEXT:   [[COPY5:%[0-9]+]]:vgpr_32 = COPY $vgpr1
  ; GFX12-NEXT:   [[COPY6:%[0-9]+]]:sreg_32 = COPY $sgpr6
  ; GFX12-NEXT:   [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN:%[0-9]+]]:vgpr_32 = BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN [[COPY]], [[COPY5]], [[REG_SEQUENCE]], [[COPY6]], 0, 3, implicit $exec :: (volatile dereferenceable load store (s32), align 1, addrspace 8)
  ; GFX12-NEXT:   $vgpr0 = COPY [[BUFFER_ATOMIC_ADD_VBUFFER_OFFEN_RTN]]
  ; GFX12-NEXT:   SI_RETURN_TO_EPILOG implicit $vgpr0
  %ret = call i32 @llvm.amdgcn.raw.buffer.atomic.add.i32(i32 %val, <4 x i32> %rsrc, i32 %voffset, i32 %soffset, i32 2)
  %cast = bitcast i32 %ret to float
  ret float %cast
}

declare i32 @llvm.amdgcn.raw.buffer.atomic.add.i32(i32, <4 x i32>, i32, i32, i32 immarg) #0
declare i64 @llvm.amdgcn.raw.buffer.atomic.add.i64(i64, <4 x i32>, i32, i32, i32 immarg) #0

attributes #0 = { nounwind }
