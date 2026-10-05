# Luau (vendored)

| Field | Value |
|---|---|
| Upstream | https://github.com/luau-lang/luau |
| Version / tag | `0.741` (latest GitHub release at vendoring time) |
| Commit | `2d88f9b5facaa9e4ec91247b39ae8251bd49a244` ("Sync to upstream/release/741", 2026-10-02) |
| Date vendored | 2026-10-05 |
| License | `MIT` - `LICENSE.txt` (Roblox Corporation, Lua.org/PUC-Rio) and `lua_LICENSE.txt` (Lua 5.x, MIT) |
| Local modifications | None. Line endings normalized to LF (repo policy `* text=auto eol=lf`); content is byte-identical otherwise. |

## What was kept

The upstream directory layout is preserved so that updates are a straight copy.

| Directory | Upstream target | Built into |
|---|---|---|
| `Common/` | `Luau.Common` (header-mostly utilities, fast flags, `Bytecode.h`) | `Luau` |
| `Ast/` | `Luau.Ast` (lexer, parser, AST) | `Luau` |
| `Bytecode/` | `Luau.Bytecode` (`BytecodeBuilder`; public dependency of `Luau.Compiler`) | `Luau` |
| `Compiler/` | `Luau.Compiler` (`luacode.h` C API, `Luau/Compiler.h` C++ API) | `Luau` |
| `Config/` | `Luau.Config` (`.luaurc` / `.config.luau` parsing; public dependency of `Luau.Require`) | `Luau` |
| `VM/` | `Luau.VM` (`lua.h`, `lualib.h`, `luaconf.h`; `VM/src` is private) | `Luau` |
| `Require/` | `Luau.Require` (require-by-string runtime library + `RequireNavigator`) | `Luau` |
| `Analysis/` | `Luau.Analysis` (type checker, linter, autocomplete, `Frontend`) | `LuauAnalysis` |
| `tools/natvis/` | `Common/Ast/VM/Analysis.natvis` debugger visualizers | VS solution only |
| `LICENSE.txt`, `lua_LICENSE.txt` | licenses | - |

## What was removed

`CodeGen/` (native JIT - deliberately not used), `Inliner/` (JIT-only bytecode inliner, links VM internals), `CLI/`,
`tests/`, `fuzz/`, `bench/`, `extern/` (doctest, isocline, ...), `tools/` (except the four natvis files), `CMakeLists.txt`,
`CMakePresets.json`, `Sources.cmake`, `EXTLuau.cmake`, `Makefile`, `README.md`, `CONTRIBUTING.md`, `SECURITY.md`,
`.github/`, `Analysis/include/Luau/ControlFlow.md` (design note), `tools/natvis/CodeGen.natvis`.
Note: upstream no longer has an `EqSat/` directory (the e-graph library was removed from the tree before 0.741),
so there is nothing to vendor for it.

## Premake projects (`premake5.lua`)

| Project | Contents | Depends on |
|---|---|---|
| `Luau` | Common, Ast, Bytecode, Compiler, Config, VM, Require | - |
| `LuauAnalysis` | Analysis | `Luau` (Ast, Config, Compiler and VM are used by Analysis) |

Why Config is in `Luau` and not in `LuauAnalysis`: upstream `Luau.Require` links `Luau.Config` publicly
(`RequireNavigator.h` includes `Luau/Config.h` / `Luau/LuauConfig.h`, and alias resolution parses `.luaurc` /
`.config.luau`), and `Luau.Config` itself only needs Ast + Compiler + VM. Putting Config in the runtime library is the only
layout without a dependency cycle (`LuauAnalysis` already depends on the runtime). Static linking only pulls in the object
files that are actually referenced, so the Runtime executable does not pay for unused parts.

Build settings mirrored from upstream `CMakeLists.txt` / `Makefile`:

- Dialect C++17 for every module (upstream `Luau.Common` has `PUBLIC cxx_std_17`, which propagates to `Luau.VM`).
- C++ exceptions ON (`exceptionhandling "On"` -> `/EHsc`). Required: see error handling below. RTTI is not required by
  Luau (no `dynamic_cast`/`typeid`); it is left at the compiler default.
- MSVC: `_CRT_SECURE_NO_WARNINGS`, `/MP`, and `/d2ssa-pre-` on `VM/src/lvmexecute.cpp` only (upstream disables partial
  redundancy elimination for the interpreter loop on MSVC >= 19.24 because it badly regresses interpreter codegen).
- Linux/macOS: `-fno-math-errno` (upstream CMake applies it to `Luau.VM`, the Makefile release config to everything).
  Linux: `pic "On"`.
- `NDEBUG` in Release and Dist (upstream optimized builds define it), which compiles out `LUAU_ASSERT`. Debug keeps
  Luau's internal assertions on.
- Not mirrored on purpose: `/we4018 /we4388` and `LUAU_WERROR` (warnings-as-errors is not used for vendor code),
  `-Wall` (warnings stay at default level for vendor code).
