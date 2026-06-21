# dynarmic patch — preserved here because the submodule can't be pushed

`external/dynarmic` is a git submodule pointing at upstream
`https://github.com/Vita3K/dynarmic.git`, to which we have no write access (and
there is no personal fork configured). The change below was committed inside the
submodule locally (commit `274661eb`, branch
`developers/lonycell/fix-skinned-mesh-render-cpu`), but that commit lives only on
this machine — a fresh clone would fail `git submodule update` on its SHA.

To keep the change from being lost, it is archived here in the main repo.

## What the change does

Adds `Dynarmic::Backend::SetNonJitFaultFallback()`. On macOS the JIT installs a
task-level Mach exception port that intercepts every `EXC_BAD_ACCESS`, including
faults from non-JIT host code on protected guest memory. Without forwarding, the
host's own SIGSEGV-based memory-protection handler never runs. The fallback routes
such non-JIT faults back to the host handler so memory-protection features (GPU
memory mapping write-watch / page recovery) work, instead of aborting.

Note: the CPU vertex-job skinning fix (the main change on this branch) does **not**
depend on this patch — it works with memory mapping disabled. This patch only
matters if the macOS memory-mapping path is revisited.

## Contents

- `0001-backend-exception_handler-forward-non-JIT-faults-on-.patch` — `git am`-able patch.
- `src/dynarmic/backend/exception_handler.h`
- `src/dynarmic/backend/exception_handler_macos.cpp` — full copies of the modified
  files (fallback if the patch doesn't apply cleanly to a different dynarmic base).

## How to re-apply (after a fresh submodule checkout)

```sh
cd external/dynarmic
git am ../../patches/dynarmic/0001-*.patch
# or, if it doesn't apply cleanly, copy the files from patches/dynarmic/src/ over
# the matching paths in external/dynarmic/src/.
```

## How to make it properly reproducible (recommended)

Fork dynarmic, push the branch, and repoint the submodule:

```sh
# after creating github.com/lonycell/dynarmic
cd external/dynarmic
git remote add fork git@github.com:lonycell/dynarmic.git
git push fork developers/lonycell/fix-skinned-mesh-render-cpu
cd ../..
git config -f .gitmodules submodule.external/dynarmic.url git@github.com:lonycell/dynarmic.git
git add .gitmodules && git commit -m "dynarmic: point submodule at fork with the macOS fault-forwarding change"
```
