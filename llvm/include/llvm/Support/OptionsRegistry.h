//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_OPTIONSREGISTRY_H
#define LLVM_SUPPORT_OPTIONSREGISTRY_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"
#include <memory>
#include <utility>

namespace llvm {

/// The command line options of each library, as code working on a context
/// (an LLVMContext, say) reads them: the copy a tool attached with set, else
/// the process-wide instance cl::ParseCommandLineOptions fills. A library's
/// struct T has static members Global and Slot, from allocateSlot().
class OptionsRegistry {
  SmallVector<std::unique_ptr<void, void (*)(void *)>, 4> Slots;

public:
  LLVM_ABI OptionsRegistry();

  template <class T> const T &get() const {
    if (LLVM_LIKELY(T::Slot < Slots.size()))
      return *static_cast<const T *>(Slots[T::Slot].get());
    return T::Global;
  }
  /// Attaches a copy of \p O, which the registry owns, and returns it.
  template <class T> T &set(T O) {
    T *Copy = new T(std::move(O));
    set(T::Slot, Copy, [](void *P) { delete static_cast<T *>(P); });
    return *Copy;
  }

  /// Allots a slot to a struct whose process-wide instance is \p Global;
  /// registries created afterwards start with it in the slot. Called at
  /// static initialization or when a plugin loads.
  LLVM_ABI static unsigned allocateSlot(void *Global);

private:
  LLVM_ABI void set(unsigned Slot, void *O, void (*Delete)(void *));
};

} // namespace llvm

#endif // LLVM_SUPPORT_OPTIONSREGISTRY_H
