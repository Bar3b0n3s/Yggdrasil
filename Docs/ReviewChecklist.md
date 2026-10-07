# Review Checklist

This is the pre-commit code review checklist from Architecture §15.9. The `commit-review` skill (`.claude/skills/commit-review/SKILL.md`; Architecture calls it `code-review`, see `Docs/Decisions/0002-m0-deviations.md`) applies it to the staged diff before every commit; a human reviewer uses the same list.

## How to use it

- Review the whole change: every staged file, the code around each hunk, and the files the change should have touched but did not (tests, docs, generated files, premake).
- Give each item one of three outcomes:
  - **pass**;
  - **n/a**, with a reason when the reason is not obvious;
  - **finding**, with file, line, the item, the concrete failure scenario and the fix.
- **Blocking**, so the commit does not happen: a failing `PreCommit.py`, any finding against an item in §1–§13, missing tests for new behaviour, and any style violation.
  - A finding is resolved by fixing it and re-reviewing the fix.
  - It may also be closed by an ADR in `Docs/Decisions/` when the rule itself is wrong.
  - "Will fix later" never resolves a finding.
- **Non-blocking:** naming taste beyond the style guide, optional simplifications, and follow-ups outside the change's scope. Record each one in the review summary, and spin it off as its own task when it is worth doing.
- **Recording:** the commit message ends with the trailer
  `Reviewed: <outcome>; <n> findings (<n> fixed); PreCommit <green|red> (<stages>)`, for example
  `Reviewed: approved; 3 findings (3 fixed); PreCommit green (generate, checkbuildconfig, lint, lint self-test, format, Debug build, unit 42/42)`.
  A milestone's contract commit records its mode: `PreCommit green, contract mode (...)`.

Section references (§) point to `Docs/Architecture.md` unless marked CodeStyle (`Docs/CodeStyle.md`) or Roadmap (`Docs/Roadmap.md`).

## 0. Gate and scope

- [ ] `python Scripts/PreCommit.py` passed on the final tree, after the last edit. A run that predates a fix does not count.
- [ ] The gate ran in the right mode (Roadmap rule 3, `Docs/Decisions/0004-contract-stub-gate.md`). Only a milestone's contract commit runs `PreCommit.py --contract`. Every other commit ran strict, so no `ENGINE_CONTRACT_STUB` remains and no test case is skipped outside the child-process targets (`Test::ChildTargetSuite`).
- [ ] The diff does what the task asks and nothing else. There are no unrelated edits, drive-by reformatting or stray files.
- [ ] Only files the task owns are changed. Changes to shared integration files (premake files, `Scripts/ModuleRules.json`, `BuiltinComponents.h`, `RegisterBindings.cpp`, `RegisterMethods.cpp`, `Docs/Reference/*`) come from, or are approved by, their owner (Roadmap rule 4).
- [ ] Frozen contract headers are unchanged, or the change carries the contract owner's review (Roadmap rule 3).
- [ ] Milestone commit: the full `python Scripts/CI.py` is green in the §15.8 configurations.

## 1. Correctness

- [ ] The logic is right for normal, boundary and empty inputs: zero entities, empty strings, maximum sizes, the first and last element.
- [ ] Every new failure path is reachable and tested, and leaves state consistent, with no half-applied mutations.
- [ ] Integer arithmetic has no overflow, sign or narrowing bugs. Every narrowing conversion is an explicit `static_cast`, and the value is known to fit.
- [ ] Floating-point code handles NaN and Inf from external input. Values are rejected before they reach Jolt, the renderer or serialized data (§5.4, §9.1).
- [ ] Code that reads binary or JSON data is bounds-checked and returns a `Result`. Hostile input never crashes it.

## 2. Style and naming (CodeStyle; `Format.py --check` and `Lint.py` catch most of it)

