# 0002 — M0 build and tooling deviations

- **Status:** accepted for M0. The amendments listed at the end have been applied (2026-10-05) to `Docs/Architecture.md` v1.2, `Docs/Roadmap.md` v1.2 and `Docs/CodeStyle.md` §15; those documents are again the rule, and this record keeps the reason for each change.
- **Date:** 2026-10-05
- **Context:** M0 (Roadmap "M0 — Bootstrap") departs from the authoritative documents in the places below. Each one was forced by a tool's behaviour or chosen for a concrete reason. `AGENTS.md` ("Deviations") and `Docs/ReviewChecklist.md` §9 require a record of each.

## Decisions

### 1. Clang floating-point flags: `-fno-fast-math -ffp-contract=off`

Architecture §2.2 and Appendix A, and Roadmap M0, specify `-ffp-model=precise -ffp-contract=off` for Clang.

- **What M0 does:** GCC and Clang (gmake, ninja, xcode4 and the compile-commands database) use `-fno-fast-math -ffp-contract=off`. clang-cl takes the same options as `/clang:-fno-fast-math /clang:-ffp-contract=off`, because it accepts GCC-style `-f` options only through `/clang:`. This applies to first-party projects (`Dependencies.lua`) and to JoltPhysics (`Vendor/JoltPhysics/premake5.lua`).
- **Why:** `-ffp-model=precise` implies `-ffp-contract=on`. Spelling both makes Clang 22 report `-Woverriding-option`, which is an error under `-Werror`. Clang's default model is already precise, so `-fno-fast-math -ffp-contract=off` gives exactly "precise without contraction". `-fno-fast-math` also resets the `-ffast-math` that premake's xcode4 generator adds for `optimize "Full"` (Dist).
- **Instruction set under clang-cl:** `/arch:SSE4.2` is filtered on `toolset:msc*`, so clang-cl gets `-msse4.2 -mpopcnt` from the `toolset:gcc or clang` filter instead. That is the same instruction set for Jolt and every consumer; `Scripts/CheckBuildConfig.py` compares them.

### 2. `Engine/Config/entt/ext/config.h` moves to the M1 contract task

Roadmap M0 lists the file, and Architecture §4.5 puts `Engine/Config` on the include path of every first-party project.

- **What M0 does:** neither the file nor the include directory exists yet. `Scripts/ModuleRules.json` already lists it in `Banned.AbiMacroFiles`, the one file allowed to define `ENTT_*` macros.
- **Why:** its only content is `#define ENTT_ASSERT(condition, msg) ENGINE_CORE_ASSERT(condition, "{}", msg)`. `ENGINE_CORE_ASSERT` lives in `Core/Assert.h`, which the M1 contract task freezes, and no M0 code includes EnTT. Landing it in M0 would mean either a header that does not compile or an assert surface written ahead of its contract owner.
- **M1 contract task:** adds the file, the `RepositoryRoot .. "/Engine/Config"` include directory in `ApplyFirstPartySettings()`, and its `ModuleRules.json` entry, together with `Assert.h`.

### 3. The regex naming fallback lives in `Scripts/Lint.py`

Appendix A names `Scripts/Lib/naming_lint.py`.

- **What M0 does:** the fallback is the `RegexNamingChecker` in `Lint.py`, next to the clang-tidy runner.
- **Why:** it reads the same `.clang-tidy` options as clang-tidy, and `Lint.py` is a standalone checker. A second module would only split one rule set across two files.

### 4. Shader rule on `Engine` for ninja

Architecture §2.2 makes `Engine` depend on the `Shaders` utility project.

- **What M0 does:** for the ninja generator, `Engine/premake5.lua` attaches the shader build rule to `Engine` itself, so every Engine object depends on the shader stamp. The other generators keep the `Shaders` project and `dependson { "Shaders" }`.
- **Why:** premake 5.0.0's ninja generator supports neither Utility projects nor `dependson` edges to them.

### 5. Python syntax checks use `ast.parse`, not `compileall`

Architecture §2.3 and Appendix A say `Lint.py` runs `compileall`.

- **What M0 does:** the python step parses every file with `ast.parse`, and also checks the syntax against the Python 3.10 grammar (f-strings included).
- **Why:** `compileall` writes `.pyc` files into the tree, and it only checks the grammar of the interpreter that happens to run it. The scripts must run on Python 3.10.

### 6. Header self-containment runs in the lint stage

Architecture §15.8 lists header self-containment under the portability stage.

- **What M0 does:** it is the `headers` step of `Lint.py`, in the lint stage on every host.
- **Why:** it needs the compilation database, which `Lint.py` already generates. It also runs on the Linux and macOS CI jobs that way, against libstdc++ and libc++.

### 7. Module-rule details beyond the Architecture §3 table

`Scripts/ModuleRules.json` allows three things the table does not list explicitly, each implied by another section:

