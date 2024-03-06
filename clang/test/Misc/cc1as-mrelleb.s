// REQUIRES: x86-registered-target
// RUN: %clang -cc1as -triple x86_64 %s -filetype obj -o %t -mrelleb
// RUN: llvm-readelf -S %t | FileCheck %s

// CHECK: .relleb.text
call foo