- No `/bigobj` is needed (upstream does not use it; verified with MSVC 14.51).

## Consumer usage

### Include directories (relative to `Vendor/Luau`)

Runtime (Engine, Runtime, Editor, Tests):

```
Common/include  Ast/include  Bytecode/include  Compiler/include  Config/include  VM/include  Require/include
```

Additionally for static analysis (Editor / tools only): `Analysis/include`.
Never add `VM/src` (VM internals) to a consumer's include path.

Public headers:

| Header | Purpose |
|---|---|
| `lua.h`, `lualib.h` (`luaconf.h`) | VM C API: `luaL_newstate`, `luaL_openlibs`, `luaL_sandbox`, `luaL_sandboxthread`, `luau_load`, `lua_pcall`, ... |
| `luacode.h` | `luau_compile` (returns a `malloc`ed buffer - release it with `free`) |
| `Luau/Compiler.h` | C++ compiler API (`Luau::compile`, `Luau::CompileOptions`) |
| `Luau/Require.h` | require-by-string: `luaopen_require` + `luarequire_Configuration` callbacks |
| `Luau/Common.h` | `LUAU_ASSERT`, `Luau::assertHandler()`, fast flags (`Luau::FValue`) |
| `Luau/ExperimentalFlags.h` | `Luau::isAnalysisFlagExperimental` (see fast flags below) |
| `Luau/Frontend.h`, `Luau/BuiltinDefinitions.h`, `Luau/Error.h`, `Luau/FileResolver.h`, `Luau/ConfigResolver.h`, `Luau/ToString.h` | type checking (`LuauAnalysis`) |

### Defines

None are required. Do **not** define any of the following unless the change is made in `premake5.lua` for the libraries
and every consumer at the same time (they change ABI or error semantics):
`LUA_USE_LONGJMP`, `LUA_API`, `LUACODE_API`, `LUA_VECTOR_SIZE` (default 3), `LUA_VECTOR_DOUBLE`.
Optional and off: `LUAU_ENABLE_ASSERT` (keep `LUAU_ASSERT` in NDEBUG builds), `LUAU_ENABLE_TIME_TRACE`.

`NDEBUG` note: the libraries are built with `NDEBUG` in Release/Dist. A consumer that does not define `NDEBUG` in its
Release config links and works fine (verified), but inline functions in Luau's C++ headers (`DenseHash`, `Variant`, ...)
then get `LUAU_ASSERT` in the consumer's translation units only. The C API headers are unaffected. No struct layout depends
on `NDEBUG`.

### Linking

Link both libraries, `LuauAnalysis` **before** `Luau` (GNU ld resolves archives left to right):

```lua
links { "LuauAnalysis", "Luau" }   -- Editor / tools / Tests
links { "Luau" }                   -- Runtime (no analysis)
```

| Platform | Extra link requirements |
|---|---|
| Windows | none (only `kernel32`, linked by default; `NativeStackGuard` uses `GetCurrentThreadStackLimits`). Recommended for Debug executables that run `LuauAnalysis` or deep scripts: `linkoptions { "/STACK:2097152" }` (upstream sets this for its Debug CLI/test executables because MSVC Debug frames are large). Optional debugging aid: `linkoptions { "/NATVIS:<Vendor/Luau>/tools/natvis/VM.natvis" }` (likewise `Ast`, `Common`, `Analysis`) - upstream adds these as interface link options. |
| Linux | `pthread` (upstream links `-lpthread` into its executables; part of libc on glibc >= 2.34, harmless to list) |
| macOS | none (pthread / mach are part of libSystem) |

Exceptions must be enabled in every consumer (the premake default; never use `exceptionhandling "Off"` / `-fno-exceptions`).

### Error handling (important for the engine's error policy)

Luau is built with its default `LUA_USE_LONGJMP=0`, i.e. **C++ exception mode**:

- `luaD_throw` throws an internal `lua_exception` (derived from `std::exception`; `what()` returns the error message).
  `lua_pcall`, `luau_load`, `lua_resume` and Lua-side `pcall`/`xpcall` catch it and return a status code
  (`LUA_ERRRUN`, `LUA_ERRMEM`, `LUA_ERRERR`, `LUA_ERRSYNTAX`) with the error value on the stack.
- Because errors are exceptions, **C++ destructors run** when a Lua error unwinds through a C++ binding
  (`luaL_error`, `luaL_argerror`, `luaL_checkinteger`, `lua_error`, ...). RAII in bindings is safe. (With longjmp
  they would be skipped - one reason not to switch.)
- A `std::exception` thrown by a C++ binding is caught by the nearest protected call and converted into a Lua runtime
  error whose message is `e.what()` (verified by the smoke test, also with `LuauFastpcall` enabled).
- Exceptions **not** derived from `std::exception` are not caught by Luau and leave the VM state inconsistent:
  bindings must only throw `std::exception`-derived types (or nothing).
