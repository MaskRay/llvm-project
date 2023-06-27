# REQUIRES: x86, zlib

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 a.s -o a.o
# RUN: not ld.lld -T a.lds a.o --compress-sections 'foo=zlib' 2>&1 | FileCheck %s --check-prefix=ERR --implicit-check-not=error:

# ERR: error: uncompressed size of SHF_COMPRESSED section 'foo' is dependent on linker script commands

# RUN: ld.lld -T b.lds a.o --compress-sections 'foo=zlib' -o a
# RUN: llvm-readelf -Ss a | FileCheck %s

# CHECK: .text    PROGBITS [[#%x,]]        [[#%x,]] [[#%x,]] 00 AX 0 0  4
# CHECK: foo      PROGBITS [[#%x,FOO:]]    [[#%x,]] [[#%x,]] 00 AC 0 0  1
# CHECK: bar      PROGBITS [[#%x,BAR:]]    [[#%x,]] [[#%x,]] 00 A  0 0  1

# CHECK: [[#FOO]]      0 NOTYPE  LOCAL  DEFAULT   [[#]] foo0_sym
# CHECK: [[#FOO+8]]    0 NOTYPE  LOCAL  DEFAULT   [[#]] foo1_sym
# CHECK: [[#FOO]]      0 NOTYPE  GLOBAL PROTECTED [[#]] __start_foo
# CHECK: [[#BAR]]      0 NOTYPE  GLOBAL PROTECTED [[#]] __stop_foo

#--- a.s
.globl _start
_start:
  leaq __start_foo(%rip), %rax
  leaq __stop_foo(%rip), %rax
  ret

.section foo0,"a"
foo0_sym:
.quad 42
.section foo1,"a"
foo1_sym:
.quad 42
.section bar,"a"
.quad 42

#--- a.lds
SECTIONS {
  foo : { *(foo*) . += a; }
  .text : { *(.text) }
  a = b+1;
  b = c+1;
  c = SIZEOF(.text);
}

#--- b.lds
SECTIONS {
  .text : { *(.text) }
  c = SIZEOF(.text);
  b = c+1;
  a = b+1;
  foo : { *(foo*) QUAD(SIZEOF(foo)) . += a; }
}
