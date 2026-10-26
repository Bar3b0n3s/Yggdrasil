---
name: luau-gameplay
description: Write or debug gameplay behaviours, shared modules and test suites for this engine's Luau runtime, including fields, lifecycle callbacks, yielding tasks and deterministic input.
---

# Luau gameplay

The API is served by `docs.get` (a connected editor answers it): `reference/ScriptAPI` describes every module, type, member and callback, and `scripting/Engine.d.luau` is the declarations file the type checker uses. Both are long, so ask for one section (`{topic: "reference/ScriptAPI", section: "Input"}`, a type such as `{topic: "scripting/Engine.d.luau", section: "Entity"}`, where the Script component's type is `Script (type)` and the `Script` global is `Script`) or page with `offset` and `length`. The script templates are `templates/Scripts/Behaviour`, `templates/Scripts/Module` and `templates/Scripts/Test`: the Behaviour and Module templates name their table `Script_<File>` (a Behaviour also its exported alias and `Script.Define` name), which you rename; the Test template has no type and names its suite after the file stem without `.test` (`Assets/Tests/Board.test.luau` gives `Test.Suite("Board", ...)`). In an automation-only game-building task, create and edit scripts through `script.create` and `script.write`; their results include compile, type and lint diagnostics. Iterate until `script.check` returns an empty `diagnostics` array before testing: its `passed` counts only errors, but the games suite fails a game on lint warnings too. (Engine developers find the semantics behind the API in Architecture §5.7 and §11 of the repository.)

## Script shape

A Behaviour starts with a local table, adds fields and methods to that table, then returns `Script.Define("Name", class)`. Its exported instance alias is `ScriptInstance & { ... }`, used to annotate `self`. Do not add members to a sealed object returned from a constructor. A Module returns ordinary library data. A TestSuite returns `Test.Suite`; only a Behaviour may be attached to an entity.

Declare editable data in `.Fields` with `Field.Number`, `Integer`, `Bool`, `String`, `Vector`, `Color`, `Quat`, `Entity`, `Asset`, `Enum` or `Array`. `Field.Asset` takes an `AssetType` name (`Prefab`, `Material`, `Scene`, `AudioClip` for imported audio and `.sfx` sound effects, ...); any other name fails the import. Field defaults become per-instance values. Keep transient state on `self`, initialized in `OnCreate` or `OnStart`; shared module locals are shared within that scene's VM. Unknown or mismatching persisted overrides are diagnosed and preserved, while runtime uses defaults.

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

Examples in this file and every other document are type-checked by the unit suite ("DocExamples: every Luau example in the docs type-checks") and must raise no lint warning ("... is free of lint warnings"); `./Ball` resolves to a stub of the rolling-ball behaviour there.

`GetScript()` returns the instance (of the entity's first script slot, below); `GetComponent("Script")` returns the component proxy. There is no `.Script` shortcut. `entity.Transform` is never nil (every entity has one); every other component shortcut can be nil: narrow or cast after checking the component exists.

**Several scripts on one entity.** The Script component is an ordered list of script slots, `Scripts`, each `{Script, Fields, ExecutionOrder}`, so an entity can carry one small behaviour per concern (movement, health, scoring) instead of one large script. Every slot that names a script is its own instance, with its own fields, `self` state, tasks and faults; the same script may fill two slots. Callbacks run in (`ExecutionOrder`, canonical entity order, slot) order. The callbacks addressed to the entity reach each slot's instance in slot order (physics callbacks, `OnEnable`, `OnDisable`), and `OnDestroy` runs in reverse slot order. A fault disables only the instance that raised it, so the entity's other behaviours keep running.
- `GetScript()` returns the first slot's instance, `GetScript("Health")` the instance of the first slot, in slot order, whose behaviour is `Script.Define("Health", ...)`, and `GetScripts()` the live instances of every slot in slot order; each returns nil, or leaves out, a slot without a live instance. Cast as above: `other:GetScript("Ball") :: BallScript.Ball?`.
- From code, attach with `entity:AddComponent("Script", { Scripts = { { Script = mover, Fields = {}, ExecutionOrder = 0 }, { Script = health, Fields = { Points = 3 }, ExecutionOrder = 10 } } })`, every slot table giving all three members. A write of `GetComponent("Script").Scripts` replaces the list: an instance whose slot still holds the same script keeps running, and every other slot's instance is destroyed or created, as removing and adding the component would.
- Find a sibling behaviour by name, `self.Entity:GetScript("<Name>")`, never by its position in `GetScripts()`, which changes when a slot is added, removed or reordered.

## Timing and tasks

Put deterministic gameplay in `OnFixedUpdate`; input edges and `Time.GetDeltaTime()` reflect the current phase. Use simulation ticks/time and seeded `Random`, including a separate `Random.New(seed)` when independent state is intended. `Time.GetRealTime()` is unsuitable for replayed gameplay.

**Randomness.** `Random.*` and `math.random` draw from the session stream, which the session seed (the project's `Simulation.Seed` XOR the scene's `Seed`, or `play.start`'s `seed`) starts and the state hash covers. `Random.New(seed)` returns an independent `RandomGenerator`:
- Create it in a callback (`OnStart`) or a function a callback calls: `Random` is unavailable at a module's top level, which fails the import.
- Seed it from the session stream, `Random.New(Random.Integer(0, 2147483647))`, so the session seed chooses its sequence; a constant seed repeats the same sequence in every session.
- Its own state is a script value outside the state hash, so whatever it decides must show in components (a piece's cells) for replays to see it.
- `Random.Shuffle` and `RandomGenerator:Shuffle` refuse a frozen table: shuffle a `table.clone` of it (`Choice` reads frozen tables).

Callbacks run by execution order and canonical hierarchy order. Entity creation is immediate; destruction is deferred. Recheck a retained entity with `IsValid()` when later work could outlive it. An entity destroyed earlier in the tick stays readable until the flush (its fields, `Name`, tags, components and place in the hierarchy), so a collision partner can still read a brick another callback just destroyed; `IsValid()` is already false, `Scene.Find*` skip it, and every write to it raises `entity <id> was destroyed`. A scene load takes effect after the frame's callbacks return, creates a fresh VM, and invalidates old proxies/tasks. Pass data explicitly through `Scene.Load(scene, parameters)` and read it with `Scene.GetLoadParameters()`. `Scene.Reload(parameters?)` restarts the current scene from the document it started from (in editor play the scene as it was at Play, unsaved edits included), keeping the load parameters unless given new ones; `Scene.GetCurrent()` returns the scene asset it came from (nil for a scene never saved). The first `Scene.Load` or `Scene.Reload` of a frame wins; a second raises InvalidState.

Callback bodies cannot yield. Start a task from the callback with `Task.Spawn(function() ... end)` or `Task.Delay`; only the task or `Test.Case` body may call waits. Use `Task.WaitTicks` for exact stepping. Tasks belong to their instance and are cancelled when it is destroyed or disabled.

For a follow camera, use `OnLateUpdate` and the target's `Transform.RenderPosition`/`RenderRotation`, which reflect interpolation. Move the camera's world pose or use `LookAt`; keep gameplay decisions in the fixed phase. Teleports use `Transform:Teleport` or the body's teleport method so interpolation does not sweep across the jump.

## Idioms

- **State machines.** Keep the state in a field (`self.State = "Falling"`) and dispatch in `OnFixedUpdate` through a table of per-state functions; change state only there, so transitions happen on ticks and replay exactly.
- **Grid logic.** Store a board as a flat array indexed `(row - 1) * width + column`, or as rows of arrays; iterate with numeric `for` loops in a fixed order. Convert to world positions only when placing entities.
- **DAS/ARR input** (delayed auto-shift and auto-repeat, as in falling-block games). On `Input.IsActionPressed` move once and reset a tick counter; while `Input.IsActionDown`, count ticks and move again after the delay ticks, then every repeat-interval ticks. Count ticks, never seconds or wall time.
- **Pooling.** Create a fixed set of entities once, for example in `OnStart` with `Scene.Instantiate(prefab, position, nil, parent)`, which places each at a world position without interpolation, then reuse them with `SetActive(true/false)` and `Transform:Teleport` instead of creating and destroying them. `OnEnable`/`OnDisable` run on each toggle of a scripted entity. `SetActive` does nothing when the state is unchanged, but `MeshRenderer:SetMaterial` and every proxy field write rewrite the component on each call, so keep a shadow array of what each entity shows and touch only what changed, and write HUD `Text` only when its string changes. Give pooled entities no `RigidBody` or collider: a disabled body is removed and re-added on enable. Keep grid cells at fixed positions; a `Transform.Translation` written in `OnFixedUpdate` is interpolated, so a moved cell slides between ticks (use `Transform:Teleport`).
- **Records with methods.** Under strict mode a module's object is a record type whose methods are function-valued fields, created by a constructor that fills them (`Board.New()` returning `{Width = 10, ..., ToAscii = toAscii}`, typed by an exported alias), as the `Bag` module after the Breakout example shows. Callers use `board:ToAscii()`.
- **Action names** are exact, case-sensitive strings: an unknown one raises `INPUT_UNKNOWN_ACTION` and disables the instance (`project_validate` reports only an undeclared name written as a string literal in an `Input` call). Define them once (a frozen table of constants in a module) and inject every action from a test case, which refuses unknown names too.
- **Restart.** `Scene.Reload()` restarts the level with a fresh VM and its scene as it started (pass parameters to carry a score or a level number). To keep the VM and a pool instead, reset the module state, the counters and the HUD and `SetActive(false)` every pooled entity; `Scene.Load` of another level needs a scene reference, such as a `Field.Asset("Scene")`.
- **Typing.** Annotate callbacks with the exported instance alias (`function Ball.OnStart(self: Ball)`), so strict mode checks `self` fields. An optional field (`Goal: Entity?`, `Field.Asset`) can be nil: test it (`if self.Goal then`) or cast after checking before using it.
- **Constant-speed ball** (Breakout, Pong). Give the ball a `Dynamic` `RigidBody` with `GravityFactor` 0, `Restitution` 1, `Friction` 0, `LinearDamping` 0, `AngularDamping` 0, `AllowSleeping` false and `MotionQuality` `LinearCast` (continuous collision, so a fast ball cannot pass through a thin paddle or brick between ticks), and keep it in the play plane with `LockTranslation` `[false, false, true]` and every `LockRotation` axis locked. Contacts combine friction as the geometric mean of the two bodies' values and restitution as the larger, so these values on the ball hold against every surface. Bounces still gain or lose a little speed and the angle drifts, so renormalize the velocity every tick, keeping a minimum vertical share so the ball never travels flat between the side walls (example below).
- **Player-driven paddles and platforms.** Make them `Kinematic` and move them with `RigidBody:MoveKinematic` in `OnFixedUpdate`. A kinematic body pushes dynamic bodies but is never stopped by `Static` ones, so clamp its target in the script (a paddle limit), or it slides through the walls. A `Dynamic` paddle would be knocked away by the ball.
- **Characters on moving platforms.** Move a character with `CharacterController:Move` in every `OnFixedUpdate` (a jump is one `Move` with an upward part). A moving platform carries it only when its `CharacterController` has `InheritGroundVelocity` set: while grounded, the platform's horizontal velocity is added to the one `Move` gives, so the character walks relative to the platform and rides it v·t when standing still; without it the platform slides under the character. The carry ends when the character leaves the ground: in the air it moves only as `Move` tells it, so a jump off a moving platform does not keep the platform's speed unless the script adds it.
- **Per-entity collisions.** `OnCollisionEnter` names the entity that owns the other body; colliders under an ancestor with a `RigidBody`, or in a tree of collider-only entities, form one compound body, whose owner receives the body's events. A child collider whose entity has a Script naming a script also receives its own `OnCollisionEnter` and `OnCollisionExit` (`self` its entity, `contact.Collider` itself; the partner's callback names the body's owner, its `contact.OtherCollider` the piece), and its own `OnTriggerEnter` and `OnTriggerExit` when its body passes through a sensor, so pieces gathered into one body can report their own hits. Pieces of one body whose contacts share a normal report the event to one piece only (Jolt merges such contacts): a ball on the seam between two edge-to-edge bricks, or a body resting across two coplanar tiles, notifies one of them, so a piece that must get its own event at a seam, such as a Breakout brick, keeps its own `Static` body (`game-building`, "Scenes, assets and prefabs"). An enter arrives when the shapes touch, or at the step the solver bounces one off the other, never for a near miss that only came within Jolt's 2 cm speculative distance. Break it with `SetActive(false)` and bring it back with `SetActive(true)` on a restart rather than destroying and recreating it.
- **Variants from one prefab.** Instances built with `prefab.instantiate {components}` differ only in their overrides (a material, a script field such as a brick's `Points`); a behaviour reads its own fields from `self` and another's through `GetScript()` (`GetScript("<Name>")` on an entity with several scripts) with its exported alias.

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

```luau path=Assets/Scripts/Bag.luau
--!strict
-- A 7-bag over a generator its caller creates in a callback (Random is unavailable at a module's top level).
export type Bag = {
	Generator: RandomGenerator,
	Pending: { string },
	Draw: (self: Bag) -> string,
}

local function draw(self: Bag): string
	if #self.Pending == 0 then
		self.Pending = self.Generator:Shuffle({ "I", "O", "T", "S", "Z", "J", "L" })
	end
	return table.remove(self.Pending, 1) :: string
end

local Bag = {}

function Bag.New(generator: RandomGenerator): Bag
	local bag: Bag = { Generator = generator, Pending = {}, Draw = draw }
	return bag
end

return Bag
```

A behaviour creates it in `OnStart` with `Bag.New(Random.New(Random.Integer(0, 2147483647)))` and calls `self.Bag:Draw()`; a test passes a `Random.New(<constant>)` to get a known sequence.

## Determinism pitfalls

- Write non-trivial powers with `math.pow(x, y)`, which uses the engine's deterministic math. The `^` operator is exact only when the exponent is a `2`, `3` or `0.5` the compiler knows: written in the expression (`x ^ 2`) or held in a local initialized with it and never reassigned. An exponent held in a parameter, a table field or a reassigned local (`x ^ n`, even when `n` is 2) and every other constant call the platform's C runtime and may differ between machines, and a constant expression such as `2 ^ 10` is folded with the compiling editor's C runtime. A curve such as a gravity speed per level is safest as a frozen integer table indexed by level. `math.sin` and the other `math` functions and `vector.angle` are already deterministic.
- Never let the host decide gameplay or a component: `Application.IsEditor` (true in the editor, false in an export), `Application.IsFocused` (the host window, in every session), `Application.IsHeadless` and `Time.GetRealTime()` differ between the editor, the gate and an export, and split their state hashes.
- `pairs`/`next` over a table keyed by entities, components, asset references, tables or functions visits keys in heap-address order, which differs between runs. Key sets by `entity.ID` and sort, or keep an array; iterate with `ipairs`. One entity is the same value as a key while any reference to it is alive, so `inside[other] = true` in `OnTriggerEnter` and `inside[other] = nil` in `OnTriggerExit` work, but never base gameplay on the visiting order.
- Weak tables (`__mode`) lose entries when the garbage collector runs, which is not tied to ticks; never let their contents decide gameplay. `collectgarbage` and `gcinfo` do not exist.
- `Task.Wait(seconds)` and `Task.Delay` wait `ceil(seconds / fixedDelta)` ticks, with decimal multiples of the tick counted exactly (0.14 s at 50 Hz is 7 ticks). Prefer `Task.WaitTicks` when you mean ticks.

## Limits

Log entries and `print` keep at most 8 KiB each (longer text is truncated and marked) and one VM writes at most 1024 entries per frame (the rest are counted and dropped); evaluation output still captures every line. `Debug.DrawText` text is at most 1024 bytes, entity names 256 bytes, and an entity carries at most 256 tags of at most 256 bytes each; one component field value a script writes (for example `Text.Text`, or a value passed to `AddComponent`) holds at most 4096 bytes of text; `Audio.PlayOneShot` starts at most 1024 one-shots per frame. Larger values raise a located error before any change. Test scripts have their own limits: suite and case names at most 256 bytes, `Test.ExpectScriptError` patterns at most 1 KiB, at most 4096 cases per suite, 1024 `Test.Inject*` events per input tick and 256 `Test.Screenshot` calls per suite; a row keeps 32 failures (the last counts the rest) and a suite 1024, and a case keeps at most 256 unclaimed script errors. `script.errors` keeps 256 distinct errors with 8 KiB messages and 32 traceback frames, and reports `evicted` when older ones were dropped. User-defined type functions (`type function`) are not supported: the type checker refuses each declaration with a `SCRIPT_TYPE_ERROR` (it would run their bodies without a bound), so write ordinary type aliases instead.

## Testing and diagnosis

Create test scripts with `Test.Suite` and register `Test.Case` inside the suite body; the body registers cases, while the cases perform runtime work (`templates/Scripts/Test` is the starting point). Use `Test.Inject*` and `Test.WaitTicks` for deterministic input. Type an input plan's state as `InputEventState` (`"Down" | "Up" | "Tap"`), which `Test.InjectAction` takes: strict mode refuses a `string` field there (`type Step = { Tick: number, Action: string, State: InputEventState }`). A button injected `Down` stays down until an `Up`, and an analog value (`Test.InjectAction("MoveX", nil, 0.5)`) holds on every later tick until another value replaces it: inject 0 to let go. `Test.Expect` records a failure and continues; `Test.Fail` ends the case. Pass the actual value first to `Test.ExpectEqual(actual, expected, message?)` and `Test.ExpectNear(actual, expected, epsilon?, message?)`: a failure row shows the message, then `(got <actual>, expected <expected>)`, with `, epsilon <e>` for `ExpectNear`. A case for a game-over rule (a lock-out, a kick above the well) also asserts that no next piece spawned (the piece and the next queue unchanged): otherwise another rule, such as a block-out by the next spawn, can end the game and pass the case without the rule under test, depending on what the seed deals next. Run a new rule's case against the code without the rule first; it must fail. Configure suites in `Testing.Suites` with their scene, parameters and supported modes. Scripts and suites cannot read a native module's `Native.*` components or resources, which have no script proxy (`Entity:HasComponent("Native.<Name>")` raises `NotFound`): a suite tests a native game through the built-in state its systems write, such as Transforms, and automation reads the module's own state (`game-debugging`, "Native module"). Run with `test.run` or `Editor --headless --project <project> --run-tests`; `--filter` selects a suite/case.

### Diagnosis

Use `script.errors`, diagnostics and the structured test results to find authored file/line, traceback, callback, entity, slot and tick. A failing behaviour is disabled (only that slot's instance on an entity with several scripts); `script.eval {entity, script: "<Name>"}` binds `self` to the behaviour of that `Script.Define` name, and without `script` to the entity's first slot; in the editor `PauseOnError` may pause the session. Fix the source and confirm the fresh run rather than assuming an old disabled instance restarted. Ordinary editor play supports transactional hot reload; deterministic lockstep/replay/recording/test sessions defer it, and asset reloads, until they end (an editor test's `Test.ReloadScript` excepted).

### Recording

Start input recording at tick zero. Record through a recording suite and `test.run`'s record option (game-building §8), or `input.record`, then verify with `input.replay` and strict hash checking, twice. **During a recording, evaluate only reads.** A `script.eval`, `play.waitFor` or test case that calls a mutating engine API (an entity or component write, `SetActive`, a session `Random` draw, or `Scene.CreateEntity`, `Instantiate`, `Destroy` or `Load`; `Scene.FindByName` and the other `Scene` queries only read) invalidates the recording at once. A write to script tables only (`self.Lines = 5`, a board array) is neither detected nor recorded, so once it shows in the world or an expectation the recording no longer reproduces itself: `input.record`'s `stop` replays every recording, an event-free one included, with its input and refuses it ("does not reproduce itself"). A test-driven recording is published only after an ordinary fresh replay reproduces every state-hash checkpoint and its final hash, and, with input events, either recording path refuses it when a run without them reproduces it too (game-building §7).

A replay is recorded from a `Modes: ["Editor"]` suite whose single case only reads the scene and injects input, named so that no replay path contains its name (game-building §8). A case that taps planned actions at fixed ticks suits a falling-block opening, recorded with `test.run {filter: "Record.Opening", record: "Assets/Tests/Replays/Opening.replay"}`:

```luau path=Assets/Tests/Record/Opening.test.luau
--!strict
local GameScript = require("../../Scripts/Game")

-- The planned input: one tap of each action at its tick, ticks increasing.
type Tap = { Tick: number, Action: string }
local Plan: { Tap } = {
	{ Tick = 12, Action = "MoveLeft" },
	{ Tick = 30, Action = "RotateCW" },
	{ Tick = 48, Action = "HardDrop" },
}

return Test.Suite("Record.Opening", function()
	Test.Case("plays the opening", function()
		local found = Scene.FindByName("Game")
		Test.Expect(found ~= nil, "Main.scene has a Game entity")
		local game = (found :: Entity):GetScript() :: GameScript.Game
		local tick = 0
		for _, tap in Plan do
			Test.WaitTicks(tap.Tick - tick)
			tick = tap.Tick
			Test.InjectAction(tap.Action, "Tap")
		end
		Test.WaitTicks(60)
		Test.Expect(game.LockedCount >= 1, "the input locked a piece")
	end)
end)
```

An **autopilot** steers where hand-written input is impractical, such as a physics ball following a level's `PathNode` waypoints with a PD controller:

```luau path=Assets/Tests/Autopilot/Level1.test.luau
--!strict
local BallScript = require("../../Scripts/Ball")

-- Steering gains of the PD controller and the distance at which a waypoint counts as reached.
local Gain, Damping, Reach = 0.6, 0.25, 1.5

local function byName(a: Entity, b: Entity): boolean
	return a.Name < b.Name
end

return Test.Suite("Autopilot.Level1", function()
	Test.Case("reaches the goal", function()
		local found = Scene.FindByName("Ball")
		Test.Expect(found ~= nil, "the level has a Ball")
		local ballEntity = found :: Entity
		local ball = ballEntity:GetScript() :: BallScript.Ball
		local body = ballEntity.RigidBody :: RigidBody
		local waypoints = Scene.FindAllByTag("PathNode")
		table.sort(waypoints, byName)
		local target = 1
		while not ball.ReachedGoal do
			local offset = waypoints[target].Transform.WorldPosition - ballEntity.Transform.WorldPosition
			if vector.magnitude(vector.create(offset.x, 0, offset.z)) < Reach and target < #waypoints then
				target += 1
			end
			local velocity = body:GetLinearVelocity()
			Test.InjectAction("MoveX", nil, math.clamp(offset.x * Gain - velocity.x * Damping, -1, 1))
			Test.InjectAction("MoveZ", nil, math.clamp(offset.z * Gain - velocity.z * Damping, -1, 1))
			Test.WaitTicks(1)
		end
	end, { TimeoutTicks = 3600 })
end)
```