- Never swallow Lua errors: a `catch (const std::exception&)` or `catch (...)` around `lua_*` calls inside a binding also
  catches Lua errors. Rethrow (`throw;`) if you need cleanup there.
- A Lua error raised outside of any protected call (calling `lua_call`/`lua_gettable` etc. from engine code without
  `lua_pcall`) propagates to the C++ caller as a `std::exception`. Engine code should enter Lua only through `lua_pcall`
  (or `lua_resume`). In longjmp mode this would instead call `lua_callbacks(L)->panic` and `abort()`.
- `luau_compile` never throws: on a syntax error it returns bytecode that encodes the error, and `luau_load` returns
  non-zero with the message (`"chunk:line: message"`) on the stack. The C++ API `Luau::compile` behaves the same way.
- `LuauAnalysis` uses exceptions as well: `Frontend::check` may throw `Luau::InternalCompilerError` (and its subclasses
  `TimeLimitError` / `UserCancelError`, see `Luau/TypeCheckLimits.h`). Wrap analysis calls in
  `try`/`catch (const Luau::InternalCompilerError&)`. The parser/compiler use `ParseErrors` / `CompileError` internally;
  only `Luau::compileOrThrow` lets them escape (`luau_compile` and `Luau::compile` do not).

### Fast flags

Luau gates new behavior behind fast flags (`Luau::FValue<bool>`), all `false` by default in source. Upstream tools
(`luau`, `luau-analyze`) enable every non-experimental `Luau*` flag at startup; the engine should do the same once, before
creating any `lua_State` or `Luau::Frontend`, to get the behavior of the released version:

```cpp
for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
	if (strncmp(flag->name, "Luau", 4) == 0 && !Luau::isAnalysisFlagExperimental(flag->name))
		flag->value = true;
```

For type checking, construct the frontend with `Luau::SolverMode::New` (the upstream CLI default), then
`Luau::registerBuiltinGlobals(frontend, frontend.globals)` and `Luau::freeze(frontend.globals.globalTypes)`.
`Luau::assertHandler()` can route `LUAU_ASSERT` failures (Debug builds) to the engine log.

### Threading

A `lua_State` (and all threads created from it) must only be used by one OS thread at a time; independent states may run
in parallel. `Luau::Frontend` is not thread-safe except through its own `checkQueuedModules` executor callback.

## Verification (2026-10-05)

Throwaway premake workspace (outside the repo) including this `premake5.lua` plus a C++23 smoke executable, generated with
premake 5.0.0 (`vs2026`) and built with MSVC 14.51 (v145) x64 in Debug, Release and Dist - no warnings except the
expected MSB8029 (Temp directory). The smoke test passed in all three configurations:

- `luau_compile` + `luau_load` into `luaL_newstate` + `luaL_openlibs` + `luaL_sandbox`, `lua_pcall`, returned values read back
  (`fib(20) == 6765`, `vector` library);
- `require('./util')` through `luaopen_require` with an in-memory `luarequire_Configuration`;
- `std::runtime_error` and `luaL_error` thrown from C++ bindings caught by Lua `pcall`, with C++ destructors run;
- syntax error reported by `luau_load`, runtime error via `lua_pcall` (`LUA_ERRRUN`), sandbox read-only violation;
- `Luau::Frontend` (new solver, strict mode, custom `FileResolver` + `NullConfigResolver`) reports exactly one
  `TypeMismatch` (`Expected this to be 'number', but got 'string'`) for `local count: number = "not a number"`.

`premake5 --os=linux gmake`, `--os=linux ninja` and `--os=macosx xcode4` were also generated to check the Linux/macOS
filters (`-fno-math-errno`, `-fPIC`, no MSVC-only flags); those platforms could not be compiled on this machine.

## Updating

1. Find the latest release: `git ls-remote --tags https://github.com/luau-lang/luau` (tags are `0.NNN`; ignore the old
   `696` tag) or https://github.com/luau-lang/luau/releases/latest.
2. `git clone --depth 1 --branch <tag> https://github.com/luau-lang/luau <tmp>`.
3. Replace `Common/ Ast/ Bytecode/ Compiler/ Config/ VM/ Require/ Analysis/`, the two license files and
   `tools/natvis/{Common,Ast,VM,Analysis}.natvis` with the new copies (delete the old directories first so removed files
   disappear; drop `Analysis/include/Luau/ControlFlow.md`). Normalize line endings to LF.
4. Diff upstream `Sources.cmake` against the explicit `.cpp` lists in `premake5.lua` (every `.cpp` under the copied
   `*/src` directories must be listed; headers are picked up by globs). Check `CMakeLists.txt` for new modules,
   new inter-module dependencies (e.g. if Compiler/VM/Require gain a dependency on another directory), new compile
   definitions/options, and `Makefile` for per-module include paths.
5. Regenerate, rebuild all configurations, run the Engine scripting tests, and update the version, commit and date above.
