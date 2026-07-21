<!-- If you want to modify sections/contents permanently, you should modify both
ReleaseNotes.md and ReleaseNotesTemplate.txt. -->

(lld-release-release-notes)=

# lld {{ release | default("") }} Release Notes

```{contents}
:local: true
```

::::{only} PreRelease

:::{warning}
These are in-progress notes for the upcoming LLVM {{ release | default("") }} release.
Release notes for previous releases can be found on
[the Download Page](https://releases.llvm.org/download.html).
:::
::::

## Introduction

This document contains the release notes for the lld linker, release {{ release | default("") }}.
Here we describe the status of lld, including major improvements
from the previous release. All lld releases may be downloaded
from the [LLVM releases web site](https://llvm.org/releases/).

## Non-comprehensive list of changes in this release

### ELF Improvements

* Thunks in a thunk section are now sorted by descending destination address,
  preventing cascaded short-to-long thunk promotions from exceeding the
  address assignment pass limit.
  ([#61250](https://github.com/llvm/llvm-project/issues/61250))

### Breaking changes

### COFF Improvements

### MinGW Improvements

### MachO Improvements

### WebAssembly Improvements

#### Fixes
