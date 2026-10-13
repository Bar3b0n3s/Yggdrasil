---
name: luau-gameplay
description: Write or debug gameplay behaviours, shared modules and test suites for this engine's Luau runtime, including fields, lifecycle callbacks, yielding tasks and deterministic input.
---

# Luau gameplay

Use `Docs/Reference/ScriptAPI.md` and `Resources/Scripting/Engine.d.luau` for the available API, `Docs/Architecture.md` §5.7 and §11 for semantics, and the shipped `Resources/Templates/Scripts/` templates. In an automation-only game-building task, create and edit scripts through `script.create` and `script.write`; their results include compile/type diagnostics. Use `script.check` before testing.

## Script shape

A Behaviour starts with a local table, adds fields and methods to that table, then returns `Script.Define("Name", class)`. Its exported instance alias is `ScriptInstance & { ... }`, used to annotate `self`. Do not add members to a sealed object returned from a constructor. A Module returns ordinary library data. A TestSuite returns `Test.Suite`; only a Behaviour may be attached to an entity.

Declare editable data in `.Fields` with `Field.Number`, `Integer`, `Bool`, `String`, `Vector`, `Color`, `Quat`, `Entity`, `Asset`, `Enum` or `Array`. Field defaults become per-instance values. Keep transient state on `self`, initialized in `OnCreate` or `OnStart`; shared module locals are shared within that scene's VM. Unknown or mismatching persisted overrides are diagnosed and preserved, while runtime uses defaults.

Module top levels run during import in a restricted load-time VM. They may construct pure tables, schemas and functions, and `require` other modules inside `Assets/`; they may not query input, create entities or otherwise use runtime engine services. Put such work in callbacks. `require` paths are relative to the script, never paths outside `Assets/`.

For typed access to another behaviour, require its module for the exported alias, then cast the instance:

```luau
local BallScript = require("./Ball")
local ball = other:GetScript() :: BallScript.Ball?
if ball then
	ball.ReachedGoal = true
end
```

`GetScript()` returns the instance; `GetComponent("Script")` returns the component proxy. There is no `.Script` shortcut. Other eligible component shortcuts can be nil: narrow or cast after checking the component exists.

## Timing and tasks

Put deterministic gameplay in `OnFixedUpdate`; input edges and `Time.GetDeltaTime()` reflect the current phase. Use simulation ticks/time and seeded `Random`, including a separate `Random.New(seed)` when independent state is intended. `Time.GetRealTime()` is unsuitable for replayed gameplay.

Callbacks run by execution order and canonical hierarchy order. Entity creation is immediate; destruction is deferred. Recheck a retained entity with `IsValid()` when later work could outlive it. A scene load takes effect after the frame's callbacks return, creates a fresh VM, and invalidates old proxies/tasks. Pass data explicitly through `Scene.Load(scene, parameters)` and read it with `Scene.GetLoadParameters()`.

Callback bodies cannot yield. Start a task from the callback with `Task.Spawn(function() ... end)` or `Task.Delay`; only the task or `Test.Case` body may call waits. Use `Task.WaitTicks` for exact stepping. Tasks belong to their instance and are cancelled when it is destroyed or disabled.

For a follow camera, use `OnLateUpdate` and the target's `Transform.RenderPosition`/`RenderRotation`, which reflect interpolation. Move the camera's world pose or use `LookAt`; keep gameplay decisions in the fixed phase. Teleports use `Transform:Teleport` or the body's teleport method so interpolation does not sweep across the jump.

## Idioms

