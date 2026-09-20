//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Support for a library that keeps its command line options in a struct
// generated from its .td file: conversions from an option's string value to
// the field types a struct uses, and registration with OptionsRegistry and
// cl::. A library's .cpp defines OPTIONS_STRUCT_DEFS and includes its .inc
// after this header.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OPTION_LIBRARYOPTIONS_H
#define LLVM_OPTION_LIBRARYOPTIONS_H

#include "llvm/ADT/STLForwardCompat.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/OptionsRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include <initializer_list>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace llvm::opt {

inline bool parseArgValue(StringRef S, bool &V) {
  if (S == "true" || S == "1")
    V = true;
  else if (S == "false" || S == "0")
    V = false;
  else
    return false;
  return true;
}

inline bool parseArgValue(StringRef S, std::string &V) {
  V = S.str();
  return true;
}

template <class T>
std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, bool>
parseArgValue(StringRef S, T &V) {
  return to_integer(S, V);
}

inline bool parseArgValue(StringRef S, double &V) { return !S.getAsDouble(V); }

inline bool parseArgValue(StringRef S, float &V) {
  double D;
  if (S.getAsDouble(D))
    return false;
  V = D;
  return true;
}

template <class T> bool parseArgValue(StringRef S, std::optional<T> &V) {
  return parseArgValue(S, V.emplace());
}

template <class T> bool parseArgValue(StringRef S, std::vector<T> &V) {
  return parseArgValue(S, V.emplace_back());
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

/// Registers T, an OptionsStruct, with OptionsRegistry and cl::; a library's
/// .cpp defines a RegisterLibraryOptions<T>. The Joined spelling ValueField
/// and its siblings declare, "name=", is cl::'s "name".
template <class T> void registerLibraryOptions() {
  T::Slot = OptionsRegistry::allocateSlot(&T::Global);
  cl::registerLibraryOptions(
      {[](function_ref<void(StringRef)> F) {
         T::table().forEachOptionName(
             [&](StringRef Name) { F(Name.rtrim('=')); });
       },
       [](ArrayRef<const char *> Argv, unsigned &Index, raw_ostream &Errs) {
         // A Flag, Joined or Separate option spans at most two arguments.
         unsigned I = 0;
         bool Ok = T::table().applyOneArg(
             Argv.slice(Index).take_front(2), I, Errs,
             [](const Arg &A) { return T::Global.apply(A); });
         Index += I;
         return Ok;
       },
       [](raw_ostream &OS, bool ShowHidden) {
         if (!ShowHidden)
           return;
         OS << '\n';
         T::table().printHelpOptions(OS, ShowHidden);
       },
       [] { T::Global = T(); }});
}

template <class T> struct RegisterLibraryOptions {
  RegisterLibraryOptions() { registerLibraryOptions<T>(); }
};

} // namespace llvm::opt

#endif // LLVM_OPTION_LIBRARYOPTIONS_H
