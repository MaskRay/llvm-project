// REQUIRES: arm
// RUN: llvm-mc -arm-add-build-attributes -filetype=obj --triple=thumbv7m-none-eabi %s -o %t1.o
// RUN: not ld.lld --cmse-implib %t1.o -o /dev/null 2>&1 | FileCheck %s --check-prefix CHECK-MISSING

  .text
  .thumb

  # Missing standard symbol.
  .global __acle_se_missing_standard_sym
  .thumb_func
__acle_se_missing_standard_sym:

  # External symbol absolute.
  .global __acle_se_absolute_sym
__acle_se_absolute_sym=0x1001
  .global absolute_sym
absolute_sym=0x1001

// CHECK-MISSING: CMSE symbol __acle_se_missing_standard_sym detected in {{.*\.o}} but no associated global symbol definition missing_standard_sym found.
// CHECK-MISSING: CMSE symbol absolute_sym in {{.*\.o}} is absolute and cannot be changed to point to a new secure gateway veneer.
