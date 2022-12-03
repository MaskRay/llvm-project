
// REQUIRES: arm
// RUN: llvm-mc -arm-add-build-attributes -filetype=obj --triple=thumbv7m-none-eabi %s -o %t.o
// RUN: ld.lld -Ttext=0x8000 --section-start .gnu.sgstubs=0x20000 %t.o -o %t
// RUN: llvm-objdump -d %t | FileCheck %s

  .align  2
  .global  __acle_se_global_foo
  .global  global_foo
  .thumb
  .thumb_func
  .type  __acle_se_global_foo, %function
  .thumb
  .thumb_func
  .type  global_foo, %function
__acle_se_global_foo:
global_foo:
   nop
  .size  global_foo, .-global_foo
  .size  __acle_se_global_foo, .-__acle_se_global_foo

  .align  2
  .weak  __acle_se_weak_bar
  .weak  weak_bar
  .thumb
  .thumb_func
  .type  __acle_se_weak_bar, %function
  .thumb
  .thumb_func
  .type  weak_bar, %function
__acle_se_weak_bar:
weak_bar:
  nop
  .size  weak_bar, .-weak_bar
  .size  __acle_se_weak_bar, .-__acle_se_weak_bar

  .align  2
  .weak  __acle_se_global_baz
  .global  global_baz
  .thumb
  .thumb_func
  .type  __acle_se_global_baz, %function
  .thumb
  .thumb_func
  .type  global_baz, %function
__acle_se_global_baz:
global_baz:
  nop
  .size  global_baz, .-global_baz
  .size  __acle_se_global_baz, .-__acle_se_global_baz

  .align  2
  .global  __acle_se_weak_qux
  .weak  weak_qux
  .thumb
  .thumb_func
  .type  __acle_se_weak_qux, %function
  .thumb
  .thumb_func
  .type  weak_qux, %function
__acle_se_weak_qux:
weak_qux:
  nop
  .size  weak_qux, .-weak_qux
  .size  __acle_se_weak_qux, .-__acle_se_weak_qux

	.align 2
	.global	__acle_se_absolute_foobar
	.global	absolute_foobar
	.type	__acle_se_absolute_foobar, %function
	.type	absolute_foobar, %function
__acle_se_absolute_foobar = 0x10000
absolute_foobar = 0x10004
	.size	absolute_foobar, 0
	.size	__acle_se_absolute_foobar, 0

// CHECK: Disassembly of section .text:
// CHECK: 00008000 <__acle_se_global_foo>
// CHECK: 00008004 <__acle_se_weak_bar>
// CHECK: 00008008 <__acle_se_global_baz>
// CHECK: 0000800c <__acle_se_weak_qux>

// CHECK: Disassembly of section .gnu.sgstubs:

// CHECK: 00020000 <global_foo>
// CHECK:    20000: e97f e97f
// CHECK:    20004: f7e7 bffc    	b.w	0x8000 <__acle_se_global_foo>

// CHECK: 00020008 <weak_bar>
// CHECK:    20008: e97f e97f
// CHECK:    2000c: f7e7 bffa    	b.w	0x8004 <__acle_se_weak_bar>

// CHECK: 00020010 <global_baz>
// CHECK:    20010: e97f e97f
// CHECK:    20014: f7e7 bff8    	b.w	0x8008 <__acle_se_global_baz>

// CHECK: 00020018 <weak_qux>
// CHECK:    20018: e97f e97f
// CHECK:    2001c: f7e7 bff6    	b.w	0x800c <__acle_se_weak_qux>
