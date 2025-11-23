# REQUIRES: x86

# RUN: llvm-mc -filetype=obj -triple x86_64 --elf-compact-unwind %s -o %t.o
# RUN: ld.lld --eh-frame-hdr --no-rosegment %t.o -o %t
# RUN: llvm-readelf --unwind %t | FileCheck %s

# CHECK:      EHFrameHeader {
# CHECK:        Header {
# CHECK-NEXT:     version: 2
# CHECK-NEXT:     eh_frame_ptr_enc: 0x1b
# CHECK-NEXT:     fde_count_enc: 0x3
# CHECK-NEXT:     table_enc: 0x3c
# CHECK-NEXT:     eh_frame_ptr: 0x200148
# CHECK-NEXT:     fde_count: 2
# CHECK-NEXT:     entry 0 {
# CHECK-NEXT:       initial_location: 0x200172
# CHECK-NEXT:       descriptor: 0x100000000000001
# CHECK-NEXT:     }
# CHECK-NEXT:     entry 1 {
# CHECK-NEXT:       initial_location: 0x200183
# CHECK-NEXT:       descriptor: 0x100000000000001
# CHECK-NEXT:     }
# CHECK-NEXT: }

# CHECK:      .eh_frame section
# CHECK:        [0x200148] CIE length=20
# CHECK-NEXT:     version: 1
# CHECK-NEXT:     augmentation: zRC
# CHECK:        [0x200160] CIE length=0

	.text
	.globl	foo                             # -- Begin function foo
	.p2align	4
	.type	foo,@function
foo:                                    # @foo
	.cfi_startproc
# %bb.0:                                # %entry
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	popq	%rbp
	retq
.Lfunc_end0:
	.size	foo, .Lfunc_end0-foo
	.cfi_endproc
                                        # -- End function
	.globl	bar                             # -- Begin function bar
	.p2align	4
	.type	bar,@function
bar:                                    # @bar
	.cfi_startproc
# %bb.0:                                # %entry
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	popq	%rbp
	retq
.Lfunc_end1:
	.size	bar, .Lfunc_end1-bar
	.cfi_endproc
