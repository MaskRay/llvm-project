//===- llvm/Use.h - Definition of the Use class -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This defines the Use class.  The Use class represents the operand of an
/// instruction or some other User instance which refers to a Value.  The Use
/// class keeps the "use list" of the referenced value up to date.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_USE_H
#define LLVM_IR_USE_H

#include "llvm-c/Types.h"
#include "llvm/Support/CBindingWrapping.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

template <typename> struct simplify_type;
class User;
class Value;

/// A Use represents the edge between a Value definition and its users.
///
/// This is notionally a two-dimensional linked list. It supports traversing
/// all of the uses for a particular value definition. It also supports jumping
/// directly to the used value when we arrive from the User's operands, and
/// jumping directly to the User when we arrive from the Value's uses.
class Use {
public:
  Use(const Use &U) = delete;

  /// Provide a fast substitute to std::swap<Use>
  /// that also works with less standard-compliant compilers
  LLVM_ABI void swap(Use &RHS);

private:
  /// Destructor - Only for zap()
  ~Use() { removeFromList(); }

  /// Constructor
  Use(User *Parent) : Parent(Parent) {}

public:
  friend class Value;
  friend class User;

  operator Value *() const { return Val; }
  Value *get() const { return Val; }

  /// Returns the User that contains this Use.
  ///
  /// For an instruction operand, for example, this will return the
  /// instruction.
  User *getUser() const { return Parent; };

  LLVM_ABI inline void set(Value *Val);

  LLVM_ABI inline Value *operator=(Value *RHS);
  LLVM_ABI inline const Use &operator=(const Use &RHS);

  Value *operator->() { return Val; }
  const Value *operator->() const { return Val; }

  Use *getNext() const { return Next; }

  /// Return the operand # of this use in its User.
  LLVM_ABI unsigned getOperandNo() const;

  /// Destroys Use operands when the number of operands of
  /// a User changes.
  LLVM_ABI static void zap(Use *Start, const Use *Stop, bool del = false);

private:

  Value *Val = nullptr;
  Use *Next = nullptr;
  Use **Prev = nullptr;
  User *Parent = nullptr;

  void addToList(Use **List) {
    Next = *List;
    if (Next)
      Next->Prev = &Next;
    Prev = List;
    *Prev = this;
  }

  void removeFromList() {
    if (Prev) {
      *Prev = Next;
      if (Next) {
        Next->Prev = Prev;
        Next = nullptr;
      }

      Prev = nullptr;
    }
  }
};

/// Allow clients to treat uses just like values when using
/// casting operators.
template <> struct simplify_type<Use> {
  using SimpleType = Value *;

  static SimpleType getSimplifiedValue(Use &Val) { return Val.get(); }
};
template <> struct simplify_type<const Use> {
  using SimpleType = /*const*/ Value *;

  static SimpleType getSimplifiedValue(const Use &Val) { return Val.get(); }
};

// Create wrappers for C Binding types (see CBindingWrapping.h).
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(Use, LLVMUseRef)

} // end namespace llvm

#endif // LLVM_IR_USE_H