- [ ] clang-format 22.x clean. `// clang-format off` appears only around tabular data, in matched pairs.
- [ ] Naming follows CodeStyle §2:
  - PascalCase for types, functions, enumerators and `constexpr` constants;
  - `m_`, `s_` and `g_` prefixes;
  - camelCase for locals and parameters;
  - the `ENGINE_` prefix on macros;
  - acronyms keep their capitalization.
- [ ] The code is in `namespace Engine`, with only the allowed sub-namespaces. The product name appears nowhere in code, macros, file names, shaders or script symbols.
- [ ] Includes follow CodeStyle §4.4: group order, quoted full paths, no relative paths, and `EnginePCH.h` first only in Engine `.cpp` files. Headers are self-contained and never include a PCH.
- [ ] Class layout follows CodeStyle §4.5. Single-argument constructors are `explicit`, overrides use `override` without `virtual`, and every member has a default initializer.
- [ ] Results, handles and factories are `[[nodiscard]]`; `const` is used correctly; parameters are passed per CodeStyle §6.
- [ ] No C-style or functional casts. `reinterpret_cast` and `const_cast` appear only for the cases CodeStyle §8 allows, with a comment.
- [ ] `auto` is used per CodeStyle §9.
- [ ] Comments explain why. There is no commented-out code and no `#if 0`. A `TODO` never marks a known bug, missing error handling or unfinished behaviour.
- [ ] No forbidden constructs (CodeStyle §13):
  - modules, `std::stacktrace`, coroutines, `std::flat_map`/`std::flat_set`;
  - `std::print`, `printf`, `std::cout`;
  - `rand()`, `goto`, `std::endl`, `std::bind`, `volatile` for synchronization;
  - naked `new`/`delete`, `using namespace std`.

## 3. Architecture and layers (§3)

- [ ] Every include respects the layer table and the explicit layer-5 DAG in `Scripts/ModuleRules.json`. `Renderer` does not include `Scene`, and `AssetPipeline` is never included by Runtime code.
- [ ] Public headers expose no GLFW, Jolt, Luau, miniaudio, cgltf or stb types. NVRHI types appear only in Graphics, Renderer and ImGui headers, and EnTT only in Scene headers. `RenderSnapshot.h` and `DebugDrawList.h` stay free of NVRHI.
- [ ] Platform-specific code is only in `Platform/<OS>/` (or the Graphics device setup) and is guarded by `ENGINE_PLATFORM_*`.
- [ ] No new mutable global beyond the process-level state that §3 rule 5 lists.
- [ ] Services are passed explicitly (`EngineContext&` or the specific service). There is no `Application::Get()`-style singleton access.
- [ ] Mutations go through the single mutation path (Commands, §12.3). Scene copies go through the serializer, and loading goes through the cooked-bytes loader (§1.3 "One path per job").
- [ ] A new abstraction carries a written justification ("pays for itself", §1.3), and nothing falls under the non-goals (§1.2).

## 4. Error handling (§4.5–4.6)

- [ ] No `throw` in first-party code. `try`/`catch` appears only in the allowlisted boundary files, and never as `catch (...)` around `lua_*` calls.
- [ ] Expected failures return `Result<T>`/`Status` with a precise `ErrorCode` and a message that names the thing. They gain context (`WithContext`, `WithLocation`, `WithHint`) as they cross layers.
- [ ] No `Result` is discarded. Every third-party result is checked: `VkResult`, NVRHI null handles, file I/O, miniaudio, Jolt `ShapeSettings::Create`, JSON parsing.
- [ ] Asserts guard only programmer errors, and their conditions have no side effects. External input (files, scenes, scripts, automation parameters, user actions) is validated with `Result`. `VERIFY` is used where corruption would follow in Dist. `assert()` is never used.
- [ ] JSON is read through `JsonReader` and parsed with `json::parse(text, nullptr, false)`. There is no `json::at` or `get<>` outside `Core/Json`.
- [ ] `std::filesystem` is used only through its `std::error_code` overloads.
- [ ] Fatal environment failures go through `FatalError` with the documented exit code (§4.1 table).
- [ ] Logging follows CodeStyle §11: the right macro family and level, `{}` placeholders, quoted names, and nothing at Info or Trace every frame.

