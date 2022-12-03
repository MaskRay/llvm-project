// REQUIRES: arm
// RUN: llvm-mc -arm-add-build-attributes -filetype=obj --triple=thumbv8m.base %S/Inputs/arm-cmse-implib-1.s -o %t1.o
/// TODO: Test seg faults when using multiple threads. Workaround by running on a single thread.
// RUN: ld.lld -Ttext=0x8000 --section-start .gnu.sgstubs=0x20000 -o %t1 --out-implib=%t1.lib --cmse-implib %t1.o --threads=1
// RUN: llvm-readelf -s %t1 %t1.lib | FileCheck %s --check-prefix=CHECK1
// RUN: llvm-objdump -d %t1 | FileCheck %s --check-prefix=DISS1

// RUN: llvm-mc -arm-add-build-attributes -filetype=obj --triple=thumbv8m.base %S/Inputs/arm-cmse-implib-2.s -o %t2.o
/// TODO: Test seg faults when using multiple threads. Workaround by running on a single thread.
// RUN: ld.lld -Ttext=0x8000 --section-start .gnu.sgstubs=0x20000 -o %t2 --out-implib=%t2.lib --in-implib=%t1.lib --cmse-implib %t2.o --threads=1
// RUN: llvm-readelf -s %t2 %t2.lib | FileCheck %s --check-prefix=CHECK2
// RUN: llvm-objdump -d %t2 | FileCheck %s --check-prefix=DISS2

	.syntax unified
	.text

	.align	2
	.global	foo
	.global	__acle_se_foo
	.thumb
	.thumb_func
	.type	foo, %function
	.type	__acle_se_foo, %function
// foo == __acle_se_foo. So veneer generation expected.
foo:
__acle_se_foo:
	nop
	.size	foo, .-foo
	.size	__acle_se_foo, .-__acle_se_foo

	.align	2
	.global	bar
	.global	__acle_se_bar
	.thumb
	.thumb_func
	.type	bar, %function
	.type	__acle_se_bar, %function
// Same as foo.
bar:
__acle_se_bar:
	nop
	.size	bar, .-bar
	.size	__acle_se_bar, .-__acle_se_bar

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

/// Executable 1
// CHECK1:      File:
// CHECK1:      Symbol table '.symtab' contains 12 entries:
// CHECK1-NEXT:    Num:    Value  Size Type    Bind   Vis       Ndx Name
// CHECK1-NEXT:      0: 00000000     0 NOTYPE  LOCAL  DEFAULT   UND
// CHECK1-NEXT:      1: 00020000     0 NOTYPE  LOCAL  DEFAULT     2 $t
// CHECK1-NEXT:      2: 00008000     0 NOTYPE  LOCAL  DEFAULT     1 $t.0
// CHECK1-NEXT:      3: 00020001     8 FUNC    GLOBAL DEFAULT     2 foo
// CHECK1-NEXT:      4: 00008001     2 FUNC    GLOBAL DEFAULT     1 __acle_se_foo
// CHECK1-NEXT:      5: 00020009     8 FUNC    GLOBAL DEFAULT     2 bar
// CHECK1-NEXT:      6: 00008005     2 FUNC    GLOBAL DEFAULT     1 __acle_se_bar
// CHECK1-NEXT:      7: 00008009     6 FUNC    GLOBAL DEFAULT     1 no_veneer1
// CHECK1-NEXT:      8: 0000800d     2 FUNC    GLOBAL DEFAULT     1 __acle_se_no_veneer1
// CHECK1-NEXT:      9: 00008011     6 FUNC    GLOBAL DEFAULT     1 no_veneer2
// CHECK1-NEXT:     10: 00008015     2 FUNC    GLOBAL DEFAULT     1 __acle_se_no_veneer2
// CHECK1-NEXT:     11: 00008019     2 FUNC    GLOBAL DEFAULT     1 normal_sym

/// Import library 1
// CHECK1:      File:
// CHECK1:      Symbol table '.symtab' contains 5 entries:
// CHECK1-NEXT:    Num:    Value  Size Type    Bind   Vis       Ndx Name
// CHECK1-NEXT:      0: 00000000     0 NOTYPE  LOCAL  DEFAULT   UND
// CHECK1-NEXT:      1: 00020001     8 FUNC    GLOBAL DEFAULT   ABS foo
// CHECK1-NEXT:      2: 00020009     8 FUNC    GLOBAL DEFAULT   ABS bar
// CHECK1-NEXT:      3: 00008009     6 FUNC    GLOBAL DEFAULT   ABS no_veneer1
// CHECK1-NEXT:      4: 00008011     6 FUNC    GLOBAL DEFAULT   ABS no_veneer2

// DISS1:      Disassembly of section .text:
// DISS1:      00008000 <__acle_se_foo>:
// DISS1-NEXT:     8000: bf00         	nop
// DISS1-NEXT:     8002: 46c0         	mov	r8, r8
// DISS1:      00008004 <__acle_se_bar>:
// DISS1-NEXT:     8004: bf00         	nop
// DISS1-NEXT:     8006: 46c0         	mov	r8, r8
// DISS1:      00008008 <no_veneer1>:
// DISS1-NEXT:     8008: e97f e97f    	sg
// DISS1:      0000800c <__acle_se_no_veneer1>:
// DISS1-NEXT:     800c: bf00         	nop
// DISS1-NEXT:     800e: 46c0         	mov	r8, r8
// DISS1:      00008010 <no_veneer2>:
// DISS1-NEXT:     8010: e97f e97f    	sg
// DISS1:      00008014 <__acle_se_no_veneer2>:
// DISS1-NEXT:     8014: bf00         	nop
// DISS1-NEXT:     8016: 46c0         	mov	r8, r8
// DISS1:      00008018 <normal_sym>:
// DISS1-NEXT:     8018: bf00         	nop

