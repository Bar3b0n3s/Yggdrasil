# SheenBidi (vendored)

| Field | Value |
|---|---|
| Upstream | https://github.com/Tehreer/SheenBidi |
| Version / tag | `v2.9.0` (released 2025-06-10) |
| Commit | `83f77108a2873600283f6da4b326a2dca7a3a7a6` (the tag `v2.9.0`, an annotated tag object `829d99918adf05761e9f35611dc13e76e6810f80`) |
| Archive | `https://github.com/Tehreer/SheenBidi/archive/refs/tags/v2.9.0.tar.gz`, 2,532,288 bytes, SHA-256 `e90ae142c6fc8b94366f3526f84b349a2c10137f87093db402fe51f6eace6d13` (GitHub's tag archive; upstream publishes no release asset) |
| Date vendored | 2026-11-03 (M22's contract C22.3, Docs/Decisions/0033-m22-contract.md decision 41) |
| License | `Apache-2.0` - `LICENSE` (upstream ships no `NOTICE` file) |
| Local modifications | None. Every kept file is byte-identical to the archive's (all of them use LF line endings already). |
| Approval | `Docs/Decisions/0001-approvals.md` (HarfBuzz and SheenBidi, ADR 0028 decision 7) |

Why v2.9.0 and not the newer v3.0.0 (2026-01-24), reconsidered by the review of C22.3 (ADR 0033 decision 41): v3.0.0's
process-level state (one-time initializations, atomic reference counts, a process-wide pool of three 8 KiB scratch
buffers that a thread holds through a thread-local slot while it builds a paragraph or a line, with the heap as fallback,
and a default allocator only `SBAllocatorSetDefault` changes) is of the kind Architecture §3 rule 5 allows a vendored
library, as HarfBuzz's is: thread-safe, and no result depends on it. So that state does not decide the version. What
does: v3.0.0's release notes list additions (a text-editing and analysis API, a public allocator abstraction, the
concurrency primitives they need), a refinement of its run and bracket queues, performance work and Unicode 17.0 data,
and no correction of the bidirectional algorithm or the script locator, the two parts the engine calls (UAX #9 and
UAX #24, which both versions implement); the editing API is code the engine would carry and never call (75 source files
against v2.9.0's 47); and v3.0.0's build depends on detecting the compiler's C11 threads, thread-local storage and
atomics (an `#error` without them unless `SB_CONFIG_DISABLE_SCRATCH_MEMORY`, a define of the `SB_CONFIG_` family the
engine keeps empty), branches its GCC, Clang and Apple Clang builds would add unverified (ADR 0028 decision 10). Its
Unicode 17.0 data differs from v2.9.0's only in characters added since, which no font the engine bakes for its tests
draws. v2.9.0 keeps no process-level state (its objects are created and released by each call, their reference counts
plain). A later update follows "Updating" below.

## What was kept

`LICENSE`, `Headers/SheenBidi/` (the 15 public headers) and `Source/` (its 47 `.c` and `.h` files, `SheenBidi.c` among
them although it is not compiled, so the directory stays a straight copy of upstream's): 63 files, 609,024 bytes.

The kept tree's digest is SHA-256 `63b5ea40468a8de4242bb3eb39f695835e16d7e0279362d6541cfc83e01a13b9`, computed as
Vendor/HarfBuzz/VENDOR.md says. `LICENSE`'s SHA-256 is `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`
(the Apache License 2.0 text, byte-identical to `Resources/Licenses/MoltenVK-LICENSE.txt`).

## What was removed

`Tests/`, `Tools/` (the Unicode data generators), the build files (`CMakeLists.txt`, `Makefile`, `meson.build`,
`meson_options.txt`, `SheenBidi.cmake.in`, `sheenbidi.pc.in`) and `README.md`.

## Premake project (`premake5.lua`)

| Project | Contents | Depends on |
|---|---|---|
| `SheenBidi` | every `Source/*.c` but `SheenBidi.c` | - |

Build settings:

- C, a static library of every source compiled on its own, as upstream's build does without its unity option.
  `Source/SheenBidi.c` is the unity build's file (it includes the others under `SB_CONFIG_UNITY` and holds nothing else),
  so it is not compiled.
- No defines: `SB_CONFIG_UNITY` would also change the public `SBConfig.h`'s `SB_INTERNAL`, which every consumer would
  have to define alike, and `SB_CONFIG_LOG`, `SB_CONFIG_DLL_EXPORT` and `SB_CONFIG_DLL_IMPORT` are for logging and
  shared libraries. The `SB_CONFIG_` family stays empty in every project (`Scripts/CheckBuildConfig.py`'s
  `VENDOR_ABI_FAMILIES`, and `Scripts/Lint.py`'s `banned-abi-macro`).
- Floating point: the precise model without contraction, as Vendor/HarfBuzz/VENDOR.md says for HarfBuzz (the library
  computes no floating-point value today; the rule keeps it so). `Scripts/CheckBuildConfig.py` checks it
  (`PRECISE_VENDOR_PROJECTS`; the fixture `Tests/Data/BuildConfig/FastMathSheenBidi` must fail).
- Debug, Release and Dist follow the other vendored libraries.

## Consumer usage

`Dependencies.lua`'s `UseSheenBidi()`: the external include directory `Vendor/SheenBidi/Headers` (consumers include
`<SheenBidi/SheenBidi.h>`), no defines, and the link of the `SheenBidi` library in every executable, as `UseHarfBuzz()`.
`Scripts/ModuleRules.json` gives it to the Renderer module, privately (the text-measurement layer's bidirectional runs);
no public header exposes a SheenBidi type. How the engine calls it is ADR 0033's decision 43.

## Verification

- The kept files match the tag archive's byte for byte (the tree digest above).
- `python Scripts/CheckBuildConfig.py` passes on the workspace and fails on `FastMathSheenBidi`.
- The library builds without warnings in Debug and Release with MSVC (14.51), and in Release and Dist with clang-cl (the
  portability stage's build of Architecture §15.8), where Dist reports one warning in the vendored code, tolerated as
  the portability stage tolerates vendored code's warnings: `SBParagraph.c`'s `stringLength`, which only an assertion
  reads, is unused once `NDEBUG` removes it (`-Wunused-variable`). Its GCC, Clang and Apple Clang builds are never
  compiled (`Docs/Decisions/0028-backlog-program.md` decision 10).

## Updating

1. Download `https://github.com/Tehreer/SheenBidi/archive/refs/tags/<tag>.tar.gz` and record its SHA-256.
2. Replace `Headers/`, `Source/` and `LICENSE` with the archive's.
3. Update this file (version, commit, archive, digests, the kept count), rebuild every configuration and the clang-cl
   portability build, run `python Scripts/CheckBuildConfig.py` and the text-measurement tests.

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).

| Field | Value |
|---|---|
| Name | SheenBidi |
| Upstream | https://github.com/Tehreer/SheenBidi |
| Version | 2.9.0 |
| License | Apache-2.0 |
| Shipped | yes |
| Projects | SheenBidi |
| License files | LICENSE |