## 5. Ownership and lifetime (§4.7, CodeStyle §7)

- [ ] `Scope<T>` is the default. `Ref<T>` is used only for genuinely shared immutable data, and there is no `WeakRef`.
- [ ] Raw pointers and references are non-owning and are not stored beyond the call, unless they are documented back-references.
- [ ] Third-party objects use RAII wrappers or their own handle types (`nvrhi::*Handle`, `JPH::Ref`), and are never wrapped in `Ref`.
- [ ] No pointer or reference to a component survives a structural change. Entities are referenced by `UUID` and assets by `AssetHandle`.
- [ ] `std::string_view` and `std::span` are not stored beyond the call unless the lifetime is documented.
- [ ] Stored or deferred lambdas list their captures, and every capture outlives the lambda.
- [ ] Each GPU object has exactly one owner, and teardown order matches §8.14. Live-object counters return to zero.

## 6. Determinism (§1.3, §4.12, §9.1)

- [ ] No CRT transcendental functions on the simulation path (Core, Scene, Physics, Scripting, Session); DetMath is used instead.
- [ ] No observable output (events, serialization, hashes, automation results) depends on unordered container iteration, pointer values, `entt::entity` values or hash-map order.
- [ ] Simulation reads only simulation time (`SimStep`), never the wall clock or the frame delta.
- [ ] Randomness and runtime UUIDs come from seeded instances. There is no `std::random_device` and no unseeded generator on the simulation path.
- [ ] No floating-point build flag change weakens the precise model, and `JPH_CROSS_PLATFORM_DETERMINISTIC` stays everywhere. `CheckBuildConfig.py` passes.
- [ ] Committed hashes and replays are not split per configuration. A configuration-dependent result is treated as a bug.

## 7. Threading (§4.11)

- [ ] The ECS, scripts, physics stepping, NVRHI recording and submission, and ImGui are touched only on the main thread. Debug thread asserts are present at new entry points that need them.
- [ ] Jobs capture values, return `Result`s and never touch the ECS. Completions run through `MainThreadQueue`, and stale completions are dropped.
- [ ] Shared state has a clear synchronization owner. There are no data races, no `volatile` used for synchronization, and no lock held across a callback into foreign code.
- [ ] Tests that involve jobs use `JobSystem(0)` or another deterministic setup.

## 8. Tests (CodeStyle §14, §15)

- [ ] Every new or changed behaviour has a test, and every bug fix has a regression test that fails without the fix.
- [ ] Tests are meaningful: they assert outcomes rather than mere execution, cover failure paths, and would catch a plausible regression. No test was weakened, skipped or deleted to get green. A permanent `doctest::skip` exists only on a child-process target, with `doctest::test_suite(Test::ChildTargetSuite)` in the same decorator expression.
- [ ] Naming and location follow the conventions: `Tests/Source/<path>Tests.cpp`, `TEST_SUITE("<Module>")`, and `TEST_CASE("<Unit>: <behaviour>")`. Roadmap acceptance names are used verbatim.
- [ ] Tests are deterministic: no sleeps or wall-clock time, fixed seeds, no network, temporary directories only, and no dependence on order. The wall-clock exceptions are the windowed child "FrameLoop: a minimized window uses little CPU time per second" (ADR 0005 decision 13) and the Python `test_busy_watchdog_reports_phase` (ADR 0008 decision 15); another needs its own ADR entry. Bounded waits (ADR 0008 decision 15) are allowed: a generous deadline that only bounds the failure of a wait for another thread or process (`Test::WaitUntil`), or a timeout whose peer never answers, never asserted on and never deciding a passing outcome. A wait bounded by a spin count is a finding.
- [ ] Expected error logs use `Test::ExpectLog`, and expected asserts are death tests. GPU tests skip with a reason unless `--require-gpu`.
- [ ] Parity (Roadmap rule 5): every editor-visible capability has its automation method and a Python test. New registry entries have FeatureTest coverage once M14 has landed.
- [ ] Fixtures are small, committed, licensed (`Tests/Data/LICENSES.md`) and produced by a committed generator where possible.

