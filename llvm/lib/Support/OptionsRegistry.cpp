//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/OptionsRegistry.h"
#include <cassert>

using namespace llvm;

static unsigned NumSlots;

unsigned OptionsRegistry::allocateSlot() { return NumSlots++; }

void OptionsRegistry::set(unsigned Slot, void *O, void (*Delete)(void *)) {
  assert(Slot < NumSlots && "options struct is not registered");
  while (Slots.size() <= Slot)
    Slots.emplace_back(nullptr, nullptr);
  Slots[Slot] = {O, Delete};
}
