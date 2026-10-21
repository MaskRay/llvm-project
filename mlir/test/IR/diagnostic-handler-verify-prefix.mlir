// RUN: mlir-opt -allow-unregistered-dialect %s -verify-diagnostics

// A directive prefix in the middle of a word is not recognized.
// unexpected-error {{not a directive}}
"test.foo"() : () -> ()
