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
/// struct T has static members Global and Slot; Slot is -1 until
/// allocateSlot() allots it.
class OptionsRegistry {
  // The attached copies by slot; null reads T::Global.
  SmallVector<std::unique_ptr<void, void (*)(void *)>, 0> Slots;

public:
  template <class T> const T &get() const {
    if (T::Slot < Slots.size())
      if (const void *O = Slots[T::Slot].get())
        return *static_cast<const T *>(O);
    return T::Global;
  }

  /// Attaches a copy of \p O, which the registry owns, and returns it. The
  /// copy replaces, and invalidates references to, one attached before.
  template <class T> T &set(T O) {
    T *Copy = new T(std::move(O));
    set(T::Slot, Copy, [](void *P) { delete static_cast<T *>(P); });
    return *Copy;
  }

  /// Like a target's registration, runs at static initialization or when a
  /// plugin loads, before other threads use the slot.
  LLVM_ABI static unsigned allocateSlot();

private:
  LLVM_ABI void set(unsigned Slot, void *O, void (*Delete)(void *));
};

} // namespace llvm

#endif // LLVM_SUPPORT_OPTIONSREGISTRY_H
