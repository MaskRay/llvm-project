//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Support for the struct -gen-opt-parser-defs generates from a library's
// OptionsStruct: the conversions its apply() uses, and its registration. A
// library's .cpp includes this header, then defines OPTIONS_STRUCT_DEFS and
// includes its .inc.
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

// Each accepts the spellings cl::opt accepts for the type.
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
std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, bool>
parseArgValue(StringRef S, T &V) {
  if constexpr (std::is_floating_point_v<T>)
    return to_float(S, V);
  else
    return to_integer(S, V);
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

/// Allots T's slot in OptionsRegistry and registers T's options with cl::.
/// A library's .cpp defines one as a static object, as does a plugin's.
template <class T> struct RegisterLibraryOptions {
  RegisterLibraryOptions() {
    T::Slot = OptionsRegistry::allocateSlot();
    cl::registerLibraryOptions(
        {[](function_ref<void(StringRef)> F) {
           // cl:: spells the Joined "name=" as "name".
           T::table().forEachOptionName(
               [&](StringRef Name) { F(Name.rtrim('=')); });
         },
         [](ArrayRef<const char *> Argv, unsigned &Index, raw_ostream &Errs) {
           // An option of an OptionsStruct spans at most two arguments.
           return T::table().applyOneArg(
               Argv.take_front(Index + 2), Index, Errs,
               [](const Arg &A) { return T::Global.apply(A); });
         },
         [](raw_ostream &OS, bool ShowHidden) {
           // The multiclasses make every option HelpHidden.
           if (!ShowHidden)
             return;
           OS << '\n';
           T::table().printHelpOptions(OS, ShowHidden);
         },
         [] { T::Global = T(); }});
  }
};

} // namespace llvm::opt

#endif // LLVM_OPTION_LIBRARYOPTIONS_H