- **State machines.** Keep the state in a field (`self.State = "Falling"`) and dispatch in `OnFixedUpdate` through a table of per-state functions; change state only there, so transitions happen on ticks and replay exactly.
- **Grid logic.** Store a board as a flat array indexed `(row - 1) * width + column`, or as rows of arrays; iterate with numeric `for` loops in a fixed order. Convert to world positions only when placing entities.
- **DAS/ARR input** (delayed auto-shift and auto-repeat, as in falling-block games). On `Input.IsActionPressed` move once and reset a tick counter; while `Input.IsActionDown`, count ticks and move again after the delay ticks, then every repeat-interval ticks. Count ticks, never seconds or wall time.
- **Pooling.** Create a fixed set of entities once (for example in `OnStart`), then reuse them with `SetActive(true/false)` and `Transform:Teleport` instead of creating and destroying every tick. `OnEnable`/`OnDisable` run on each toggle.
- **Typing.** Annotate callbacks with the exported instance alias (`function Ball.OnStart(self: Ball)`), so strict mode checks `self` fields. An optional field (`Goal: Entity?`, `Field.Asset`) can be nil: test it (`if self.Goal then`) or cast after checking before using it.

## Determinism pitfalls

- Write non-trivial powers with `math.pow(x, y)`, which uses the engine's deterministic math. The `^` operator is exact only for the exponents 2, 3 and 0.5; any other exponent calls the platform's C runtime and may differ between machines. `math.sin` and the other `math` functions and `vector.angle` are already deterministic.
- `pairs`/`next` over a table keyed by entities, components, asset references, tables or functions visits keys in heap-address order, which differs between runs. Key sets by `entity.ID` and sort, or keep an array; iterate with `ipairs`. One entity is the same value as a key while any reference to it is alive, so `inside[other] = true` in `OnTriggerEnter` and `inside[other] = nil` in `OnTriggerExit` work, but never base gameplay on the visiting order.
- Weak tables (`__mode`) lose entries when the garbage collector runs, which is not tied to ticks; never let their contents decide gameplay. `collectgarbage` and `gcinfo` do not exist.
- `Task.Wait(seconds)` and `Task.Delay` wait `ceil(seconds / fixedDelta)` ticks, with decimal multiples of the tick counted exactly (0.14 s at 50 Hz is 7 ticks). Prefer `Task.WaitTicks` when you mean ticks.

## Limits

Log entries and `print` keep at most 8 KiB each (longer text is truncated and marked) and one VM writes at most 1024 entries per frame (the rest are counted and dropped); evaluation output still captures every line. `Debug.DrawText` text is at most 1024 bytes, entity names 256 bytes, and an entity carries at most 256 tags of at most 256 bytes each; one component field value a script writes (for example `Text.Text`, or a value passed to `AddComponent`) holds at most 4096 bytes of text; `Audio.PlayOneShot` starts at most 1024 one-shots per frame. Larger values raise a located error before any change. Test scripts have their own limits: suite and case names at most 256 bytes, `Test.ExpectScriptError` patterns at most 1 KiB, at most 4096 cases per suite, 1024 `Test.Inject*` events per input tick and 256 `Test.Screenshot` calls per suite; a row keeps 32 failures (the last counts the rest) and a suite 1024, and a case keeps at most 256 unclaimed script errors. `script.errors` keeps 256 distinct errors with 8 KiB messages and 32 traceback frames, and reports `evicted` when older ones were dropped.

## Testing and diagnosis

Create test scripts with `Test.Suite` and register `Test.Case` inside the suite body; the body registers cases, while the cases perform runtime work. Use `Test.Inject*` and `Test.WaitTicks` for deterministic input. `Test.Expect` records a failure and continues; `Test.Fail` ends the case. Configure suites in `Testing.Suites` with their scene, parameters and supported modes. Run with `test.run` or `Editor --headless --project <project> --run-tests`; `--filter` selects a suite/case.

Use `script.errors`, diagnostics and the structured test results to find authored file/line, traceback, callback, entity and tick. A failing behaviour is disabled; in the editor `PauseOnError` may pause the session. Fix the source and confirm the fresh run rather than assuming an old disabled instance restarted. Ordinary editor play supports transactional hot reload; deterministic lockstep/replay/recording/test sessions defer it except explicit test reload.

Start input recording at tick zero. Record through `input.record` or `test.run`'s record option, then verify with `input.replay` and strict hash checking. External `script.eval`/`play.waitFor` writes or session-RNG use invalidate recording; observation alone does not. A test-driven recording is published only after an ordinary fresh replay reproduces its original final state hash.
