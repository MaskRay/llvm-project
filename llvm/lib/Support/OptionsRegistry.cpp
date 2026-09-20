//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/OptionsRegistry.h"
#include <vector>

using namespace llvm;

// The process-wide instance of each struct allocateSlot allotted a slot, by
// slot. Like a target's registration, a slot is allotted at static
// initialization or when a plugin loads, before registries read it from other
// threads.
static std::vector<void *> &globalOptions() {
  static std::vector<void *> Global;
  return Global;
}

static void noDelete(void *) {}

OptionsRegistry::OptionsRegistry() {
  for (void *O : globalOptions())
    Slots.emplace_back(O, noDelete);
}

unsigned OptionsRegistry::allocateSlot(void *Global) {
  globalOptions().push_back(Global);
  return globalOptions().size() - 1;
}

void OptionsRegistry::set(unsigned Slot, void *O, void (*Delete)(void *)) {
  // A registry older than the struct's registration lacks its slot.
  for (unsigned I = Slots.size(); I <= Slot; ++I)
    Slots.emplace_back(globalOptions()[I], noDelete);
  Slots[Slot] = {O, Delete};
}