## 9. Documentation and generated files

- [ ] A change in behaviour, CLI or format updates the owning document: Architecture, Roadmap, `AGENTS.md`, a skill, or a `VENDOR.md`. A deliberate deviation has an ADR.
- [ ] Generated files are regenerated and committed: `Docs/Reference/*`, `Engine.d.luau` and `catalog.json`. `GenerateDocs.py --check` is clean once it exists.
- [ ] Every new script has `--help`, `--json` where sensible, and documented exit codes.
- [ ] New reflected types, fields, methods and script APIs have descriptions (coverage gate 7).

## 10. No debug leftovers

- [ ] No temporary logging, `printf` debugging, hard-coded local paths, disabled tests, `#if 0`, commented-out code or forced flags. Outside a contract commit, no `ENGINE_CONTRACT_STUB` stub remains.
- [ ] No warning suppressed to get green. A suppression is acceptable only when it is file-scoped in a vendor premake file, with the reason recorded in its `VENDOR.md`.
- [ ] No generated, build or machine-specific files are committed: `bin/`, project files, `.mcp.json`, `Library/`, `compile_commands.json`.
- [ ] No secrets, tokens or personal data appear in code, fixtures, logs or test output.

## 11. Cross-platform (§16)

- [ ] The code compiles on MSVC 14.51, GCC 14 (libstdc++ 14), Clang 19+ and Apple Clang (Xcode 26). It uses no feature one of them lacks (CodeStyle §13). CI is the judge for Linux and macOS.
- [ ] Paths are UTF-8 internally and converted only in `Platform`. Data paths use `/`. File names respect the case policy, so nothing relies on case-insensitive lookup.
- [ ] Binary formats are little-endian and asserted. Text is LF, and JSON is UTF-8 without a BOM.
- [ ] Premake changes are written for every OS branch (Windows, Linux, macOS). New files are picked up after `Generate.py`.
- [ ] Python scripts work on all three OSes: `pathlib`, no shell, `python3` on POSIX, no Windows-only APIs outside guarded branches.

## 12. Vendored code and build configuration

- [ ] No upstream source under `Vendor/` is edited or reformatted. Build changes are in `Vendor/<Lib>/premake5.lua` and are recorded in `VENDOR.md`.
- [ ] `Use<Lib>()` defines match `VENDOR.md` verbatim, and `python Scripts/CheckBuildConfig.py` passes.
- [ ] `NDEBUG` appears only at workspace scope for Dist. First-party projects keep `/W4 /WX` (`-Wall -Wextra -Wshadow -Werror`), and vendor headers stay external.
- [ ] A new library or asset meets all of these:
  - official source and pinned version or commit;
  - verified checksum and committed license;
  - a `VENDOR.md` or `LICENSES.md` entry;
  - covered by the approvals record (`Docs/Decisions/0001-approvals.md`).

## 13. Security of automation surfaces (where relevant)

- [ ] Automation stays loopback-only and token-authenticated, rejects HTTP probes and oversized frames, and is absent from Dist (§13.2, §14.4).
- [ ] Every path from a request, script or asset is confined: `VfsPath` rejects `..`, absolute paths, NUL bytes and reserved names. Only `asset.import` reads an absolute source path, and only read-only.
- [ ] No method or script API executes processes, shell commands or arbitrary files. Script sandbox escapes (§11.1) stay closed.
- [ ] Responses are size-bounded, and offloaded results stay inside the project's `Library/`.
- [ ] Downloads (`Setup.py`, `FetchAssets.py`) use allowlisted official hosts over HTTPS, pinned versions and verified checksums.
- [ ] CI workflows use least-privilege `permissions`, actions pinned to a commit SHA, `persist-credentials: false`, and no untrusted input interpolated into `run:` scripts.