- `App` .cpp files may include the Vulkan headers, for the frame-boundary `vk::SystemError` catch in `App/FrameLoop.cpp` (§4.6).
- `ImGui` .cpp files may include GLFW, for the GLFW glue `ImGui/ImGuiGlfwImplementation.cpp` (§2.2).
- `EditorCore/Scripting/ScriptTypeChecker.cpp` may include the Luau Ast and Common headers next to LuauAnalysis. The Analysis API is written in those types.

### 8. Static checks wider than §2.2 describes

Architecture §2.2 says `CheckBuildConfig.py` generates the vs2026 projects and compares the families `JPH_*`, `SPDLOG_*`, `MA_NO_*`, `VULKAN_HPP_*`, `VK_USE_PLATFORM_*`, `NOMINMAX` and `NDEBUG`. M0 checks more, because each addition closes an ABI or determinism hole a review found.

- **Targets:** every generator and toolset the build or CI uses: vs2026 with MSVC and with clang-cl, gmake with GCC and with Clang, and xcode4.
- **Effective defines:** computed in command-line order, so undefines and `-D`/`/D` in compiler options count.
- **More families:** Luau (`LUA_USE_LONGJMP`, `LUA_API`, `LUACODE_API`, `LUA_VECTOR_*`), Dear ImGui and ImGuizmo (`IMGUI_*`, `ImTextureID`, `ImDrawIdx`, `USE_IMGUI_API`, `IMGUIZMO_NAMESPACE`), GLFW (`_GLFW_*`, `GLFW_DLL`), miniaudio's `MINIAUDIO_IMPLEMENTATION`, and NVRHI's `VK_ENABLE_BETA_EXTENSIONS` and `NVRHI_SHARED_LIBRARY_*`, each from its `VENDOR.md`.
- **Instruction set:** Jolt consumers must use JoltPhysics' instruction-set options, because Jolt derives `JPH_USE_AVX2` and the like from the compiler's `__AVX2__` macros, and those are not part of `JPH_VERSION_ID`.
- **Floating point:** every component of `-ffast-math` counts as non-precise, and so does MSVC's `/Qfast_transcendentals`.

`Lint.py` adds three checks:

- `banned-abi-macro`: no `#define` or `#undef` of these macros in first-party source.
- The product name is also banned in Python and Lua code.
- `json::at`/`get<>` receivers are resolved by type with clang-query when it is installed.

### 9. The commit-gate skill is named `commit-review`

Architecture §13.10 and §15.9 and Roadmap M0 name it `code-review`.

- **Why:** Claude Code ships a generic `code-review` skill. With the same name, which one runs is not guaranteed, so the recorded review against `Docs/ReviewChecklist.md` could silently become the generic one, without the checklist walk or the `Reviewed:` trailer.

### 10. `Tests/Source/ThirdParty/` for tests of vendored-library configuration

The Architecture §2.1 layout has no such directory.

- **What M0 does:** `Tests/Source/ThirdParty/JoltPhysicsTests.cpp` holds the Roadmap M0 acceptance test `"Jolt: version ID includes JPH_CROSS_PLATFORM_DETERMINISTIC"`. It tests how the vendored library is configured, not an engine unit.
- **Later:** when M11 creates the Physics module, the test may move to `Tests/Source/Engine/Physics/`.

### 11. Python line length: 120 columns

CodeStyle §15 says "Follow PEP 8", whose limit is 79 columns (or up to 99 by team agreement).

- **What M0 does:** the scripts use 120 columns, the width the repository's C++ comments and premake scripts are wrapped at.
- **Why:** long argparse help texts, messages and paths stay readable on one line.

## Requested amendments (docs owner)

- **Architecture §2.2:**
  - the Clang flag spelling of decision 1;
  - the CheckBuildConfig targets, families, instruction-set rule and floating-point components of decision 8;
  - the ninja exception of decision 4;
  - "Dist = Full + LTO" is implemented for xcode4 through `LLVM_LTO = YES`, because premake ignores `linktimeoptimization` there.
- **Architecture §2.3:** `ast.parse` instead of `compileall` (decision 5), and Lint's `--mode` (clang tools or regex fallbacks).
- **Architecture §3:** the module-rule details of decision 7, and the Core rule "spdlog only in `Log.h` among public headers", which `ModuleRules.json` now enforces.
- **Architecture §13.10, §15.9 and Roadmap M0:** the skill name `commit-review` (decision 9).
- **Architecture §15.8:**
  - header self-containment in the lint stage (decision 6);
  - the portability checks of precompiled-header paths and xcode4 Dist LTO, `--require-clang-cl`, and warnings tolerated only in vendored code in the clang-cl build;
  - every CI job calls `CI.py`.
- **Architecture §15.9:** PreCommit runs generate and the full lint-stage list, `CheckBuildConfig.py` included.
- **Architecture §2.1:** `Tests/Source/ThirdParty/` (decision 10).
- **Architecture Appendix A:** the naming fallback in `Lint.py` (decision 3), the Clang floating-point spelling (decision 1), and Python at 120 columns.
- **Roadmap M0:** `Engine/Config/entt/ext/config.h` moves to M1's contract task (decision 2).
- **CodeStyle §15:** "PEP 8 with a 120-column limit" (decision 11).
