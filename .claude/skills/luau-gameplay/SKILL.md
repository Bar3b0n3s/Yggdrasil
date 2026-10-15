---
name: luau-gameplay
description: Write or debug gameplay behaviours, shared modules and test suites for this engine's Luau runtime, including fields, lifecycle callbacks, yielding tasks and deterministic input.
---

# Luau gameplay

The API is served by `docs.get` (a connected editor answers it): `reference/ScriptAPI` describes every module, type, member and callback, and `scripting/Engine.d.luau` is the declarations file the type checker uses. Both are long, so ask for one section (`{topic: "reference/ScriptAPI", section: "Input"}`, a type such as `{topic: "scripting/Engine.d.luau", section: "Entity"}`, where the Script component's type is `Script (type)` and the `Script` global is `Script`) or page with `offset` and `length`. The script templates are `templates/Scripts/Behaviour`, `templates/Scripts/Module` and `templates/Scripts/Test`; they name their type `Script_<File>`, which you rename. In an automation-only game-building task, create and edit scripts through `script.create` and `script.write`; their results include compile/type diagnostics. Use `script.check` before testing. (Engine developers find the semantics behind the API in Architecture §5.7 and §11 of the repository.)

## Script shape

A Behaviour starts with a local table, adds fields and methods to that table, then returns `Script.Define("Name", class)`. Its exported instance alias is `ScriptInstance & { ... }`, used to annotate `self`. Do not add members to a sealed object returned from a constructor. A Module returns ordinary library data. A TestSuite returns `Test.Suite`; only a Behaviour may be attached to an entity.

Declare editable data in `.Fields` with `Field.Number`, `Integer`, `Bool`, `String`, `Vector`, `Color`, `Quat`, `Entity`, `Asset`, `Enum` or `Array`. Field defaults become per-instance values. Keep transient state on `self`, initialized in `OnCreate` or `OnStart`; shared module locals are shared within that scene's VM. Unknown or mismatching persisted overrides are diagnosed and preserved, while runtime uses defaults.

Module top levels run during import in a restricted load-time VM. They may construct pure tables, schemas and functions, and `require` other modules inside `Assets/`; they may not query input, create entities or otherwise use runtime engine services. Put such work in callbacks. `require` paths are relative to the script, never paths outside `Assets/`.

For typed access to another behaviour, require its module for the exported alias, then cast the instance. To call a behaviour's own methods with `self:Method()` in strict mode, list them in its exported alias with `self` as their first parameter; the class table defines them:

```luau path=Assets/Scripts/Goal.luau
--!strict
local BallScript = require("./Ball")

export type Goal = ScriptInstance & {
	Hits: number,
	Celebrate: (self: Goal, ball: BallScript.Ball) -> (),
}

local Goal = {}

Goal.Fields = {
	Hits = Field.Integer(0, { Min = 0 }),
}

function Goal.Celebrate(self: Goal, ball: BallScript.Ball)
	self.Hits += 1
	ball.ReachedGoal = true
end

function Goal.OnTriggerEnter(self: Goal, other: Entity)
	local ball = other:GetScript() :: BallScript.Ball?
	if ball then
		self:Celebrate(ball)
	end
end

return Script.Define("Goal", Goal)
```

Examples in this file and every other document are type-checked by the unit suite ("DocExamples: every Luau example in the docs type-checks"); `./Ball` resolves to a stub of the rolling-ball behaviour there.

`GetScript()` returns the instance; `GetComponent("Script")` returns the component proxy. There is no `.Script` shortcut. `entity.Transform` is never nil (every entity has one); every other component shortcut can be nil: narrow or cast after checking the component exists.

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
- **Constant-speed ball** (Breakout, Pong). Give the ball a `Dynamic` `RigidBody` with `GravityFactor` 0, `Restitution` 1, `Friction` 0, `LinearDamping` 0, `AngularDamping` 0, `AllowSleeping` false and `MotionQuality` `LinearCast` (continuous collision, so a fast ball cannot pass through a thin paddle or brick between ticks), and keep it in the play plane with `LockTranslation` `[false, false, true]` and every `LockRotation` axis locked. Contacts combine friction as the geometric mean of the two bodies' values and restitution as the larger, so these values on the ball hold against every surface. Bounces still gain or lose a little speed and the angle drifts, so renormalize the velocity every tick, keeping a minimum vertical share so the ball never travels flat between the side walls (example below).
- **Player-driven paddles and platforms.** Make them `Kinematic` and move them with `RigidBody:MoveKinematic` in `OnFixedUpdate`. A kinematic body pushes dynamic bodies but is never stopped by `Static` ones, so clamp its target in the script (a paddle limit), or it slides through the walls. A `Dynamic` paddle would be knocked away by the ball.
- **Per-entity collisions.** `OnCollisionEnter` names the entity that owns the other body; colliders under an ancestor with a `RigidBody` belong to that ancestor's compound body and report the ancestor. An entity that must report its own hits (a brick) needs its own body (`game-building`, "Scenes, assets and prefabs"). Break it with `SetActive(false)` and bring it back with `SetActive(true)` on a restart rather than destroying and recreating it.
- **Variants from one prefab.** Instances built with `prefab.instantiate {components}` differ only in their overrides (a material, a script field such as a brick's `Points`); a behaviour reads its own fields from `self` and another's through `GetScript()` with its exported alias.

```luau path=Assets/Scripts/Breakout.luau
--!strict
export type Breakout = ScriptInstance & {
	Ball: Entity?,
	Paddle: Entity?,
	BallSpeed: number,
	PaddleSpeed: number,
	PaddleLimit: number,
	PaddleX: number,
}

local Breakout = {}

Breakout.Fields = {
	Ball = Field.Entity(),
	Paddle = Field.Entity(),
	BallSpeed = Field.Number(9, { Min = 1 }),
	PaddleSpeed = Field.Number(14, { Min = 1 }),
	PaddleLimit = Field.Number(7.3, { Min = 0 }),
}

-- The smallest vertical share of the ball's direction, so it never ends up travelling flat between the walls.
local MinVertical = 0.25

function Breakout.OnStart(self: Breakout)
	self.PaddleX = 0
end

function Breakout.OnFixedUpdate(self: Breakout, dt: number)
	-- The kinematic paddle is not stopped by the static walls: clamp its target before moving it.
	self.PaddleX = math.clamp(self.PaddleX + Input.GetAxis("MoveX") * self.PaddleSpeed * dt, -self.PaddleLimit, self.PaddleLimit)
	local paddle = self.Paddle
	local paddleBody = if paddle then paddle.RigidBody else nil
	if paddle and paddleBody then
		paddleBody:MoveKinematic(vector.create(self.PaddleX, paddle.Transform.WorldPosition.y, 0), Quat.Identity())
	end

	-- Restitution 1 keeps the speed only approximately: restore it every tick, keeping the direction.
	local ball = self.Ball
	local body = if ball then ball.RigidBody else nil
	if body then
		local velocity = body:GetLinearVelocity()
		local speed = vector.magnitude(velocity)
		if speed > 0 then
			local direction = velocity / speed
			if math.abs(direction.y) < MinVertical then
				local x = (if direction.x < 0 then -1 else 1) * math.sqrt(1 - MinVertical * MinVertical)
				direction = vector.create(x, if direction.y < 0 then -MinVertical else MinVertical, 0)
			end
			body:SetLinearVelocity(direction * self.BallSpeed)
		end
	end
end

return Script.Define("Breakout", Breakout)
```

## Determinism pitfalls

- Write non-trivial powers with `math.pow(x, y)`, which uses the engine's deterministic math. The `^` operator is exact only for the exponents 2, 3 and 0.5; any other exponent calls the platform's C runtime and may differ between machines. `math.sin` and the other `math` functions and `vector.angle` are already deterministic.
- `pairs`/`next` over a table keyed by entities, components, asset references, tables or functions visits keys in heap-address order, which differs between runs. Key sets by `entity.ID` and sort, or keep an array; iterate with `ipairs`. One entity is the same value as a key while any reference to it is alive, so `inside[other] = true` in `OnTriggerEnter` and `inside[other] = nil` in `OnTriggerExit` work, but never base gameplay on the visiting order.
- Weak tables (`__mode`) lose entries when the garbage collector runs, which is not tied to ticks; never let their contents decide gameplay. `collectgarbage` and `gcinfo` do not exist.
- `Task.Wait(seconds)` and `Task.Delay` wait `ceil(seconds / fixedDelta)` ticks, with decimal multiples of the tick counted exactly (0.14 s at 50 Hz is 7 ticks). Prefer `Task.WaitTicks` when you mean ticks.

## Limits

Log entries and `print` keep at most 8 KiB each (longer text is truncated and marked) and one VM writes at most 1024 entries per frame (the rest are counted and dropped); evaluation output still captures every line. `Debug.DrawText` text is at most 1024 bytes, entity names 256 bytes, and an entity carries at most 256 tags of at most 256 bytes each; one component field value a script writes (for example `Text.Text`, or a value passed to `AddComponent`) holds at most 4096 bytes of text; `Audio.PlayOneShot` starts at most 1024 one-shots per frame. Larger values raise a located error before any change. Test scripts have their own limits: suite and case names at most 256 bytes, `Test.ExpectScriptError` patterns at most 1 KiB, at most 4096 cases per suite, 1024 `Test.Inject*` events per input tick and 256 `Test.Screenshot` calls per suite; a row keeps 32 failures (the last counts the rest) and a suite 1024, and a case keeps at most 256 unclaimed script errors. `script.errors` keeps 256 distinct errors with 8 KiB messages and 32 traceback frames, and reports `evicted` when older ones were dropped. User-defined type functions (`type function`) are not supported: the type checker refuses each declaration with a `SCRIPT_TYPE_ERROR` (it would run their bodies without a bound), so write ordinary type aliases instead.

## Testing and diagnosis

Create test scripts with `Test.Suite` and register `Test.Case` inside the suite body; the body registers cases, while the cases perform runtime work (`templates/Scripts/Test` is the starting point). Use `Test.Inject*` and `Test.WaitTicks` for deterministic input. A button injected `Down` stays down until an `Up`, and an analog value (`Test.InjectAction("MoveX", nil, 0.5)`) holds on every later tick until another value replaces it: inject 0 to let go. `Test.Expect` records a failure and continues; `Test.Fail` ends the case. Pass the actual value first to `Test.ExpectEqual(actual, expected, message?)` and `Test.ExpectNear(actual, expected, epsilon?, message?)`: a failure row shows the message, then `(got <actual>, expected <expected>)`, with `, epsilon <e>` for `ExpectNear`. Configure suites in `Testing.Suites` with their scene, parameters and supported modes. Run with `test.run` or `Editor --headless --project <project> --run-tests`; `--filter` selects a suite/case.

Use `script.errors`, diagnostics and the structured test results to find authored file/line, traceback, callback, entity and tick. A failing behaviour is disabled; in the editor `PauseOnError` may pause the session. Fix the source and confirm the fresh run rather than assuming an old disabled instance restarted. Ordinary editor play supports transactional hot reload; deterministic lockstep/replay/recording/test sessions defer it except explicit test reload.

Start input recording at tick zero. Record through `input.record` or `test.run`'s record option, then verify with `input.replay` and strict hash checking. External `script.eval`/`play.waitFor` writes or session-RNG use invalidate recording; observation alone does not. A test-driven recording is published only after an ordinary fresh replay reproduces every state-hash checkpoint and its final hash, and, with input events, is refused when a run without them reproduces it too (game-building §7).
