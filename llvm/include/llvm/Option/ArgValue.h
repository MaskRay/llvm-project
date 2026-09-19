//===- ArgValue.h - Option value conversions --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Conversions from an option's string value to the field types an
// OPTION_VALUE struct uses.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OPTION_ARGVALUE_H
#define LLVM_OPTION_ARGVALUE_H

#include "llvm/ADT/STLForwardCompat.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include <initializer_list>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace llvm::opt {

inline bool parseArgValue(StringRef S, bool &V) { return to_bool(S, V); }

inline bool parseArgValue(StringRef S, std::string &V) {
  V = S.str();
  return true;
}

template <class T>
std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, bool>
parseArgValue(StringRef S, T &V) {
  return to_integer(S, V);
}

template <class T>
std::enable_if_t<std::is_floating_point_v<T>, bool> parseArgValue(StringRef S,
                                                                  T &V) {
  double D;
  if (S.getAsDouble(D))
    return false;
  V = D;
  return true;
}

template <class T> bool parseArgValue(StringRef S, std::optional<T> &V) {
  T X;
  if (!parseArgValue(S, X))
    return false;
  V = std::move(X);
  return true;
}

/// Looks \p S up in \p Values, the spellings of an enumeration's values.
template <class T>
bool parseEnumValue(
    StringRef S, T &V,
    std::initializer_list<std::pair<StringRef, type_identity_t<T>>> Values) {
  for (const auto &[Name, Value] : Values) {
    if (S == Name) {
      V = Value;
      return true;
    }
  }
  return false;
}

} // namespace llvm::opt

#endif // LLVM_OPTION_ARGVALUE_H
