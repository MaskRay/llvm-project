; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN:  llc -mtriple=amdgcn  < %s | FileCheck -enable-var-scope --check-prefixes=GCN %s

define amdgpu_kernel void @uniform_sext_in_reg_i8_to_i32(ptr addrspace(1) %out, i32 %a, i32 %b) #0 {
; GCN-LABEL: uniform_sext_in_reg_i8_to_i32:
; GCN:       ; %bb.0:
; GCN-NEXT:    s_load_dwordx4 s[0:3], s[4:5], 0x9
; GCN-NEXT:    s_mov_b32 s7, 0xf000
; GCN-NEXT:    s_waitcnt lgkmcnt(0)
; GCN-NEXT:    s_add_i32 s2, s2, s3
; GCN-NEXT:    s_sext_i32_i8 s2, s2
; GCN-NEXT:    s_mov_b32 s6, -1
; GCN-NEXT:    s_mov_b32 s4, s0
; GCN-NEXT:    s_mov_b32 s5, s1
; GCN-NEXT:    v_mov_b32_e32 v0, s2
; GCN-NEXT:    buffer_store_dword v0, off, s[4:7], 0
; GCN-NEXT:    s_endpgm
  %c = add i32 %a, %b ; add to prevent folding into extload
  %shl = shl i32 %c, 24
  %ashr = ashr i32 %shl, 24
  store i32 %ashr, ptr addrspace(1) %out, align 4
  ret void
}

define amdgpu_kernel void @divergent_sext_in_reg_i8_to_i32(ptr addrspace(1) %out, i32 %a, i32 %b) #0 {
; GCN-LABEL: divergent_sext_in_reg_i8_to_i32:
; GCN:       ; %bb.0:
; GCN-NEXT:    s_load_dwordx4 s[0:3], s[4:5], 0x9
; GCN-NEXT:    s_mov_b32 s7, 0xf000
; GCN-NEXT:    s_mov_b32 s6, -1
; GCN-NEXT:    s_waitcnt lgkmcnt(0)
; GCN-NEXT:    s_mov_b32 s4, s0
; GCN-NEXT:    s_mov_b32 s5, s1
; GCN-NEXT:    s_add_i32 s0, s2, s3
; GCN-NEXT:    v_add_i32_e32 v0, vcc, s0, v0
; GCN-NEXT:    v_bfe_i32 v0, v0, 0, 8
; GCN-NEXT:    buffer_store_dword v0, off, s[4:7], 0
; GCN-NEXT:    s_endpgm
  %tid = call i32 @llvm.amdgcn.workitem.id.x()
  %c = add i32 %a, %b ; add to prevent folding into extload
  %c.divergent = add i32 %c, %tid
  %shl = shl i32 %c.divergent, 24
  %ashr = ashr i32 %shl, 24
  store i32 %ashr, ptr addrspace(1) %out, align 4
  ret void
}

define amdgpu_kernel void @uniform_sext_in_reg_i16_to_i32(ptr addrspace(1) %out, i32 %a, i32 %b) #0 {
; GCN-LABEL: uniform_sext_in_reg_i16_to_i32:
; GCN:       ; %bb.0:
; GCN-NEXT:    s_load_dwordx4 s[0:3], s[4:5], 0x9
; GCN-NEXT:    s_mov_b32 s7, 0xf000
; GCN-NEXT:    s_waitcnt lgkmcnt(0)
; GCN-NEXT:    s_add_i32 s2, s2, s3
; GCN-NEXT:    s_sext_i32_i16 s2, s2
; GCN-NEXT:    s_mov_b32 s6, -1
; GCN-NEXT:    s_mov_b32 s4, s0
; GCN-NEXT:    s_mov_b32 s5, s1
; GCN-NEXT:    v_mov_b32_e32 v0, s2
; GCN-NEXT:    buffer_store_dword v0, off, s[4:7], 0
; GCN-NEXT:    s_endpgm
  %c = add i32 %a, %b ; add to prevent folding into extload
  %shl = shl i32 %c, 16
  %ashr = ashr i32 %shl, 16
  store i32 %ashr, ptr addrspace(1) %out, align 4
  ret void
}

define amdgpu_kernel void @divergent_sext_in_reg_i16_to_i32(ptr addrspace(1) %out, i32 %a, i32 %b) #0 {
; GCN-LABEL: divergent_sext_in_reg_i16_to_i32:
; GCN:       ; %bb.0:
; GCN-NEXT:    s_load_dwordx4 s[0:3], s[4:5], 0x9
; GCN-NEXT:    s_mov_b32 s7, 0xf000
; GCN-NEXT:    s_mov_b32 s6, -1
; GCN-NEXT:    s_waitcnt lgkmcnt(0)
; GCN-NEXT:    s_mov_b32 s4, s0
; GCN-NEXT:    s_mov_b32 s5, s1
; GCN-NEXT:    s_add_i32 s0, s2, s3
; GCN-NEXT:    v_add_i32_e32 v0, vcc, s0, v0
; GCN-NEXT:    v_bfe_i32 v0, v0, 0, 16
; GCN-NEXT:    buffer_store_dword v0, off, s[4:7], 0
; GCN-NEXT:    s_endpgm
  %tid = call i32 @llvm.amdgcn.workitem.id.x()
  %c = add i32 %a, %b ; add to prevent folding into extload
  %c.divergent = add i32 %c, %tid
  %shl = shl i32 %c.divergent, 16
  %ashr = ashr i32 %shl, 16
  store i32 %ashr, ptr addrspace(1) %out, align 4
  ret void
}

declare i32 @llvm.amdgcn.workitem.id.x() #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone speculatable }