// DISS1:      Disassembly of section .gnu.sgstubs:
// DISS1:      00020000 <foo>:
// DISS1-NEXT:    20000: e97f e97f    	sg
// DISS1-NEXT:    20004: f7e7 bffc    	b.w	0x8000 <__acle_se_foo>
// DISS1:      00020008 <bar>:
// DISS1-NEXT:    20008: e97f e97f    	sg
// DISS1-NEXT:    2000c: f7e7 bffa    	b.w	0x8004 <__acle_se_bar>

/// Executable 2
// CHECK2:      File:
// CHECK2:      Symbol table '.symtab' contains 14 entries:
// CHECK2-NEXT:    Num:    Value  Size Type    Bind   Vis       Ndx Name
// CHECK2-NEXT:      0: 00000000     0 NOTYPE  LOCAL  DEFAULT   UND
// CHECK2-NEXT:      1: 00020000     0 NOTYPE  LOCAL  DEFAULT     2 $t
// CHECK2-NEXT:      2: 00008000     0 NOTYPE  LOCAL  DEFAULT     1 $t.0
// CHECK2-NEXT:      3: 00020011     8 FUNC    GLOBAL DEFAULT     2 baz
// CHECK2-NEXT:      4: 00008001     2 FUNC    GLOBAL DEFAULT     1 __acle_se_baz
// CHECK2-NEXT:      5: 00020001     8 FUNC    GLOBAL DEFAULT     2 foo
// CHECK2-NEXT:      6: 00008005     2 FUNC    GLOBAL DEFAULT     1 __acle_se_foo
// CHECK2-NEXT:      7: 00020019     8 FUNC    GLOBAL DEFAULT     2 qux
// CHECK2-NEXT:      8: 00008009     2 FUNC    GLOBAL DEFAULT     1 __acle_se_qux
// CHECK2-NEXT:      9: 0000800d     6 FUNC    GLOBAL DEFAULT     1 no_veneer1
// CHECK2-NEXT:     10: 00008011     2 FUNC    GLOBAL DEFAULT     1 __acle_se_no_veneer1
// CHECK2-NEXT:     11: 00008015     6 FUNC    GLOBAL DEFAULT     1 no_veneer2
// CHECK2-NEXT:     12: 00008019     2 FUNC    GLOBAL DEFAULT     1 __acle_se_no_veneer2
// CHECK2-NEXT:     13: 0000801d     2 FUNC    GLOBAL DEFAULT     1 normal_sym

/// Note that foo retains its address from Import library 1 (0x00020001)
/// New entry functions, baz and qux, use addresses not used by Import librar 1.
/// Import library 2
// CHECK2:      File:
// CHECK2:      Symbol table '.symtab' contains 6 entries:
// CHECK2-NEXT:    Num:    Value  Size Type    Bind   Vis       Ndx Name
// CHECK2-NEXT:      0: 00000000     0 NOTYPE  LOCAL  DEFAULT   UND
// CHECK2-NEXT:      1: 00020011     8 FUNC    GLOBAL DEFAULT   ABS baz
// CHECK2-NEXT:      2: 00020001     8 FUNC    GLOBAL DEFAULT   ABS foo
// CHECK2-NEXT:      3: 00020019     8 FUNC    GLOBAL DEFAULT   ABS qux
// CHECK2-NEXT:      4: 0000800d     6 FUNC    GLOBAL DEFAULT   ABS no_veneer1
// CHECK2-NEXT:      5: 00008015     6 FUNC    GLOBAL DEFAULT   ABS no_veneer2

// DISS2: Disassembly of section .text:
// DISS2:      00008000 <__acle_se_baz>:
// DISS2-NEXT:     8000: bf00         	nop
// DISS2-NEXT:     8002: 46c0         	mov	r8, r8
// DISS2:      00008004 <__acle_se_foo>:
// DISS2-NEXT:     8004: bf00         	nop
// DISS2-NEXT:     8006: 46c0         	mov	r8, r8
// DISS2:      00008008 <__acle_se_qux>:
// DISS2-NEXT:     8008: bf00         	nop
// DISS2-NEXT:     800a: 46c0         	mov	r8, r8
// DISS2:      0000800c <no_veneer1>:
// DISS2-NEXT:     800c: e97f e97f    	sg
// DISS2:      00008010 <__acle_se_no_veneer1>:
// DISS2-NEXT:     8010: bf00         	nop
// DISS2-NEXT:     8012: 46c0         	mov	r8, r8
// DISS2:      00008014 <no_veneer2>:
// DISS2-NEXT:     8014: e97f e97f    	sg
// DISS2:      00008018 <__acle_se_no_veneer2>:
// DISS2-NEXT:     8018: bf00         	nop
// DISS2-NEXT:     801a: 46c0         	mov	r8, r8
// DISS2:      0000801c <normal_sym>:
// DISS2-NEXT:     801c: bf00         	nop

// DISS2: Disassembly of section .gnu.sgstubs:
// DISS2:      00020000 <foo>:
// DISS2-NEXT:    20000: e97f e97f    	sg
// DISS2-NEXT:    20004: f7e7 bffe    	b.w	0x8004 <__acle_se_foo>
// DISS2:      00020010 <baz>:
// DISS2-NEXT:    20010: e97f e97f    	sg
// DISS2-NEXT:    20014: f7e7 bff4    	b.w	0x8000 <__acle_se_baz>
// DISS2:      00020018 <qux>:
// DISS2-NEXT:    20018: e97f e97f    	sg
// DISS2-NEXT:    2001c: f7e7 bff4    	b.w	0x8008 <__acle_se_qux>
