// REQUIRES: x86-registered-target, aarch64-registered-target

/// The direct code generator leaves a function to the regular pipeline when
/// it sees an attribute, module flag or metadata kind it does not know. Check
/// that what clang emits under common options is known: a new one must be
/// classified in llvm/lib/CodeGen/AsmPrinter/DirectCodeGen.cpp.

// DEFINE: %{direct} = -O0 -c %s -o %t.o -mllvm -direct-codegen -mllvm -direct-codegen-abort
// RUN: %clang --target=x86_64-linux-gnu %{direct}
// RUN: %clang --target=x86_64-linux-gnu %{direct} -fPIC -g -fstack-protector-strong -fcf-protection -fstack-clash-protection
// RUN: %clang --target=x86_64-linux-gnu %{direct} -fno-delete-null-pointer-checks -ffast-math -fno-plt -fwrapv -fno-builtin -ftrivial-auto-var-init=zero -fvisibility=hidden -funique-internal-linkage-names -Wframe-larger-than=4096
// RUN: %clang --target=x86_64-linux-gnu %{direct} -fprofile-instr-generate -fcoverage-mapping -fsanitize=function
// RUN: %clang --target=aarch64-linux-gnu %{direct}
// RUN: %clang --target=aarch64-linux-gnu %{direct} -fPIC -g -fstack-protector-strong -mbranch-protection=standard -fstack-clash-protection
// RUN: %clang --target=aarch64-linux-gnu %{direct} -march=armv9-a -fno-delete-null-pointer-checks -fprofile-instr-generate -fsanitize=function

/// -S takes the same path as -c.
// RUN: %clang --target=x86_64-linux-gnu %{direct} -S -g -fstack-protector-strong
// RUN: %clang --target=aarch64-linux-gnu %{direct} -S -g -mbranch-protection=standard

struct S { long a; double b; };
int ext(const char *, int);
static int counter;

static int local(int x) { return x + counter++; }

double fp(double a, float b, int c) { return a * b + c - (a > b ? a : b); }

int sw(int x) {
  switch (x) {
  case 0: return 3;
  case 7: return 11;
  default: return local(x);
  }
}

long agg(struct S *s, int n) {
  char buf[32];
  long sum = 0;
  for (int i = 0; i < n; ++i) {
    buf[i & 31] = i;
    sum += s[i].a + buf[i & 31];
  }
  return sum + ext(buf, n);
}

int ind(int (*f)(int), int x) { return f ? f(x) : sw(x); }
