//===- ArgValue.h - Parse an Arg's value into a field ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Conversions from an Arg's string value to the field types an OPTION_VALUE
// struct uses.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OPTION_ARGVALUE_H
#define LLVM_OPTION_ARGVALUE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include <optional>
#include <string>
#include <type_traits>

namespace llvm::opt {

inline bool parseArgValue(const Arg &A, bool &V) {
  std::optional<bool> B = StringSwitch<std::optional<bool>>(A.getValue())
                              .Cases({"true", "TRUE", "True", "1"}, true)
                              .Cases({"false", "FALSE", "False", "0"}, false)
                              .Default(std::nullopt);
  if (B)
    V = *B;
  return B.has_value();
}

inline bool parseArgValue(const Arg &A, std::string &V) {
  V = A.getValue();
  return true;
}

template <class T>
std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, bool>
parseArgValue(const Arg &A, T &V) {
  return !StringRef(A.getValue()).getAsInteger(0, V);
}

template <class T> bool parseArgValue(const Arg &A, std::optional<T> &V) {
  T X;
  if (!parseArgValue(A, X))
    return false;
  V = X;
  return true;
}

} // namespace llvm::opt

#endif // LLVM_OPTION_ARGVALUE_H
