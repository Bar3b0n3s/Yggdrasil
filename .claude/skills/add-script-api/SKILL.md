---
name: add-script-api
description: Add or change an engine Luau binding, component proxy method, script property, callback or signature, keeping validation, generated declarations and tests consistent.
---

# Add a script API

Read `Docs/Architecture.md` §11.3–11.5 and §15.2, `Docs/Decisions/0019-m13-contract.md`, and the existing binding in `Engine/Source/Engine/Scripting/Bindings/` closest to the requested change. `ScriptApiRegistry` is the source for dispatch, descriptions, declarations and coverage; do not hand-edit `Resources/Scripting/Engine.d.luau` or `Docs/Reference/ScriptAPI.md`.

## Binding and policy

Implement the native callback in its domain's binding file. Use `Lua::Check<T>` and the shared JSON/value helpers for finite, typed arguments and located errors. A component write goes through `ComponentAccess` with the host's schema source. Physics and audio calls use the session's systems. Identity values must re-resolve their UUID and scene generation; never retain component references across a structural edit or expose an owning/raw engine pointer to Luau.

Register the member once with its signature and a useful description. Runtime/Test availability and `RunModes` are separate: `OnHotReload` and `Test.ReloadScript` are EditorOnly; a pure constructor may also be available at load time. Functions default to mutating: mark a truly pure function explicitly, and preserve separate setter mutation policy. External evaluation must notify the host before a host write or session RNG consumption, even if the call later fails. Local Color/Quat/local-generator edits may be pure with respect to the session.

Register string-enum argument slots and omitted defaults in the member options, using the authoritative enum table, and type the slot with the enum in the signature (never plain `string`). When overloads give one slot different enums, select each slot by another argument's Lua type (`ScriptEnumParameter::SelectorIndex`/`Selector`, as `Test.InjectGamepad` does) and write the signature as an intersection of the overloads. A binding enum may repeat a host-reflected enum only with identical names and values. Do not maintain a second dispatch/type/coverage enum table. Generated signatures must parse under the vendored New solver: for optional bounds use `(vector?, vector?)`, not an optional tuple.

Text that the host keeps (names, tags, debug text, anything stored in a component or queued for later) goes through `Lua::CheckBoundedString` with a limit from `Scripting/ScriptLimits.h`, so one VM string cannot be copied into unbounded host memory; document the limit in the member description. Component field values are bounded by the native conversion (`MaxScriptComponentStringBytes`), and text that becomes a diagnostic or a test message is read with `Lua::CheckDiagnosticText`, which rewrites private locations and bounds the result like a log entry. Anything a script can make the host retain per tick, per frame or per suite (queued requests, records, files) needs a constant in `ScriptLimits.h` (or the owning module's header when Scripting may not hold it), a located error at the limit and a test that fails with the bound disabled. Identity values (Entity, component proxies, `AssetRef`) are interned by `Lua::Push`; mutable value types (Color, Quat) are not, so give them an `__eq` operator that compares values. Method errors read `Type:Method:`, everything else `Owner.Member:`.

Only the shared protected-call/resume boundary enters Luau. Nested callbacks inherit their caller's watchdog deadline. A wait API must reject a non-yieldable callback at the authored location; Task and Test bodies resume as threads. Stored tasks preserve owner identity and external-execution origin.

If adding a binding file or domain, route `RegisterBindings.cpp` through its current owner and regenerate project files. Public headers expose no Luau types. A frozen contract change is reviewed by its owner and recorded in the milestone ADR.

## Verification and references

Add public-API tests through `Support/ScriptTestFixture` for ordinary use and the actual failure boundary: wrong types, non-finite/range inputs, destroyed identities, access policy, ownership, and mutation timing where relevant. A property needs read and write coverage; an operator or constructor needs its own exercised path. Include the member in the independently maintained API test coverage, backed by a test that really invokes it. From M14, add FeatureTest coverage in each supported run mode, including small string-enum values.

Test type inference through `ScriptTypeChecker` as well as runtime behavior when signatures change. After building the editor, run `python Scripts/GenerateDocs.py`, inspect the generated diff, then `python Scripts/GenerateDocs.py --check`. Add or adjust the relevant `Docs/Architecture.md` API entry through its owner. If the capability also changes editor automation, use `add-automation-method`. Finish through the repository's `build-and-test` and `commit-review` gates.
