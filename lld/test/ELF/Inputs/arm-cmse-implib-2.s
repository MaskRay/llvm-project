	.syntax unified
	.text

	.align	2
	.global	baz
	.global	__acle_se_baz
	.thumb
	.thumb_func
	.type	baz, %function
	.type	__acle_se_baz, %function
// baz == __acle_se_baz. So veneer generation expected.
baz:
__acle_se_baz:
	nop
	.size	baz, .-baz
	.size	__acle_se_baz, .-__acle_se_baz

	.align	2
	.global	foo
	.global	__acle_se_foo
	.thumb
	.thumb_func
	.type	foo, %function
	.type	__acle_se_foo, %function
// Same as baz.
foo:
__acle_se_foo:
	nop
	.size	foo, .-foo
	.size	__acle_se_foo, .-__acle_se_foo

	.align	2
	.global	qux
	.global	__acle_se_qux
	.thumb
	.thumb_func
	.type	qux, %function
	.type	__acle_se_qux, %function
// Same as baz.
qux:
__acle_se_qux:
	nop
	.size	qux, .-qux
	.size	__acle_se_qux, .-__acle_se_qux

	@ Valid setup for entry function without veneer generation
	.align	2
	.global	no_veneer1
	.global	__acle_se_no_veneer1
	.thumb
	.thumb_func
	.type	no_veneer1, %function
	.type	__acle_se_no_veneer1, %function
// no_veneer1 != __acle_se_no_veneer1.
// So no veneer generation is needed.
// However no_veneer1 will be imported to the import library.
no_veneer1:
	sg
__acle_se_no_veneer1:
	nop
	.size	no_veneer1, .-no_veneer1
	.size	__acle_se_no_veneer1, .-__acle_se_no_veneer1

	.align	2
	.global	no_veneer2
	.global	__acle_se_no_veneer2
	.thumb
	.thumb_func
	.type	no_veneer2, %function
	.type	__acle_se_no_veneer2, %function
// Same as no_veneer1.
no_veneer2:
	sg
__acle_se_no_veneer2:
	nop
	.size	no_veneer2, .-no_veneer2
	.size	__acle_se_no_veneer2, .-__acle_se_no_veneer2

	.align	2
	.global	normal_sym
	.type	normal_sym, %function
// Not an entry function because there is no corresponding __acle_se_normal_sym.
normal_sym:
	nop
	.size	normal_sym, .-normal_sym
