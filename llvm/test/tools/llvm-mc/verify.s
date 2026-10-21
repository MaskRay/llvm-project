# REQUIRES: x86-registered-target
## Test --verify, which checks diagnostics against '<prefix>-<kind> {{...}}' comments.
# RUN: rm -rf %t && split-file %s %t && cd %t

## A directive matches a diagnostic on its line (or at an @ offset), and optionally its column.
## A prefix in the middle of a word is not recognized.
## Directives in .include'd files are recognized. --fatal-warnings turns warnings into errors.
# RUN: llvm-mc -triple=x86_64 --verify=common,warn a.s -o /dev/null 2>&1 | count 0
# RUN: llvm-mc -triple=x86_64 --verify=common,fatal --fatal-warnings a.s -o /dev/null 2>&1 | count 0

## An error without a location, reported after parsing. The output is not kept.
# RUN: llvm-mc -triple=x86_64 --verify --filetype=obj obj.s -o obj.o 2>&1 | count 0
# RUN: not ls obj.o

# RUN: llvm-mc -triple=x86_64 --verify --disassemble dis.txt 2>&1 | count 0

## The output is not kept if verification fails.
# RUN: not llvm-mc -triple=x86_64 --verify --filetype=obj fail-inc.s -o fail.o 2>/dev/null
# RUN: not ls fail.o

# RUN: not llvm-mc -triple=x86_64 --verify fail.s -o /dev/null 2>&1 | FileCheck %s --match-full-lines --implicit-check-not=fail
# CHECK:      fail.s:6:3: error: invalid column '0'
# CHECK:      fail.s:1:1: error: unexpected error: unknown directive
# CHECK:      fail.s:2:8: error: 'error' diagnostic emitted when expecting a 'warning'
# CHECK:      fail.s:4:1: error: unexpected warning: w
# CHECK-DAG:  fail.s:2:8: error: expected warning "unknown directive" was not produced
# CHECK-DAG:  fail.s:3:7: error: expected error "never produced" was not produced
# CHECK-DAG:  fail.s:4:16: error: expected warning "w" was not produced
# CHECK-DAG:  Included from fail.s:5:
# CHECK-DAG:  fail-inc.s:1:7: error: expected error "in include" was not produced

## A failure that is not a diagnostic cannot be expected.
# RUN: not llvm-mc -triple=x86_64 --verify --defsym=bad empty.s 2>&1 | FileCheck %s --check-prefix=DEFSYM
# DEFSYM: error: defsym must be of the form: sym=value: bad

# RUN: not llvm-mc -triple=x86_64 --verify --as-lex empty.s 2>&1 | FileCheck %s --check-prefix=AS-LEX
# AS-LEX: error: --verify is not supported with --as-lex

#--- a.s
.foo # common-error:1 {{unknown directive}}
.bar
# common-error-re@-1 {{unknown {{d[a-z]+}}}}
nop # xcommon-error {{not a directive}}
movl $1, %eax, %ebx # common-error:16 {{invalid operand for instruction}}
.warning "w"
# warn-warning@-1 {{w}}
# fatal-error@-2:1 {{w}}
.include "a-inc.s"

#--- a-inc.s
.bar # common-error {{unknown directive}}

#--- obj.s
jmp .Ltmp
# expected-error@unknown {{Undefined temporary symbol .Ltmp}}

#--- dis.txt
0x0f 0xff # expected-warning {{invalid instruction encoding}}

#--- fail.s
.foo
.bar # expected-warning {{unknown directive}}
nop # expected-error {{never produced}}
.warning "w" # expected-warning:2 {{w}}
.include "fail-inc.s"
# expected-error:0 {{bad column}}

#--- fail-inc.s
nop # expected-error {{in include}}

#--- empty.s
nop
