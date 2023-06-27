# REQUIRES: x86, zlib, zstd

# RUN: llvm-mc -filetype=obj -triple=x86_64 %s -o %t.o
# RUN: ld.lld -pie %t.o -o %t --compress-sections '*0=zlib' --compress-sections '*0=none'
# RUN: llvm-readelf -Srs %t | FileCheck %s --check-prefix=CHECK1

# CHECK1:      foo0       PROGBITS [[#%x,FOO0:]]    [[#%x,]] [[#%x,]] 00 A   0 0  1
# CHECK1-NEXT: foo1       PROGBITS [[#%x,FOO1:]]    [[#%x,]] [[#%x,]] 00 A   0 0  1
# CHECK1-NEXT: .text      PROGBITS [[#%x,TEXT:]]    [[#%x,]] [[#%x,]] 00 AX  0 0  4
# CHECK1:      write0     PROGBITS [[#%x,WRITE0:]]  [[#%x,]] [[#%x,]] 00 WA  0 0  1
# CHECK1-NEXT: nonalloc0  PROGBITS 0000000000000000 [[#%x,]] [[#%x,]] 00     0 0  1
# CHECK1-NEXT: nonalloc1  PROGBITS 0000000000000000 [[#%x,]] [[#%x,]] 00     0 0  1
# CHECK1-NEXT: .debug_str PROGBITS 0000000000000000 [[#%x,]] [[#%x,]] 01 MS  0 0  1

# CHECK1:          Offset          {{.*}} Type              Symbol's Value  Symbol's Name + Addend
# CHECK1-NEXT: {{0*}}[[#WRITE0]]   {{.*}} R_X86_64_RELATIVE                 [[#TEXT]]
# CHECK1-NEXT: {{0*}}[[#WRITE0+8]] {{.*}} R_X86_64_RELATIVE                 [[#TEXT]]

# CHECK1: [[#FOO0]]      0 NOTYPE  LOCAL  DEFAULT   [[#]] foo0_sym
# CHECK1: [[#FOO1]]      0 NOTYPE  LOCAL  DEFAULT   [[#]] foo1_sym
# CHECK1: [[#FOO0]]      0 NOTYPE  GLOBAL PROTECTED [[#]] __start_foo0
# CHECK1: [[#FOO1]]      0 NOTYPE  GLOBAL PROTECTED [[#]] __stop_foo0

# RUN: ld.lld -pie %t.o -o %t --compress-sections '*0=zlib' --compress-sections .debug_str=zstd
# RUN: llvm-readelf -Srs -x foo0 -x write0 -x nonalloc0 -x .debug_str %t | FileCheck %s --check-prefix=CHECK2

# CHECK2:      foo0       PROGBITS [[#%x,FOO0:]]    [[#%x,]] [[#%x,]] 00 AC  0 0  1
# CHECK2-NEXT: foo1       PROGBITS [[#%x,FOO1:]]    [[#%x,]] [[#%x,]] 00 A   0 0  1
# CHECK2-NEXT: .text      PROGBITS [[#%x,TEXT:]]    [[#%x,]] [[#%x,]] 00 AX  0 0  4
# CHECK2:      write0     PROGBITS [[#%x,WRITE0:]]  [[#%x,]] [[#%x,]] 00 WAC 0 0  1
# CHECK2-NEXT: nonalloc0  PROGBITS 0000000000000000 [[#%x,]] [[#%x,]] 00 C   0 0  1
# CHECK2-NEXT: nonalloc1  PROGBITS 0000000000000000 [[#%x,]] [[#%x,]] 00     0 0  1
# CHECK2-NEXT: .debug_str PROGBITS 0000000000000000 [[#%x,]] [[#%x,]] 01 MSC 0 0  1

# CHECK2:          Offset          {{.*}} Type              Symbol's Value  Symbol's Name + Addend
# CHECK2-NEXT: {{0*}}[[#WRITE0]]   {{.*}} R_X86_64_RELATIVE                 [[#TEXT]]
# CHECK2-NEXT: {{0*}}[[#WRITE0+8]] {{.*}} R_X86_64_RELATIVE                 [[#TEXT]]

# CHECK2:      Hex dump of section 'foo0':
## zlib with ch_size=0x10
# CHECK2-NEXT: 01000000 00000000 10000000 00000000
# CHECK2-NEXT: 01000000 00000000 {{.*}}
# CHECK2:      Hex dump of section 'write0':
## zlib with ch_size=0x10
# CHECK2-NEXT: 01000000 00000000 10000000 00000000
# CHECK2-NEXT: 01000000 00000000 {{.*}}
# CHECK2:      Hex dump of section 'nonalloc0':
## zlib with ch_size=0x10
# CHECK2-NEXT: 01000000 00000000 10000000 00000000
# CHECK2-NEXT: 01000000 00000000 {{.*}}
# CHECK2:      Hex dump of section '.debug_str':
## zstd with ch_size=0x38
# CHECK2-NEXT: 02000000 00000000 38000000 00000000
# CHECK2-NEXT: 01000000 00000000 {{.*}}

# RUN: not ld.lld --compress-sections=foo %t.o -o /dev/null 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ERR1 --implicit-check-not=error:
# ERR1:      error: --compress-sections: parse error, not 'section-glob=[zlib|zstd]'

# RUN: not ld.lld --compress-sections 'a[=zlib' %t.o -o /dev/null 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ERR2 --implicit-check-not=error:
# ERR2:      error: --compress-sections: invalid glob pattern: a[

# RUN: not ld.lld %t.o -o /dev/null --compress-sections='.debug*=zlib-gabi' --compress-sections='.debug*=' 2>&1 | \
# RUN:   FileCheck -check-prefix=ERR3 %s
# ERR3:      unknown --compress-sections value: zlib-gabi
# ERR3-NEXT: --compress-sections: parse error, not 'section-glob=[zlib|zstd]'

.globl _start
_start:
  leaq __start_foo0(%rip), %rax
  leaq __stop_foo0(%rip), %rax
  ret

.section foo0,"a"
foo0_sym:
.quad .text-.
.quad .text-.
.section foo1,"a"
foo1_sym:
.quad .text-.
.quad .text-.
.section write0,"aw"
.quad .text
.quad .text
.section nonalloc0,""
.quad .text
.quad .text
.section nonalloc1,""
.quad 42

.section .debug_str,"MS",@progbits,1
.Linfo_string0:
  .asciz "AAAAAAAAAAAAAAAAAAAAAAAAAAA"
.Linfo_string1:
  .asciz "BBBBBBBBBBBBBBBBBBBBBBBBBBB"
