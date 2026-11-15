# Script API

Generated from ScriptApiRegistry and TypeRegistry.

## Application

Application identity, window state and deferred quit requests.

### Application.GetPlatform

`() -> string`

Returns the host platform name.

All modes; Runtime|Test; pure/read.

### Application.GetVersion

`() -> string`

Returns the running engine version.

All modes; Runtime|Test; pure/read.

### Application.GetWindowSize

`() -> vector`

Returns the size of the session's view in view coordinates as vector(width, height, 0): the game view's image in ordinary play, the project's Window.Width x Window.Height in lockstep, recording, replay and test sessions.

All modes; Runtime|Test; pure/read.

### Application.IsEditor

`() -> boolean`

True in the editor; false in the standalone Runtime. Gameplay that branches on it splits the editor's and the export's state hashes, which the games suite compares.

All modes; Runtime|Test; pure/read.

### Application.IsFocused

`() -> boolean`

Returns the current native-window focus state. Nondeterministic: it follows the host window in every session, lockstep, recording, replay and test sessions included, so never base gameplay on it.

All modes; Runtime|Test; pure/read.

### Application.IsHeadless

`() -> boolean`

Returns whether the host has no native window.

All modes; Runtime|Test; pure/read.

### Application.Quit

`(exitCode: number?) -> ()`

Requests exit (default 0): stops editor Play, exits Runtime, or records quit and ends the current test suite. Never destroys the executing VM inline.

All modes; Runtime|Test; mutates.

## Assets

Resolves and loads project or built-in assets through the active asset manager.

### Assets.GetPath

`(asset: AssetRef) -> string`

Returns the readable reference path, or an empty string for an unknown handle.

All modes; Runtime|Test; pure/read.

### Assets.GetType

`(asset: AssetRef) -> string`

Returns the asset type name, or None for an unknown handle.

All modes; Runtime|Test; pure/read.

### Assets.IsValid

`(asset: AssetRef?) -> boolean`

Returns whether the handle is currently registered; nil is false.

All modes; Runtime|Test; pure/read.

### Assets.Load

`(reference: string) -> AssetRef?`

Loads a path or hexadecimal reference; missing references return nil and loading failures raise a located error. The import of an asset that is not cached yet does not count toward the callback budget.

All modes; Runtime|Test; pure/read.

## Audio

Session-scoped mixing and one-shot playback.

### Audio.GetGroupVolume

`(group: AudioGroup) -> number`

Read the session group gain.

All modes; Runtime|Test; pure/read.

Argument 1: AudioGroup.

### Audio.PlayOneShot

`(clip: AssetRef, position: vector?, volume: number?, group: AudioGroup?) -> ()`

Play a non-spatial one-shot when position is nil, otherwise spatial; volume defaults 1 and group Sfx. At most 1024 one-shots start per frame; a later call in the frame raises an error.

All modes; Runtime|Test; mutates.

Argument 4: AudioGroup (optional); default Sfx.

### Audio.SetGroupVolume

`(group: AudioGroup, volume: number) -> ()`

Set nonnegative session group gain; the prior mix is restored when the audio system stops.

All modes; Runtime|Test; mutates.

Argument 1: AudioGroup.

## Debug

Depth-tested world-space debug primitives with white color and one-extraction duration by default.

### Debug.Break

`() -> ()`

Pauses editor Play; Runtime does nothing; test runs count the call without pausing in every mode.

All modes; Runtime|Test; mutates.

### Debug.DrawArrow

`(from: vector, to: vector, color: Color?, duration: number?) -> ()`

Adds an arrow with a 0.1 metre head.

All modes; Runtime|Test; mutates.

### Debug.DrawBox

`(center: vector, halfExtents: vector, rotation: Quat?, color: Color?, duration: number?) -> ()`

Adds a box with positive half extents and a normalized rotation, identity by default.

All modes; Runtime|Test; mutates.

### Debug.DrawLine

`(a: vector, b: vector, color: Color?, duration: number?) -> ()`

Adds a line segment; duration is nonnegative simulation seconds.

All modes; Runtime|Test; mutates.

### Debug.DrawRay

`(origin: vector, direction: vector, color: Color?, duration: number?) -> ()`

Adds a ray segment whose length is the nonzero direction vector's magnitude.

All modes; Runtime|Test; mutates.

### Debug.DrawSphere

`(center: vector, radius: number, color: Color?, duration: number?) -> ()`

Adds a sphere with positive radius.

All modes; Runtime|Test; mutates.

### Debug.DrawText

`(position: vector, text: string, color: Color?, duration: number?) -> ()`

Adds UTF-8 text of at most 1024 bytes at a world position with the default 16-pixel size.

All modes; Runtime|Test; mutates.

## Field

Persistent behaviour field schemas.

### Field.Array

`(element: FieldDescriptor) -> FieldDescriptor`

Declare an empty array whose element keeps its complete nested field schema.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Asset

`(type: string) -> FieldDescriptor`

Declare an asset reference restricted to the named AssetType (Scene, Prefab, Mesh, Material, Texture, Environment, AudioClip for imported audio and .sfx sound effects, Script, Font, Replay, SpriteSheet, Tilemap, StringTable, Skeleton, AnimationClip, IrradianceVolume, Lut or Heightfield), with a null default; any other name fails the import.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Bool

`(default: boolean?, options: FieldOptions?) -> FieldDescriptor`

Declare a boolean; the omitted default is false.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Color

`(default: Color?, options: FieldOptions?) -> FieldDescriptor`

Declare a linear RGBA color; the omitted default is white.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Entity

`() -> FieldDescriptor`

Declare an Entity reference with a null default.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Enum

`(values: {string}, default: string?) -> FieldDescriptor`

Declare ordered unique string choices; the omitted default is the first choice.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Integer

`(default: number?, options: FieldOptions?) -> FieldDescriptor`

Declare an exact signed 32-bit integer; the omitted default is zero.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Number

`(default: number?, options: FieldOptions?) -> FieldDescriptor`

Declare a finite f32 number; the omitted default is zero.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Quat

`(default: Quat?, options: FieldOptions?) -> FieldDescriptor`

Declare a finite nonzero quaternion; the omitted default is identity.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.String

`(default: string?, options: FieldOptions?) -> FieldDescriptor`

Declare a string; the omitted default is empty.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Vector

`(default: vector?, options: FieldOptions?) -> FieldDescriptor`

Declare a native three-component vector; the omitted default is zero.

All modes; LoadTime|Runtime|Test; pure/read.

## Input

Queries the current step or frame latch, including injected input; gamepad indexes are zero-based.

### Input.GetAxis

`(name: string) -> number`

Returns a configured action's combined axis value.

All modes; Runtime|Test; pure/read.

### Input.GetCursorMode

`() -> CursorMode`

Returns the canonical cursor mode name.

All modes; Runtime|Test; pure/read.

### Input.GetGamepadAxis

`(index: number, axis: GamepadAxis) -> number`

Returns an engine-convention axis, or zero for a disconnected pad.

All modes; Runtime|Test; pure/read.

Argument 2: GamepadAxis.

### Input.GetMouseDelta

`() -> vector`

Returns accumulated cursor movement in this phase.

All modes; Runtime|Test; pure/read.

### Input.GetMousePosition

`() -> vector`

Returns the cursor position in view coordinates as vector(x, y, 0): the pixels of the session's view, origin top-left (the game view's image in ordinary play, the project's Window.Width x Window.Height in lockstep, recording, replay and test sessions).

All modes; Runtime|Test; pure/read.

### Input.GetScrollDelta

`() -> vector`

Returns accumulated scroll, right-positive and up-positive.

All modes; Runtime|Test; pure/read.

### Input.GetText

`() -> string`

Returns the characters typed in the latched view as UTF-8: in the fixed step (OnFixedUpdate, tasks, physics and UI callbacks) the tick's committed characters, in the frame phase (OnUpdate, OnLateUpdate) those of every tick since the previous frame; empty when none were typed.

All modes; Runtime|Test; pure/read.

### Input.IsActionDown

`(name: string) -> boolean`

Returns a configured action's held state; an unknown name raises INPUT_UNKNOWN_ACTION.

All modes; Runtime|Test; pure/read.

### Input.IsActionPressed

`(name: string) -> boolean`

Returns whether a configured action was pressed since the previous latch of the current phase: in OnFixedUpdate since the previous tick, in OnUpdate and OnLateUpdate since the previous frame. A press and release between two ticks reports both edges with IsActionDown false, and a frame that runs several ticks reports the edge on its first tick only; an unknown name raises INPUT_UNKNOWN_ACTION.

All modes; Runtime|Test; pure/read.

### Input.IsActionReleased

`(name: string) -> boolean`

Returns whether a configured action was released since the previous latch of the current phase: in OnFixedUpdate since the previous tick, in OnUpdate and OnLateUpdate since the previous frame. A press and release between two ticks reports both edges with IsActionDown false, and a frame that runs several ticks reports the edge on its first tick only; an unknown name raises INPUT_UNKNOWN_ACTION.

All modes; Runtime|Test; pure/read.

### Input.IsGamepadButtonDown

`(index: number, button: GamepadButton) -> boolean`

Returns a gamepad button's held state.

All modes; Runtime|Test; pure/read.

Argument 2: GamepadButton.

### Input.IsGamepadConnected

`(index: number) -> boolean`

Returns whether gamepad 0 through 3 is connected.

All modes; Runtime|Test; pure/read.

### Input.IsKeyDown

`(key: Key) -> boolean`

Returns the held key state in this phase.

All modes; Runtime|Test; pure/read.

Argument 1: Key.

### Input.IsKeyPressed

`(key: Key) -> boolean`

Returns this phase's press edge; repeated keys do not add an edge.

All modes; Runtime|Test; pure/read.

Argument 1: Key.

### Input.IsKeyReleased

`(key: Key) -> boolean`

Returns this phase's release edge.

All modes; Runtime|Test; pure/read.

Argument 1: Key.

### Input.IsMouseButtonDown

`(button: MouseButton) -> boolean`

Returns the held mouse-button state.

All modes; Runtime|Test; pure/read.

Argument 1: MouseButton.

### Input.IsMouseButtonPressed

`(button: MouseButton) -> boolean`

Returns this phase's mouse-button press edge.

All modes; Runtime|Test; pure/read.

Argument 1: MouseButton.

### Input.IsMouseButtonReleased

`(button: MouseButton) -> boolean`

Returns this phase's mouse-button release edge.

All modes; Runtime|Test; pure/read.

Argument 1: MouseButton.

### Input.SetCursorMode

`(mode: CursorMode) -> ()`

Changes cursor visibility/capture; refused during read-only evaluation.

All modes; Runtime|Test; mutates.

Argument 1: CursorMode.

## Loc

Localized strings from string tables (.stringtable assets) in the project's locale (Localization.Locale): a table's locale equal to it, else its language's, else the table's first (fallback) locale; an entry without a string for that locale gives the fallback's.

### Loc.Get

`(table: AssetRef, key: string) -> string`

Returns the string of key in the locale. A key the table lacks, an asset that is not a string table, or a table that fails to load raises a located error naming them (with the closest keys as a hint).

All modes; Runtime|Test; pure/read.

### Loc.GetLocale

`() -> string`

Returns the project's locale (Localization.Locale) that lookups use; empty when it is unset, so every table reads its fallback locale.

All modes; Runtime|Test; pure/read.

### Loc.Has

`(table: AssetRef, key: string) -> boolean`

Returns whether the string table has the key. An asset that is not a string table, or a table that fails to load, raises a located error naming it, as in Get.

All modes; Runtime|Test; pure/read.

## Log

Script-channel logging with authored source, line, entity and simulation tick; arguments use tostring and tab separators. An entry keeps at most 8192 bytes (longer text is truncated and marked), and one VM writes at most 1024 entries per frame, dropping and counting the rest.

### Log.Error

`(...any) -> ()`

Writes an Error entry without throwing or disabling the script instance.

All modes; Runtime|Test; pure/read.

### Log.Info

`(...any) -> ()`

Writes an Info entry and captures evaluation output. The print global aliases this registered function.

All modes; Runtime|Test; pure/read.

### Log.Trace

`(...any) -> ()`

Writes a Trace entry; compiled out of Dist.

All modes; Runtime|Test; pure/read.

### Log.Warn

`(...any) -> ()`

Writes a Warn entry without raising a script error.

All modes; Runtime|Test; pure/read.

## Math

Deterministic interpolation, smoothing and angle utilities. All arguments and results must be finite.

### Math.Approximately

`(a: number, b: number, epsilon: number?) -> boolean`

Compares within epsilon times max(1, abs(a), abs(b)); epsilon defaults to 0.000001 and must be nonnegative.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Clamp

`((value: number, minimum: number, maximum: number) -> number) & ((value: vector, minimum: vector, maximum: vector) -> vector)`

Clamps to ordered inclusive bounds; vector bounds apply component-wise. Reversed bounds are errors.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Clamp01

`((value: number) -> number) & ((value: vector) -> vector)`

Clamps a number or every vector component to [0, 1].

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Deg2Rad

`number`

Multiplier from degrees to radians.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.DeltaAngle

`(current: number, target: number) -> number`

Returns the shortest signed difference in degrees in (-180, 180], choosing +180 for a half turn.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.InverseLerp

`(a: number, b: number, value: number) -> number`

Returns the clamped [0, 1] fraction along a to b, including descending intervals; equal endpoints return zero.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Lerp

`((a: number, b: number, t: number) -> number) & ((a: vector, b: vector, t: number) -> vector)`

Interpolates with t clamped to [0, 1]; t=0 returns a and t=1 returns b.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.LerpAngle

`(current: number, target: number, t: number) -> number`

Follows the shortest angular path in degrees with t clamped to [0, 1]; preserves current's full turns.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.MoveTowards

`((current: number, target: number, maxDelta: number) -> number) & ((current: vector, target: vector, maxDelta: number) -> vector)`

Moves at most nonnegative maxDelta toward target without overshooting; vectors use Euclidean distance.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Pi

`number`

Pi, rounded to engine float precision.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.PingPong

`(value: number, length: number) -> number`

Reflects a repeating value into [0, length] with period 2*length; length must be positive.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Rad2Deg

`number`

Multiplier from radians to degrees.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Remap

`(value: number, sourceMinimum: number, sourceMaximum: number, targetMinimum: number, targetMaximum: number) -> number`

Maps the clamped fraction along a nonempty source interval to the target interval; either may descend.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Repeat

`(value: number, length: number) -> number`

Wraps either sign of value into [0, length); length must be positive.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.Sign

`(value: number) -> number`

Returns -1, 0 or 1; both signed zeros return zero.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.SmoothDamp

`((current: number, target: number, velocity: number, smoothTime: number, dt: number) -> (number, number)) & ((current: vector, target: vector, velocity: vector, smoothTime: number, dt: number) -> (vector, vector))`

Exact critically damped spring step, component-wise for vectors, returning value and velocity without crossing the target. smoothTime must be positive (effective minimum 0.0001 seconds), dt nonnegative; zero dt preserves both inputs.

All modes; LoadTime|Runtime|Test; pure/read.

### Math.SmoothStep

`(a: number, b: number, t: number) -> number`

Interpolates from a to b using 3t squared minus 2t cubed, with t clamped to [0, 1].

All modes; LoadTime|Runtime|Test; pure/read.

## Physics

Queries and gravity for the active scene's physics system.

### Physics.GetBodyBounds

`(entity: Entity) -> (vector?, vector?)`

World AABB of the body's complete compound, or nil if no body is owned.

All modes; Runtime|Test; pure/read.

### Physics.GetColliderBounds

`(entity: Entity) -> (vector?, vector?)`

World AABB of this entity's collider sub-shapes, or nil if absent.

All modes; Runtime|Test; pure/read.

### Physics.GetGravity

`() -> vector`

Current world gravity.

All modes; Runtime|Test; pure/read.

### Physics.LayerMask

`(...string) -> number`

Combine named project layers; omitted query masks include every layer.

All modes; Runtime|Test; pure/read.

### Physics.OverlapBox

`(center: vector, halfExtents: vector, rotation: Quat?, layerMask: number?) -> {Entity}`

Unique overlapping collider entities sorted by UUID; default identity rotation.

All modes; Runtime|Test; pure/read.

### Physics.OverlapSphere

`(center: vector, radius: number, layerMask: number?) -> {Entity}`

Unique overlapping collider entities sorted by UUID.

All modes; Runtime|Test; pure/read.

### Physics.Raycast

`(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?`

Closest hit, including sensors; normalize direction and reject invalid finite ranges.

All modes; Runtime|Test; pure/read.

### Physics.RaycastAll

`(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> {RaycastHit}`

All hits sorted by distance then collider UUID.

All modes; Runtime|Test; pure/read.

### Physics.SetGravity

`(gravity: vector) -> ()`

Set finite gravity within physics bounds for the session.

All modes; Runtime|Test; mutates.

### Physics.SphereCast

`(origin: vector, radius: number, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?`

Closest swept sphere hit; radius obeys collider size bounds.

All modes; Runtime|Test; pure/read.

## Physics2D

Queries and gravity for the active scene's 2D physics world, in the XY plane.

### Physics2D.GetGravity

`() -> vector`

Current 2D world gravity (Z is 0).

All modes; Runtime|Test; pure/read.

### Physics2D.OverlapBox

`(center: vector, halfExtents: vector, angle: number?, layerMask: number?) -> {Entity}`

Unique overlapping 2D collider entities sorted by UUID; the box rotated by angle degrees about +Z (0 by default).

All modes; Runtime|Test; pure/read.

### Physics2D.OverlapCircle

`(center: vector, radius: number, layerMask: number?) -> {Entity}`

Unique overlapping 2D collider entities sorted by UUID; radius obeys collider size bounds.

All modes; Runtime|Test; pure/read.

### Physics2D.Raycast

`(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?`

Closest 2D hit, including sensors; vectors in the plane (Z ignored, results with Z 0).

All modes; Runtime|Test; pure/read.

### Physics2D.RaycastAll

`(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> {RaycastHit}`

All 2D hits sorted by distance then collider UUID.

All modes; Runtime|Test; pure/read.

### Physics2D.SetGravity

`(gravity: vector) -> ()`

Set the 2D world's gravity from the vector's X and Y for the session.

All modes; Runtime|Test; mutates.

## Random

Deterministic shared simulation randomness. Invalid arguments never advance the stream.

### Random.Bool

`(probability: number?) -> boolean`

True with probability in [0,1], default one half.

All modes; Runtime|Test; mutates.

### Random.Choice

`<T>(array: {T}) -> T`

Chooses from a nonempty dense one-based array; frozen arrays may be read.

All modes; Runtime|Test; mutates.

### Random.Integer

`(min: number, max: number) -> number`

Uniform integer in inclusive bounds, each within the exact-number integer range.

All modes; Runtime|Test; mutates.

### Random.New

`(seed: number) -> RandomGenerator`

Creates an independent local stream; allowed in read-only evaluation. Its state is a script value outside the state hash, and a constant seed repeats the same sequence in every session: seed it from Random.Integer (Random.New(Random.Integer(0, 2147483647))) to follow the session seed. Not available at a module's top level.

All modes; Runtime|Test; pure/read.

### Random.Number

`(min: number?, max: number?) -> number`

Uniform in [min,max); defaults to [0,1), one argument is the maximum. Equal bounds still consume one draw.

All modes; Runtime|Test; mutates.

### Random.Seed

`(seed: number) -> ()`

Reseeds with a nonnegative exact integer no greater than 2^53-1.

All modes; Runtime|Test; mutates.

### Random.Shuffle

`<T>(array: {T}) -> {T}`

Fisher-Yates shuffle in place, returning the same array. Rejects holes, dictionary keys and frozen tables.

All modes; Runtime|Test; mutates.

### Random.UnitVector

`() -> vector`

Uniform unit direction using two draws and deterministic trigonometry.

All modes; Runtime|Test; mutates.

## Scene

Active-scene operations; searches return canonical order and never expose pending destruction.

### Scene.CreateEntity

`(name: string?, parent: Entity?) -> Entity`

Creates immediately with a seeded ID and default name Entity (a name is at most 256 bytes). Runs OnCreate before returning; use IsValid if it may destroy itself. A physics body it is given appears at the next physics pre-step: created in the fixed step before it (OnFixedUpdate, a task, a test case), the entity has its body by OnStart; created after it (a physics callback, OnDestroy, the frame phase, between ticks), its OnStart and first OnFixedUpdate run without one and body methods raise there.

All modes; Runtime|Test; mutates.

### Scene.Destroy

`(entity: Entity) -> ()`

Marks the entity and descendants immediately; the session performs lifecycle teardown and destruction at its safe point.

All modes; Runtime|Test; mutates.

### Scene.FindAllByName

`(name: string) -> {Entity}`

Returns all matching names in canonical scene order.

All modes; Runtime|Test; pure/read.

### Scene.FindAllByTag

`(tag: string) -> {Entity}`

Returns all tagged entities in canonical scene order.

All modes; Runtime|Test; pure/read.

### Scene.FindAllWithComponent

`(name: string) -> {Entity}`

Returns every entity carrying a script-visible component; unknown names raise a located error.

All modes; Runtime|Test; pure/read.

### Scene.FindByID

`(id: string) -> Entity?`

Finds an exact 16-digit hexadecimal UUID; malformed IDs error, absent entities return nil.

All modes; Runtime|Test; pure/read.

### Scene.FindByName

`(name: string) -> Entity?`

Returns the first matching name in canonical scene order, or nil.

All modes; Runtime|Test; pure/read.

### Scene.FindByPath

`(path: string) -> Entity?`

Resolves an escaped absolute hierarchy path; missing, malformed or ambiguous paths return nil.

All modes; Runtime|Test; pure/read.

### Scene.FindByTag

`(tag: string) -> Entity?`

Returns the first tagged entity in canonical scene order, or nil.

All modes; Runtime|Test; pure/read.

### Scene.GetCurrent

`() -> AssetRef?`

Returns the scene asset the current scene came from, or nil for a scene without one.

All modes; Runtime|Test; pure/read.

### Scene.GetEntityCount

`() -> number`

Returns the number of live entities, excluding pending destruction.

All modes; Runtime|Test; pure/read.

### Scene.GetLoadParameters

`() -> {[string]: any}`

Returns a detached copy of the parameters passed to the current scene.

All modes; Runtime|Test; pure/read.

### Scene.GetName

`() -> string`

Returns the scene name.

All modes; Runtime|Test; pure/read.

### Scene.GetPrimaryCamera

`() -> Entity?`

Returns the first active Primary camera in canonical order, or nil.

All modes; Runtime|Test; pure/read.

### Scene.Instantiate

`(prefab: AssetRef, position: vector?, rotation: Quat?, parent: Entity?) -> Entity`

Instantiates through the shared prefab path and invokes OnCreate for the complete subtree before returning. A position that cannot be represented under the parent raises a located error before anything is created. Its physics bodies appear at the next physics pre-step, as Scene.CreateEntity's do.

All modes; Runtime|Test; mutates.

### Scene.Load

`(scene: AssetRef, parameters: {[string]: any}?) -> ()`

Requests a scene/VM replacement at the end of the frame with copied JSON parameters; never destroys the calling VM inline. Until then the frame's remaining fixed steps still run in the current scene and VM, and another Scene.Load or Scene.Reload raises InvalidState: keep a flag once a transition is requested and request nothing more.

All modes; Runtime|Test; mutates.

### Scene.Reload

`(parameters: {[string]: any}?) -> ()`

Requests a restart of the current scene from the document it started from, at the end of the frame; parameters replace the load parameters, absent keeps them. Until then the frame's remaining fixed steps still run in the current scene and VM, and another Scene.Load or Scene.Reload raises InvalidState: keep a flag once a transition is requested and request nothing more.

All modes; Runtime|Test; mutates.

## Script

Behaviour class registration.

### Script.Define

`<T>(name: string, class: T) -> T`

Authenticate a behaviour class and return the exact same table.

All modes; LoadTime|Runtime|Test; pure/read.

## Task

Caller-owned coroutines scheduled in simulation ticks. All scheduling and waits require writable execution.

### Task.Cancel

`(handle: TaskHandle) -> ()`

Cancels without resuming; cancelling a completed or already cancelled task succeeds.

All modes; Runtime|Test; mutates.

### Task.Delay

`(seconds: number, fn: () -> ()) -> TaskHandle`

Schedules the caller-owned function after ceil(seconds/fixedDelta) ticks, at least one.

All modes; Runtime|Test; mutates.

### Task.Spawn

`(fn: () -> ()) -> TaskHandle`

Runs immediately to the first yield/end; inherits the caller's entity, case and deadline.

All modes; Runtime|Test; mutates.

### Task.Wait

`(seconds: number) -> ()`

Yields a task or case for ceil(seconds/fixedDelta) ticks, at least one; invalid in nonyieldable callbacks.

All modes; Runtime|Test; mutates.

### Task.WaitTicks

`(ticks: number) -> ()`

Yields for an exactly representable nonnegative integer number of ticks, at least one.

All modes; Runtime|Test; mutates.

## Test

Feature test suites, assertions, waits and deterministic input. A reported message shows authored locations and keeps at most 8192 bytes, truncated and marked like a log entry. A failed Test.ExpectEqual or Test.ExpectNear adds its values after the message, which is cut first so they always fit: nil, booleans and numbers (shortest round-trip form) as Luau writes them, strings quoted (their first 128 bytes, then their length), vectors, Colors and Quats by component, Entities and AssetRefs by UUID and other values by type name. A case's report keeps at most 32 failures for expectations and unexpected script errors, the last then counting any further ones as [truncated: <n> more failures], and a suite at most 1024 such failures. Test.Inject* queues at most 1024 events per input tick.

### Test.CaptureAudio

`(ticks: number) -> TestAudioLevels`

Capture exactly the requested current-and-following ticks; yield until grouped audio is ready.

All modes; Test; mutates.

### Test.Case

`(name: string, fn: () -> (), options: TestCaseOptions?) -> ()`

Register a case while collecting the suite body; the name is nonempty, unique in the suite and at most 256 bytes, and a suite has at most 4096 cases.

All modes; Test; pure/read.

### Test.Expect

`(condition: boolean, message: string?) -> ()`

Record a failed condition and continue the case.

All modes; Test; pure/read.

### Test.ExpectEqual

`(actual: any, expected: any, message: string?) -> ()`

Record a failed Luau equality comparison of actual with expected. The failure shows the message (default "expected values to be equal") followed by (got <actual>, expected <expected>).

All modes; Test; pure/read.

### Test.ExpectNear

`(actual: number | vector, expected: number | vector, epsilon: number?, message: string?) -> ()`

Compare finite numbers or vector components with an absolute epsilon, default 0.00001. The failure shows the message (default "values differ by more than epsilon") followed by (got <actual>, expected <expected>, epsilon <epsilon>).

All modes; Test; pure/read.

### Test.ExpectScriptError

`(pattern: string, withinTicks: number) -> ()`

Claim one nonfatal error occurrence in the current case using a Luau message pattern of at most 1024 bytes; the case keeps at most 256 unclaimed errors to claim.

All modes; Test; pure/read.

### Test.Fail

`(message: string) -> ()`

End this case as failed. A case thread, or a task a behaviour spawned (which is cancelled), never runs past the call, even inside pcall; a lifecycle callback cannot yield, so there the call raises a script error that unwinds to the nearest pcall. The outcome stays latched either way.

All modes; Test; pure/read.

### Test.GetAudioClipDigest

`(clip: AssetRef) -> string`

Returns the XXH64 of an audio clip's samples decoded to 32-bit float at its own rate, as 16 hexadecimal digits; test suites only.

All modes; Test; pure/read.

### Test.GetExtractedPosition

`(entity: Entity) -> vector?`

Read an entity's position in the last render snapshot.

All modes; Test; pure/read.

### Test.GetLastExtraction

`() -> TestExtraction`

Read the last completed render extraction.

All modes; Test; pure/read.

### Test.GetParticleDigest

`(entity: Entity) -> string?`

Returns the seeded particle hash of a ParticleEmitter entity (the XXH64 of its live particles, random stream and emission state) as 16 hexadecimal digits, or nil before its first Particles phase or without a ParticleEmitter; the same in every configuration, so a suite compares it across runs; test suites only.

All modes; Test; pure/read.

### Test.GetPoseDigest

`(entity: Entity) -> string?`

Returns the digest of an Animator entity's pose sampled in the last fixed step (the XXH64 of its joint model matrices and morph weights) as 16 hexadecimal digits, or nil without a pose; the same in every configuration, so a suite compares it across runs; test suites only.

All modes; Test; pure/read.

### Test.GetScriptErrors

`(since: number?) -> {ScriptError}`

Read occurrences newer than an exclusive error cursor; claimed errors remain visible. The VM keeps the 256 distinct errors with the latest occurrences.

All modes; Test; pure/read.

### Test.GetStateHash

`() -> string`

Read the canonical simulated state hash: the world (every entity with its serialized components, the tick, the session's Random state, the id draws and every body's physics state), never script tables, audio or input. It includes the tick, so two reads at different ticks always differ: compare two runs with input.replay's stateHash instead.

All modes; Test; pure/read.

### Test.InjectAction

`(name: string, state: InputEventState?, value: number?) -> ()`

Queue a named button state or analog value (with a nil or Down state) for the next unapplied input tick. A button stays down until an Up (Tap releases it on the next tick), and an analog value holds on every later tick until another value is injected for the action (0 releases it).

All modes; Test; mutates.

Argument 2: InputEventState (optional).

### Test.InjectGamepad

`((index: number, button: GamepadButton, state: InputEventState) -> ()) & ((index: number, axis: GamepadAxis, value: number) -> ())`

Queue a gamepad button state, or an engine-convention axis value when the third argument is a number.

All modes; Test; mutates.

Argument 2: GamepadButton when argument 3 is a string.

Argument 3: InputEventState when argument 3 is a string.

Argument 2: GamepadAxis when argument 3 is a number.

### Test.InjectKey

`(key: Key, state: InputEventState) -> ()`

Queue a key state for the next unapplied input tick.

All modes; Test; mutates.

Argument 1: Key.

Argument 2: InputEventState.

### Test.InjectMouse

`(button: MouseButton, state: InputEventState, position: vector?) -> ()`

Queue a mouse button and optional finite XY position.

All modes; Test; mutates.

Argument 1: MouseButton.

Argument 2: InputEventState.

### Test.InjectMouseMove

`(position: vector) -> ()`

Queue a mouse move to a finite XY position in view coordinates for the next unapplied input tick.

All modes; Test; mutates.

### Test.InjectScroll

`(delta: vector) -> ()`

Queue a scroll of a finite XY delta (wheel steps, y up-positive) for the next unapplied input tick.

All modes; Test; mutates.

### Test.InjectText

`(text: string) -> ()`

Queue committed text (non-empty UTF-8) for the next unapplied input tick, as typed characters.

All modes; Test; mutates.

### Test.ReloadScript

`(asset: AssetRef) -> ()`

Synchronously reload a script in an editor test.

EditorOnly; Test; mutates.

### Test.Screenshot

`(name: string) -> ()`

Capture a screenshot through the owning host; a suite takes at most 256 screenshots, and a later call raises an error without capturing.

All modes; Test; mutates.

### Test.Skip

`(reason: string) -> ()`

End this case as skipped without erasing earlier failures; it stops its caller the way Test.Fail does, and the outcome stays latched.

All modes; Test; pure/read.

### Test.Suite

`(name: string, body: () -> (), options: TestSuiteOptions?) -> TestSuite`

Construct a suite without executing its body; the name is UTF-8 without NUL and at most 256 bytes.

All modes; LoadTime|Runtime|Test; pure/read.

### Test.WaitTicks

`(ticks: number) -> ()`

Yield a test thread for simulation ticks.

All modes; Test; pure/read.

### Test.WaitUntil

`(predicate: () -> boolean, timeoutTicks: number) -> ()`

Evaluate the predicate now and after every following tick until it returns true; it must return a boolean (anything else is a script error). Still false after timeoutTicks ticks, the case fails with "Test.WaitUntil exceeded its tick limit" at this call.

All modes; Test; pure/read.

## Time

Phase-aware simulation time; use fixed time for deterministic gameplay.

### Time.GetDeltaTime

`() -> number`

Returns the fixed step in the fixed phase (the time scale changes how many steps a frame runs, not their length) and the scaled frame delta in seconds in the frame phase.

All modes; Runtime|Test; pure/read.

### Time.GetFixedDeltaTime

`() -> number`

Returns the session's fixed step in seconds.

All modes; Runtime|Test; pure/read.

### Time.GetFrameCount

`() -> number`

Returns the current rendered-frame index.

All modes; Runtime|Test; pure/read.

### Time.GetInterpolationAlpha

`() -> number`

Returns one during the fixed phase, otherwise this frame's interpolation alpha.

All modes; Runtime|Test; pure/read.

### Time.GetRealTime

`() -> number`

Returns monotonic wall-clock seconds from an unspecified epoch. Nondeterministic; never use for gameplay.

All modes; Runtime|Test; pure/read.

### Time.GetTick

`() -> number`

Returns the current simulation tick.

All modes; Runtime|Test; pure/read.

### Time.GetTime

`() -> number`

Returns simulation seconds: tick multiplied by fixed delta.

All modes; Runtime|Test; pure/read.

### Time.GetTimeScale

`() -> number`

Returns the current session time scale.

All modes; Runtime|Test; pure/read.

### Time.SetTimeScale

`(scale: number) -> ()`

Sets the session time scale in [0, 100]; refused during read-only evaluation.

All modes; Runtime|Test; mutates.

## Ui

The game UI's interaction state in the running scene: keyboard and gamepad focus, the widget under the pointer and the one it presses, and where elements were laid out. UI events reach behaviours through the OnUi callbacks, which bubble from the element to its ancestors.

### Ui.GetFocus

`() -> Entity?`

Returns the UI element that has keyboard and gamepad focus, or nil.

All modes; Runtime|Test; pure/read.

### Ui.GetHovered

`() -> Entity?`

Returns the interactable UI widget under the pointer, or nil.

All modes; Runtime|Test; pure/read.

### Ui.GetPressed

`() -> Entity?`

Returns the UI widget the held pointer pressed, or nil.

All modes; Runtime|Test; pure/read.

### Ui.GetRect

`(element: Entity) -> UiElementRect?`

Returns element's rectangle in the current UI layout, in scene pixels (which are view coordinates unless the output is upscaled) or, for an element of a World canvas, in that canvas's own pixels, with its canvas; nil when it is not laid out.

All modes; Runtime|Test; pure/read.

### Ui.IsPointerOverUi

`() -> boolean`

Returns whether the pointer was over a UI image, text or widget that catches it at this tick, so gameplay can ignore clicks meant for the UI.

All modes; Runtime|Test; pure/read.

### Ui.SetFocus

`(element: Entity?) -> ()`

Moves focus to element, a laid-out, interactable and focusable UI widget, or clears it with nil; OnUiFocus follows in the next UiEvents phase. Any other element raises a located error saying why.

All modes; Runtime|Test; mutates.

## Animator

Poses a skeleton from layers of animation clips, blended by their weights, each fixed step; skinned meshes on the entity and below it follow.

### Animator.GetJointTransform

`(self: Animator, joint: string) -> (vector?, Quat?)`

Read the world position and rotation of the skeleton's joint named joint in the pose of the last fixed step's AnimationSystem phase; nil, nil without a pose or such a joint, and the position with a nil rotation when a scale of 0 on the way (a clip scaling a joint to nothing) leaves the joint no rotation.

All modes; Runtime|Test; pure/read.

### Animator.GetRootMotion

`(self: Animator) -> (vector, Quat)`

Read the last fixed step's blended root motion in the entity's parent space: the translation RootMotion Apply adds to the Transform's Translation and the rotation it multiplies into Rotation; zero and identity without a pose or with RootMotion None.

All modes; Runtime|Test; pure/read.

### Animator.Layers

`{{Clip: AssetRef?, Time: number, Speed: number, Weight: number, Loop: boolean}}`

The layers, at most eight, blended in order over the skeleton's rest pose.

All modes; Runtime|Test; pure/read; setter mutates.

### Animator.Play

`(self: Animator, layer: number, clip: AssetRef?, time: number?) -> ()`

Set the 1-based layer to play the clip from time seconds (0 when absent), keeping its Speed, Weight and Loop; the next index appends a layer with Speed 1, Weight 1 and Loop. A layer beyond the next one or beyond 8, or a time below 0, raises an error.

All modes; Runtime|Test; mutates.

### Animator.Playing

`boolean`

Whether the layers' times advance each fixed step; the pose follows the times either way.

All modes; Runtime|Test; pure/read; setter mutates.

### Animator.RootMotion

`AnimatorRootMotion`

What happens to the skeleton's root motion: kept in the pose, applied to the entity or only reported.

All modes; Runtime|Test; pure/read; setter mutates.

### Animator.SetWeight

`(self: Animator, layer: number, weight: number) -> ()`

Set the 1-based layer's Weight, from 0 (no effect) to 1 (it overrides the layers before it on the paths its clip keys); a crossfade tweens two layers' weights. A layer the Animator lacks or a weight outside [0, 1] raises an error.

All modes; Runtime|Test; mutates.

### Animator.Skeleton

`AssetRef?`

The skeleton the layers' clips pose; null poses nothing.

All modes; Runtime|Test; pure/read; setter mutates.

## AssetRef

An opaque immutable asset handle; resolve metadata through Assets.

### AssetRef.__eq

`(self: AssetRef, other: AssetRef) -> boolean`

Compares asset handles, so two references to the same asset are equal.

All modes; LoadTime|Runtime|Test; pure/read.

## AudioListener

Hears the scene from the entity's position and orientation.

### AudioListener.Primary

`boolean`

Whether this listener is used; with several primaries the first in canonical order wins, without any the primary camera listens.

All modes; Runtime|Test; pure/read; setter mutates.

## AudioSource

Plays an audio clip, optionally positioned at the entity.

### AudioSource.Attenuation

`Attenuation`

The distance attenuation model of a spatial sound.

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.Clip

`AssetRef?`

The audio clip to play; null plays nothing.

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.DopplerFactor

`number`

The strength of the Doppler pitch shift (0 disables it).

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.Group

`AudioGroup`

The mixer group the sound plays in.

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.IsPlaying

`(self: AudioSource) -> boolean`

Whether this source owns an unpaused voice.

All modes; Runtime|Test; pure/read.

### AudioSource.Loop

`boolean`

Whether the clip restarts when it ends.

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.MaxDistance

`number`

The distance beyond which the sound stops getting quieter, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.MinDistance

`number`

The distance within which the sound plays at full volume, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.Pause

`(self: AudioSource) -> ()`

Pause this source without losing its cursor.

All modes; Runtime|Test; mutates.

### AudioSource.Pitch

`number`

The playback speed and pitch multiplier (1 is unchanged).

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.Play

`(self: AudioSource) -> ()`

Start or restart this active source.

All modes; Runtime|Test; mutates.

### AudioSource.PlayOnStart

`boolean`

Whether the clip starts playing when the play session starts.

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.Resume

`(self: AudioSource) -> ()`

Resume this active source's paused voice.

All modes; Runtime|Test; mutates.

### AudioSource.Rolloff

`number`

How quickly the attenuation model falls off (1 is the model's own rate).

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.Spatial

`boolean`

Whether the sound is positioned at the entity and attenuated with distance.

All modes; Runtime|Test; pure/read; setter mutates.

### AudioSource.Stop

`(self: AudioSource) -> ()`

Release this source's voice.

All modes; Runtime|Test; mutates.

### AudioSource.Volume

`number`

The linear volume multiplier (1 is the clip's own level).

All modes; Runtime|Test; pure/read; setter mutates.

## Behaviour

Optional behaviour lifecycle callbacks in execution order and canonical entity order.

### Behaviour.OnAnimationEvent

`(self: ScriptInstance, event: AnimationEvent) -> ()`

Called in the fixed step for each event an Animator layer's time passes on this entity, after every Animator of the tick was posed, in layer and firing order.

All modes; Runtime|Test; mutates.

### Behaviour.OnCollisionEnter

`(self: ScriptInstance, other: Entity, contact: CollisionContact) -> ()`

Called when a collision begins, with the other entity and contact.

All modes; Runtime|Test; mutates.

### Behaviour.OnCollisionEnter2D

`(self: ScriptInstance, other: Entity, contact: CollisionContact) -> ()`

Called when a 2D collision begins, with the other entity and the contact in the XY plane.

All modes; Runtime|Test; mutates.

### Behaviour.OnCollisionExit

`(self: ScriptInstance, other: Entity) -> ()`

Called when the corresponding collision or trigger transition occurs.

All modes; Runtime|Test; mutates.

### Behaviour.OnCollisionExit2D

`(self: ScriptInstance, other: Entity) -> ()`

Called when the corresponding 2D collision or trigger transition occurs.

All modes; Runtime|Test; mutates.

### Behaviour.OnCreate

`(self: ScriptInstance) -> ()`

Called at the corresponding lifecycle transition.

All modes; Runtime|Test; mutates.

### Behaviour.OnDestroy

`(self: ScriptInstance) -> ()`

Called at the corresponding lifecycle transition.

All modes; Runtime|Test; mutates.

### Behaviour.OnDisable

`(self: ScriptInstance) -> ()`

Called at the corresponding lifecycle transition.

All modes; Runtime|Test; mutates.

### Behaviour.OnEnable

`(self: ScriptInstance) -> ()`

Called at the corresponding lifecycle transition.

All modes; Runtime|Test; mutates.

### Behaviour.OnFixedUpdate

`(self: ScriptInstance, dt: number) -> ()`

Called for the current fixed, presentation or late presentation phase.

All modes; Runtime|Test; mutates.

### Behaviour.OnHotReload

`(self: ScriptInstance) -> ()`

Called after the class is replaced while instance state survives.

EditorOnly; Runtime|Test; mutates.

### Behaviour.OnLateUpdate

`(self: ScriptInstance, dt: number) -> ()`

Called for the current fixed, presentation or late presentation phase.

All modes; Runtime|Test; mutates.

### Behaviour.OnStart

`(self: ScriptInstance) -> ()`

Called at the corresponding lifecycle transition.

All modes; Runtime|Test; mutates.

### Behaviour.OnTriggerEnter

`(self: ScriptInstance, other: Entity) -> ()`

Called when the corresponding collision or trigger transition occurs.

All modes; Runtime|Test; mutates.

### Behaviour.OnTriggerEnter2D

`(self: ScriptInstance, other: Entity) -> ()`

Called when the corresponding 2D collision or trigger transition occurs.

All modes; Runtime|Test; mutates.

### Behaviour.OnTriggerExit

`(self: ScriptInstance, other: Entity) -> ()`

Called when the corresponding collision or trigger transition occurs.

All modes; Runtime|Test; mutates.

### Behaviour.OnTriggerExit2D

`(self: ScriptInstance, other: Entity) -> ()`

Called when the corresponding 2D collision or trigger transition occurs.

All modes; Runtime|Test; mutates.

### Behaviour.OnUiCancel

`(self: ScriptInstance, element: Entity) -> boolean?`

Called when Cancel is pressed, at the focused UI element (or the last canvas when none is focused); bubbles like OnUiClick.

All modes; Runtime|Test; mutates.

### Behaviour.OnUiClick

`(self: ScriptInstance, element: Entity) -> boolean?`

Called when a UI button is clicked or submitted, at its entity, then bubbling to its ancestors; return true to stop it.

All modes; Runtime|Test; mutates.

### Behaviour.OnUiFocus

`(self: ScriptInstance, element: Entity, focused: boolean) -> boolean?`

Called when a UI element gains (true) or loses (false) keyboard and gamepad focus; bubbles like OnUiClick.

All modes; Runtime|Test; mutates.

### Behaviour.OnUiHover

`(self: ScriptInstance, element: Entity, hovered: boolean) -> boolean?`

Called when the pointer enters (true) or leaves (false) an interactable UI widget; bubbles like OnUiClick.

All modes; Runtime|Test; mutates.

### Behaviour.OnUiScroll

`(self: ScriptInstance, element: Entity, position: vector) -> boolean?`

Called when interaction scrolls a UI scroll view, with its new ScrollPosition (x, y, 0); bubbles like OnUiClick.

All modes; Runtime|Test; mutates.

### Behaviour.OnUiSubmit

`(self: ScriptInstance, element: Entity, text: string) -> boolean?`

Called when Enter (or Submit) is pressed while a UI text input has focus, with its text; bubbles like OnUiClick.

All modes; Runtime|Test; mutates.

### Behaviour.OnUiTextChanged

`(self: ScriptInstance, element: Entity, text: string) -> boolean?`

Called when typing or an editing key changes a UI text input's Text, with its new text; bubbles like OnUiClick.

All modes; Runtime|Test; mutates.

### Behaviour.OnUiValueChanged

`(self: ScriptInstance, element: Entity, value: boolean | number) -> boolean?`

Called when interaction changes a UI toggle's IsOn (a boolean) or a UI slider's Value (a number); bubbles like OnUiClick.

All modes; Runtime|Test; mutates.

### Behaviour.OnUpdate

`(self: ScriptInstance, dt: number) -> ()`

Called for the current fixed, presentation or late presentation phase.

All modes; Runtime|Test; mutates.

## BoxCollider

A box-shaped collider in the entity's space.

### BoxCollider.HalfExtents

`vector`

Half the box's size along each local axis, in metres; at least 1 mm.

All modes; Runtime|Test; pure/read; setter mutates.

### BoxCollider.IsTrigger

`boolean`

Whether the collider only detects overlaps instead of colliding.

All modes; Runtime|Test; pure/read; setter mutates.

### BoxCollider.Offset

`vector`

The box centre relative to the entity, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### BoxCollider.Rotation

`Quat`

The box rotation relative to the entity: a unit quaternion.

All modes; Runtime|Test; pure/read; setter mutates.

## BoxCollider2D

A rectangle collider in the entity's XY plane.

### BoxCollider2D.Angle

`number`

The rectangle's rotation about the entity's Z axis, counter-clockwise, in degrees.

All modes; Runtime|Test; pure/read; setter mutates.

### BoxCollider2D.HalfExtents

`vector`

Half the rectangle's size along local X and Y; at least 1 mm.

All modes; Runtime|Test; pure/read; setter mutates.

### BoxCollider2D.IsTrigger

`boolean`

Whether the collider only detects overlaps instead of colliding.

All modes; Runtime|Test; pure/read; setter mutates.

### BoxCollider2D.Offset

`vector`

The rectangle's centre relative to the entity, in its XY plane.

All modes; Runtime|Test; pure/read; setter mutates.

## Camera

Renders the scene from the entity, looking down its local -Z axis.

### Camera.Clear

`ClearMode`

What the camera draws behind the scene.

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.ClearColor

`Color`

The linear background colour when Clear is Color.

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.FarClip

`number`

The far clipping distance, in metres; beyond it nothing is drawn or culled in.

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.NearClip

`number`

The near clipping distance, in metres; below FarClip.

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.OrthographicSize

`number`

Half the visible height of the orthographic projection, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.Primary

`boolean`

Whether this is the scene's primary camera (exactly one active camera should be).

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.Projection

`ProjectionType`

Perspective or orthographic projection.

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.ScreenToWorldRay

`(self: Camera, x: number, y: number) -> (vector, vector)`

Ray through a viewport pixel center, origin top-left, using the rendered camera pose; under a PixelPerfectCamera through its snapped reference view and the output mapping.

All modes; Runtime|Test; pure/read.

### Camera.VerticalFov

`number`

The vertical field of view of the perspective projection, in degrees.

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.WorldToScreen

`(self: Camera, point: vector) -> vector`

Project to top-left pixel coordinates and reverse-Z depth using the rendered camera pose; under a PixelPerfectCamera through its snapped reference view and the output mapping.

All modes; Runtime|Test; pure/read.

## CapsuleCollider

A capsule-shaped collider along the local Y axis.

### CapsuleCollider.HalfHeight

`number`

Half the height of the cylinder between the hemispheres, in metres; at least 1 mm.

All modes; Runtime|Test; pure/read; setter mutates.

### CapsuleCollider.IsTrigger

`boolean`

Whether the collider only detects overlaps instead of colliding.

All modes; Runtime|Test; pure/read; setter mutates.

### CapsuleCollider.Offset

`vector`

The capsule centre relative to the entity, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### CapsuleCollider.Radius

`number`

The radius of the capsule's hemispheres and cylinder, in metres; at least 1 mm.

All modes; Runtime|Test; pure/read; setter mutates.

### CapsuleCollider.Rotation

`Quat`

The capsule rotation relative to the entity: a unit quaternion.

All modes; Runtime|Test; pure/read; setter mutates.

## CharacterController

Moves the entity as a walking character that climbs steps and slopes and collides with the world.

### CharacterController.GetGroundNormal

`(self: CharacterController) -> vector`

Ground normal from the last update.

All modes; Runtime|Test; pure/read.

### CharacterController.GetVelocity

`(self: CharacterController) -> vector`

Velocity after the last update.

All modes; Runtime|Test; pure/read.

### CharacterController.GravityFactor

`number`

The multiplier of the project's gravity for the character.

All modes; Runtime|Test; pure/read; setter mutates.

### CharacterController.Height

`number`

The character's total height, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### CharacterController.InheritGroundVelocity

`boolean`

While grounded, the character also moves with the horizontal velocity of what it stands on, so a moving platform carries it.

All modes; Runtime|Test; pure/read; setter mutates.

### CharacterController.IsGrounded

`(self: CharacterController) -> boolean`

Whether the last character update found supporting ground.

All modes; Runtime|Test; pure/read.

### CharacterController.Layer

`string`

Physics layer name from project settings. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### CharacterController.Mass

`number`

The character's mass in kilograms, used when it pushes bodies.

All modes; Runtime|Test; pure/read; setter mutates.

### CharacterController.MaxSlopeAngle

`number`

The steepest slope the character can walk up, in degrees.

All modes; Runtime|Test; pure/read; setter mutates.

### CharacterController.Move

`(self: CharacterController, velocity: vector) -> ()`

Supply desired velocity for the next fixed character update.

All modes; Runtime|Test; mutates.

### CharacterController.Radius

`number`

The radius of the character's capsule, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### CharacterController.StepHeight

`number`

The highest step the character climbs without jumping, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

## CircleCollider2D

A circle collider in the entity's XY plane.

### CircleCollider2D.IsTrigger

`boolean`

Whether the collider only detects overlaps instead of colliding.

All modes; Runtime|Test; pure/read; setter mutates.

### CircleCollider2D.Offset

`vector`

The circle's centre relative to the entity, in its XY plane.

All modes; Runtime|Test; pure/read; setter mutates.

### CircleCollider2D.Radius

`number`

The circle's radius; at least 1 mm.

All modes; Runtime|Test; pure/read; setter mutates.

## Color

A local linear RGBA float value. Channels are finite and unclamped, allowing HDR values.

### Color.FromHex

`(hex: string) -> Color`

Reads #RRGGBB or #RRGGBBAA, case-insensitively, as linear channel bytes divided by 255; absent alpha is one.

All modes; LoadTime|Runtime|Test; pure/read.

### Color.Lerp

`(a: Color, b: Color, t: number) -> Color`

Interpolates all four linear channels with t clamped to [0, 1], producing a new color.

All modes; LoadTime|Runtime|Test; pure/read.

### Color.New

`(r: number, g: number, b: number, a: number?) -> Color`

Creates a linear color; alpha defaults to one. Channels must fit finite engine floats.

All modes; LoadTime|Runtime|Test; pure/read.

### Color.__eq

`(self: Color, other: Color) -> boolean`

Compares the four channels exactly, like vector equality.

All modes; LoadTime|Runtime|Test; pure/read.

### Color.a

`number`

The local alpha channel, finite and unclamped.

All modes; LoadTime|Runtime|Test; pure/read; local setter.

### Color.b

`number`

The local linear blue channel, finite and unclamped.

All modes; LoadTime|Runtime|Test; pure/read; local setter.

### Color.g

`number`

The local linear green channel, finite and unclamped.

All modes; LoadTime|Runtime|Test; pure/read; local setter.

### Color.r

`number`

The local linear red channel, finite and unclamped.

All modes; LoadTime|Runtime|Test; pure/read; local setter.

## DirectionalLight

A sun-like light shining down the entity's local -Z axis.

### DirectionalLight.CascadeCount

`number`

The number of shadow cascades, from 1 to 4.

All modes; Runtime|Test; pure/read; setter mutates.

### DirectionalLight.CascadeSplitLambda

`number`

The cascade split blend from uniform (0) to logarithmic (1).

All modes; Runtime|Test; pure/read; setter mutates.

### DirectionalLight.CastShadows

`boolean`

Whether the light casts cascaded soft shadows.

All modes; Runtime|Test; pure/read; setter mutates.

### DirectionalLight.Color

`Color`

The linear light colour.

All modes; Runtime|Test; pure/read; setter mutates.

### DirectionalLight.DepthBias

`number`

The shadow depth bias, in shadow-map texels.

All modes; Runtime|Test; pure/read; setter mutates.

### DirectionalLight.Intensity

`number`

The unitless brightness multiplied with Color.

All modes; Runtime|Test; pure/read; setter mutates.

### DirectionalLight.LightAngle

`number`

The angular diameter of the light source, in degrees; larger values soften shadow penumbrae.

All modes; Runtime|Test; pure/read; setter mutates.

### DirectionalLight.NormalBias

`number`

The shadow normal-offset bias, in shadow-map texels.

All modes; Runtime|Test; pure/read; setter mutates.

### DirectionalLight.ShadowDistance

`number`

How far from the camera shadows reach, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

## Entity

A UUID and scene generation identifying an entity; no native address is exposed. After a Destroy, the entity's reads keep working until the destruction flush, while IsValid is false and its writes raise.

### Entity.AddComponent

`(self: Entity, name: string, values: {[string]: any}?) -> any`

Validate and add a component and return its proxy; Script instances are synchronized before returning, and nil is returned when the new Script's OnCreate destroyed the entity. Each value holds at most 4096 bytes of text (strings, map keys and Variant text together).

All modes; Runtime|Test; mutates.

### Entity.AddTag

`(self: Entity, tag: string) -> ()`

Add a nonempty tag of at most 256 bytes if absent; an entity carries at most 256 tags.

All modes; Runtime|Test; mutates.

### Entity.Animator

`Animator?`

The entity's Animator component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.AudioListener

`AudioListener?`

The entity's AudioListener component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.AudioSource

`AudioSource?`

The entity's AudioSource component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.BoxCollider

`BoxCollider?`

The entity's BoxCollider component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.BoxCollider2D

`BoxCollider2D?`

The entity's BoxCollider2D component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Camera

`Camera?`

The entity's Camera component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.CapsuleCollider

`CapsuleCollider?`

The entity's CapsuleCollider component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.CharacterController

`CharacterController?`

The entity's CharacterController component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.CircleCollider2D

`CircleCollider2D?`

The entity's CircleCollider2D component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Destroy

`(self: Entity) -> ()`

Mark this subtree for the normal destruction flush.

All modes; Runtime|Test; mutates.

### Entity.DirectionalLight

`DirectionalLight?`

The entity's DirectionalLight component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Environment

`Environment?`

The entity's Environment component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.FindChild

`(self: Entity, name: string, recursive: boolean?) -> Entity?`

Find the first exact child name in depth-first sibling order; recursive defaults false.

All modes; Runtime|Test; pure/read.

### Entity.GetChildren

`(self: Entity) -> {Entity}`

Copy the children in sibling order.

All modes; Runtime|Test; pure/read.

### Entity.GetComponent

`((self: Entity, name: "Transform") -> Transform?) & ((self: Entity, name: "MeshRenderer") -> MeshRenderer?) & ((self: Entity, name: "Camera") -> Camera?) & ((self: Entity, name: "DirectionalLight") -> DirectionalLight?) & ((self: Entity, name: "PointLight") -> PointLight?) & ((self: Entity, name: "SpotLight") -> SpotLight?) & ((self: Entity, name: "Environment") -> Environment?) & ((self: Entity, name: "PostProcess") -> PostProcess?) & ((self: Entity, name: "Text") -> Text?) & ((self: Entity, name: "RigidBody") -> RigidBody?) & ((self: Entity, name: "BoxCollider") -> BoxCollider?) & ((self: Entity, name: "SphereCollider") -> SphereCollider?) & ((self: Entity, name: "CapsuleCollider") -> CapsuleCollider?) & ((self: Entity, name: "MeshCollider") -> MeshCollider?) & ((self: Entity, name: "CharacterController") -> CharacterController?) & ((self: Entity, name: "AudioSource") -> AudioSource?) & ((self: Entity, name: "AudioListener") -> AudioListener?) & ((self: Entity, name: "Script") -> Script?) & ((self: Entity, name: "SpriteRenderer") -> SpriteRenderer?) & ((self: Entity, name: "SpriteAnimator") -> SpriteAnimator?) & ((self: Entity, name: "PixelPerfectCamera") -> PixelPerfectCamera?) & ((self: Entity, name: "Light2D") -> Light2D?) & ((self: Entity, name: "TilemapRenderer") -> TilemapRenderer?) & ((self: Entity, name: "RigidBody2D") -> RigidBody2D?) & ((self: Entity, name: "BoxCollider2D") -> BoxCollider2D?) & ((self: Entity, name: "CircleCollider2D") -> CircleCollider2D?) & ((self: Entity, name: "PolygonCollider2D") -> PolygonCollider2D?) & ((self: Entity, name: "TilemapCollider") -> TilemapCollider?) & ((self: Entity, name: "UiCanvas") -> UiCanvas?) & ((self: Entity, name: "UiRect") -> UiRect?) & ((self: Entity, name: "UiImage") -> UiImage?) & ((self: Entity, name: "UiText") -> UiText?) & ((self: Entity, name: "UiLayout") -> UiLayout?) & ((self: Entity, name: "UiLayoutElement") -> UiLayoutElement?) & ((self: Entity, name: "UiButton") -> UiButton?) & ((self: Entity, name: "UiToggle") -> UiToggle?) & ((self: Entity, name: "UiSlider") -> UiSlider?) & ((self: Entity, name: "UiScrollView") -> UiScrollView?) & ((self: Entity, name: "UiTextInput") -> UiTextInput?) & ((self: Entity, name: "Animator") -> Animator?) & ((self: Entity, name: "SkinnedMesh") -> SkinnedMesh?) & ((self: Entity, name: "ParticleEmitter") -> ParticleEmitter?) & ((self: Entity, name: "IrradianceVolume") -> IrradianceVolume?) & ((self: Entity, name: "ReflectionProbe") -> ReflectionProbe?) & ((self: Entity, name: "Terrain") -> Terrain?) & ((self: Entity, name: "TerrainCollider") -> TerrainCollider?)`

Get a component proxy, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.GetParent

`(self: Entity) -> Entity?`

Get the parent, or nil for a root.

All modes; Runtime|Test; pure/read.

### Entity.GetScript

`(self: Entity, name: string?) -> any`

Get the instance of the first script slot whose behaviour has this name, or of the first slot without a name; nil when there is none.

All modes; Runtime|Test; pure/read.

### Entity.GetScripts

`(self: Entity) -> {any}`

Get the live instances of the entity's script slots, in slot order.

All modes; Runtime|Test; pure/read.

### Entity.GetTags

`(self: Entity) -> {string}`

Copy tags in insertion order.

All modes; Runtime|Test; pure/read.

### Entity.GetWorldBounds

`(self: Entity) -> (vector?, vector?)`

Get the world AABB of what this entity draws: its render mesh, its sprite's quad and its tilemap's drawn tiles; nil without any of them.

All modes; Runtime|Test; pure/read.

### Entity.HasComponent

`(self: Entity, name: string) -> boolean`

Check an exposed component by its exact registry name.

All modes; Runtime|Test; pure/read.

### Entity.HasTag

`(self: Entity, tag: string) -> boolean`

Check an exact tag.

All modes; Runtime|Test; pure/read.

### Entity.ID

`string`

The immutable 16-digit hexadecimal UUID.

All modes; Runtime|Test; pure/read.

### Entity.IrradianceVolume

`IrradianceVolume?`

The entity's IrradianceVolume component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.IsActive

`(self: Entity) -> boolean`

The effective active state including every ancestor.

All modes; Runtime|Test; pure/read.

### Entity.IsActiveSelf

`(self: Entity) -> boolean`

The entity's local active state.

All modes; Runtime|Test; pure/read.

### Entity.IsValid

`(self: Entity) -> boolean`

Whether this identity still names a live entity in this scene.

All modes; Runtime|Test; pure/read.

### Entity.Light2D

`Light2D?`

The entity's Light2D component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.MeshCollider

`MeshCollider?`

The entity's MeshCollider component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.MeshRenderer

`MeshRenderer?`

The entity's MeshRenderer component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Name

`string`

The entity name; a written name is UTF-8 without NUL and at most 256 bytes.

All modes; Runtime|Test; pure/read; setter mutates.

### Entity.ParticleEmitter

`ParticleEmitter?`

The entity's ParticleEmitter component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.PixelPerfectCamera

`PixelPerfectCamera?`

The entity's PixelPerfectCamera component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.PointLight

`PointLight?`

The entity's PointLight component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.PolygonCollider2D

`PolygonCollider2D?`

The entity's PolygonCollider2D component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.PostProcess

`PostProcess?`

The entity's PostProcess component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.ReflectionProbe

`ReflectionProbe?`

The entity's ReflectionProbe component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.RemoveComponent

`(self: Entity, name: string) -> ()`

Remove a removable component when no present component requires it.

All modes; Runtime|Test; mutates.

### Entity.RemoveTag

`(self: Entity, tag: string) -> ()`

Remove a tag if present.

All modes; Runtime|Test; mutates.

### Entity.RigidBody

`RigidBody?`

The entity's RigidBody component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.RigidBody2D

`RigidBody2D?`

The entity's RigidBody2D component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.SetActive

`(self: Entity, active: boolean) -> ()`

Change local active state and dispatch effective lifecycle transitions before returning.

All modes; Runtime|Test; mutates.

### Entity.SetParent

`(self: Entity, parent: Entity?, keepWorld: boolean?) -> ()`

Reparent to the last sibling, preserving world pose by default; synchronize effective lifecycle transitions.

All modes; Runtime|Test; mutates.

### Entity.SkinnedMesh

`SkinnedMesh?`

The entity's SkinnedMesh component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.SphereCollider

`SphereCollider?`

The entity's SphereCollider component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.SpotLight

`SpotLight?`

The entity's SpotLight component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.SpriteAnimator

`SpriteAnimator?`

The entity's SpriteAnimator component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.SpriteRenderer

`SpriteRenderer?`

The entity's SpriteRenderer component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Terrain

`Terrain?`

The entity's Terrain component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.TerrainCollider

`TerrainCollider?`

The entity's TerrainCollider component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Text

`Text?`

The entity's Text component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.TilemapCollider

`TilemapCollider?`

The entity's TilemapCollider component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.TilemapRenderer

`TilemapRenderer?`

The entity's TilemapRenderer component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Transform

`Transform`

The entity's Transform component, which every entity has.

All modes; Runtime|Test; pure/read.

### Entity.UiButton

`UiButton?`

The entity's UiButton component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiCanvas

`UiCanvas?`

The entity's UiCanvas component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiImage

`UiImage?`

The entity's UiImage component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiLayout

`UiLayout?`

The entity's UiLayout component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiLayoutElement

`UiLayoutElement?`

The entity's UiLayoutElement component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiRect

`UiRect?`

The entity's UiRect component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiScrollView

`UiScrollView?`

The entity's UiScrollView component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiSlider

`UiSlider?`

The entity's UiSlider component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiText

`UiText?`

The entity's UiText component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiTextInput

`UiTextInput?`

The entity's UiTextInput component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.UiToggle

`UiToggle?`

The entity's UiToggle component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.__eq

`(self: Entity, other: Entity) -> boolean`

Compare UUIDs; equality does not authorize access to an expired scene.

All modes; Runtime|Test; pure/read.

## Environment

The scene's image-based lighting and skybox (one per scene).

### Environment.Environment

`AssetRef?`

The environment map; null lights the scene with FallbackColor.

All modes; Runtime|Test; pure/read; setter mutates.

### Environment.FallbackColor

`Color`

The linear ambient colour used when no environment map is assigned.

All modes; Runtime|Test; pure/read; setter mutates.

### Environment.Intensity

`number`

The unitless strength of the image-based lighting.

All modes; Runtime|Test; pure/read; setter mutates.

### Environment.Rotation

`number`

The rotation of the environment about the +Y axis, in degrees.

All modes; Runtime|Test; pure/read; setter mutates.

### Environment.ShowSkybox

`boolean`

Whether cameras that clear to the skybox show the environment map.

All modes; Runtime|Test; pure/read; setter mutates.

### Environment.SkyboxBlur

`number`

How blurred the visible skybox is, from sharp (0) to fully blurred (1).

All modes; Runtime|Test; pure/read; setter mutates.

## FieldDescriptor

An authenticated immutable Field constructor descriptor.

## IrradianceVolume

A box of baked irradiance probes that lights the diffuse surfaces inside it with the light their surroundings bounce; lighting.bake bakes it.

### IrradianceVolume.BlendDistance

`number`

The distance inside the box's faces over which its light fades to the Environment's, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### IrradianceVolume.Data

`AssetRef?`

The baked volume that lighting.bake writes beside the scene and assigns; null (not baked) lights nothing.

All modes; Runtime|Test; pure/read; setter mutates.

### IrradianceVolume.Intensity

`number`

The unitless strength of the baked indirect light.

All modes; Runtime|Test; pure/read; setter mutates.

### IrradianceVolume.ProbeCountX

`number`

The probes along the box's local X axis, the outer ones on its faces.

All modes; Runtime|Test; pure/read; setter mutates.

### IrradianceVolume.ProbeCountY

`number`

The probes along the box's local Y axis, the outer ones on its faces.

All modes; Runtime|Test; pure/read; setter mutates.

### IrradianceVolume.ProbeCountZ

`number`

The probes along the box's local Z axis, the outer ones on its faces.

All modes; Runtime|Test; pure/read; setter mutates.

### IrradianceVolume.Size

`vector`

The box's size in metres, centred on the entity in its local axes.

All modes; Runtime|Test; pure/read; setter mutates.

## Light2D

A 2D light: lights the lit sprites and tilemaps of the view, never unlit ones; no shadows.

### Light2D.Color

`Color`

The light's linear colour.

All modes; Runtime|Test; pure/read; setter mutates.

### Light2D.Falloff

`number`

Point: the fraction of the radius, from its edge inward, over which the light fades smoothly to nothing; 0 is a hard edge.

All modes; Runtime|Test; pure/read; setter mutates.

### Light2D.Height

`number`

Point: how far above the lit plane the light sits, in world units; only normal-mapped sprites read it.

All modes; Runtime|Test; pure/read; setter mutates.

### Light2D.Intensity

`number`

How strong the light is: its colour is multiplied by this, so values above 1 overbright.

All modes; Runtime|Test; pure/read; setter mutates.

### Light2D.Radius

`number`

Point: how far the light reaches from the entity, in world units.

All modes; Runtime|Test; pure/read; setter mutates.

### Light2D.Type

`Light2DType`

Whether the light is a point light around the entity or a global light of the whole view.

All modes; Runtime|Test; pure/read; setter mutates.

## MeshCollider

A collider shaped like a mesh: its convex hull, or the exact triangles for Static and Kinematic bodies.

### MeshCollider.Convex

`boolean`

Whether the convex hull is used; a non-convex mesh collider needs a Static or Kinematic body.

All modes; Runtime|Test; pure/read; setter mutates.

### MeshCollider.IsTrigger

`boolean`

Whether the collider only detects overlaps instead of colliding.

All modes; Runtime|Test; pure/read; setter mutates.

### MeshCollider.Mesh

`AssetRef?`

The collision mesh; null uses the entity's MeshRenderer mesh.

All modes; Runtime|Test; pure/read; setter mutates.

## MeshRenderer

Draws a mesh asset with one material per submesh.

### MeshRenderer.CastShadows

`boolean`

Whether the mesh casts shadows.

All modes; Runtime|Test; pure/read; setter mutates.

### MeshRenderer.GetMaterial

`(self: MeshRenderer, index: number) -> AssetRef?`

Read a 1-based material slot, resolving an absent or null override to the mesh default.

All modes; Runtime|Test; pure/read.

### MeshRenderer.Materials

`{AssetRef?}`

One material per submesh; an empty list or a null slot uses the mesh's default material for that submesh.

All modes; Runtime|Test; pure/read; setter mutates.

### MeshRenderer.Mesh

`AssetRef?`

The mesh to draw; null draws nothing.

All modes; Runtime|Test; pure/read; setter mutates.

### MeshRenderer.ReceiveShadows

`boolean`

Whether shadows fall on the mesh.

All modes; Runtime|Test; pure/read; setter mutates.

### MeshRenderer.SetMaterial

`(self: MeshRenderer, index: number, asset: AssetRef?) -> ()`

Override an existing 1-based mesh or explicit material slot; nil restores the mesh default.

All modes; Runtime|Test; mutates.

### MeshRenderer.Visible

`boolean`

Whether the mesh is drawn at all.

All modes; Runtime|Test; pure/read; setter mutates.

## ParticleEmitter

Emits camera-facing textured particles from the entity: a presentation-only, seeded simulation that never changes the state hash and has no gameplay collision.

### ParticleEmitter.Acceleration

`vector`

The acceleration of every live particle, in metres per second squared, in the SimulationSpace (set (0, -9.81, 0) for gravity).

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Blend

`ParticleBlend`

How particles are blended into the image.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.BoxHalfExtents

`vector`

The Box shape's half extents, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Burst

`number`

Particles emitted at once at the start of each cycle, at most MaxParticles.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Clear

`(self: ParticleEmitter) -> ()`

Kill every live particle and drop the queued emits; the emission cycle and the random stream go on.

All modes; Runtime|Test; mutates.

### ParticleEmitter.ColorOverLife

`{{Time: number, Color: Color}}`

At most eight colour keys over a particle's life, in ascending Time, interpolated linearly; empty keeps particles white and opaque.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.ConeAngle

`number`

The Cone shape's half angle from the local -Z axis, in degrees.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Drag

`number`

How quickly particles slow down, per second.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Duration

`number`

The length of one emission cycle, in seconds; Burst is emitted at each cycle's start.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Emit

`(self: ParticleEmitter, count: number) -> ()`

Queue count particles (1 to 4096) for the next Particles phase, whether the emitter is Playing, looping or finished; an emitter without particles yet starts at that phase. A count outside its range raises an error. Particles are presentation, so the simulated state does not change.

All modes; Runtime|Test; mutates.

### ParticleEmitter.GetParticleCount

`(self: ParticleEmitter) -> number`

Read the live particles after the last Particles phase (0 before the first). Particles are presentation state the frame phase advances, so the count depends on frame pacing: it raises an error in a fixed-step callback (OnFixedUpdate, a task, a physics, animation or UI event) and is read in OnUpdate, OnLateUpdate or a test case; never let it drive gameplay.

All modes; Runtime|Test; pure/read.

### ParticleEmitter.Lifetime

`vector`

The range a new particle's lifetime is drawn from, in seconds: (minimum, maximum).

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Loop

`boolean`

Whether emission repeats every Duration seconds; otherwise it stops after one cycle.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.MaxParticles

`number`

The most particles alive at once; beyond it no new particle is emitted until one dies.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Playing

`boolean`

Whether the emitter emits; live particles finish their lives either way.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Rate

`number`

Particles emitted per second while playing.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Seed

`number`

Seeds the emitter's random stream together with the entity's id, so a replay shows the same particles in every build.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Shape

`ParticleEmitterShape`

Where new particles start and which way they move.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.ShapeRadius

`number`

The radius of the Sphere shape and of the Cone shape's base disc, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.SimulationSpace

`ParticleSimulationSpace`

Whether live particles stay where they were emitted or move with the emitter.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Size

`vector`

The range a new particle's size is drawn from, in metres: (minimum, maximum).

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.SizeOverLife

`{{Time: number, Size: number}}`

At most eight size factors over a particle's life, in ascending Time, interpolated linearly; empty keeps the start size.

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Speed

`vector`

The range a new particle's speed is drawn from, in metres per second: (minimum, maximum).

All modes; Runtime|Test; pure/read; setter mutates.

### ParticleEmitter.Texture

`AssetRef?`

The particle's texture; null uses the built-in soft disc (engine://Textures/Particle).

All modes; Runtime|Test; pure/read; setter mutates.

## PixelPerfectCamera

Renders the entity's camera at a reference resolution, upscaled by the largest whole factor that fits the view in a letterbox.

### PixelPerfectCamera.LetterboxColor

`Color`

The linear colour of the bars around the upscaled image.

All modes; Runtime|Test; pure/read; setter mutates.

### PixelPerfectCamera.PixelsPerUnit

`number`

How many scene pixels make one world unit; sprites of the same PixelsPerUnit map each texel to one pixel.

All modes; Runtime|Test; pure/read; setter mutates.

### PixelPerfectCamera.ReferenceHeight

`number`

The height the scene renders at, in pixels; the orthographic view is this many pixels of PixelsPerUnit high.

All modes; Runtime|Test; pure/read; setter mutates.

### PixelPerfectCamera.ReferenceWidth

`number`

The width the scene renders at, in pixels.

All modes; Runtime|Test; pure/read; setter mutates.

## PointLight

A light shining in every direction from the entity.

### PointLight.CastShadows

`boolean`

Whether the light casts soft shadows (at most four point lights per view; the shadow budget applies).

All modes; Runtime|Test; pure/read; setter mutates.

### PointLight.Color

`Color`

The linear light colour.

All modes; Runtime|Test; pure/read; setter mutates.

### PointLight.Intensity

`number`

The unitless brightness multiplied with Color.

All modes; Runtime|Test; pure/read; setter mutates.

### PointLight.Range

`number`

The distance at which the light's falloff reaches zero, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### PointLight.SourceRadius

`number`

The radius of the emitting sphere, in metres; widens highlights and shadow penumbrae.

All modes; Runtime|Test; pure/read; setter mutates.

## PolygonCollider2D

A convex polygon collider in the entity's XY plane.

### PolygonCollider2D.Angle

`number`

The polygon's rotation about the entity's Z axis, counter-clockwise, in degrees.

All modes; Runtime|Test; pure/read; setter mutates.

### PolygonCollider2D.IsTrigger

`boolean`

Whether the collider only detects overlaps instead of colliding.

All modes; Runtime|Test; pure/read; setter mutates.

### PolygonCollider2D.Offset

`vector`

Where the polygon's origin sits relative to the entity, in its XY plane.

All modes; Runtime|Test; pure/read; setter mutates.

### PolygonCollider2D.Points

`{vector}`

The points whose convex hull is the collider, 3 to 64 of them, in the collider's own space before Offset and Angle.

All modes; Runtime|Test; pure/read; setter mutates.

## PostProcess

The scene's exposure (manual or automatic), tone mapping and grading, ambient occlusion, reflections, bloom and anti-aliasing (one per scene).

### PostProcess.AntiAliasing

`AntiAliasing`

How edges are anti-aliased: none, FXAA (the default) or TAA.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.AutoExposureMaxEV

`number`

The darkest and brightest exposure automatic exposure may adapt to, in stops.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.AutoExposureMinEV

`number`

The darkest and brightest exposure automatic exposure may adapt to, in stops.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.AutoExposureSpeedDown

`number`

How many stops per second automatic exposure adapts towards a brighter or a darker exposure.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.AutoExposureSpeedUp

`number`

How many stops per second automatic exposure adapts towards a brighter or a darker exposure.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.BloomEnabled

`boolean`

Whether bright areas bloom.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.BloomIntensity

`number`

How much of the bloom is blended into the image.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.ExposureEV

`number`

The exposure compensation in stops: the image is multiplied by 2^EV.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.ExposureMode

`ExposureMode`

Manual exposure, or automatic exposure from the image's luminance histogram.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.Lut

`AssetRef?`

The colour-grading LUT applied after the tone mapping curve; null grades nothing.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.LutIntensity

`number`

How much of the graded colour replaces the ungraded one, from 0 to 1.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsaoEnabled

`boolean`

Whether screen-space ambient occlusion (GTAO) is applied. It darkens the environment's image-based light only (the Environment's FallbackColor ambient without a map), never direct lights.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsaoIntensity

`number`

The strength of the ambient occlusion, which darkens the environment's image-based light only, never direct lights.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsaoQuality

`SsaoQuality`

The ambient occlusion sample count.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsaoRadius

`number`

The world-space radius within which ambient occlusion is sampled, in metres. An occluder further than this in front of a surface casts little occlusion on it: set it above the depth gap between the pieces and the surface behind them.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsrEnabled

`boolean`

Whether glossy surfaces reflect what is on screen (screen-space reflections), falling back to the probes where a ray misses.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsrIntensity

`number`

How strongly screen-space reflections replace the probes' reflection, from 0 to 1.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsrMaxDistance

`number`

How far a reflected ray travels, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsrMaxRoughness

`number`

The perceptual roughness above which only the probes reflect.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsrThickness

`number`

How thick a surface is assumed behind its depth when a ray passes it, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.Tonemap

`Tonemapper`

The tone mapping curve.

All modes; Runtime|Test; pure/read; setter mutates.

## Quat

A local nonzero quaternion, stored as finite float x, y, z, w components. Angles are degrees.

### Quat.AngleAxis

`(degrees: number, axis: vector) -> Quat`

Creates a unit rotation around a nonzero axis, normalizing its length.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.FromEuler

`(x: number, y: number, z: number) -> Quat`

Creates a unit rotation from degrees, applying Z, then X, then Y, matching Transform.EulerAngles.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.Identity

`() -> Quat`

Creates the identity rotation (0, 0, 0, 1).

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.Inverse

`(self: Quat) -> Quat`

Returns the algebraic inverse, preserving reciprocal magnitude; an unrepresentable result is an error.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.LookRotation

`(forward: vector, up: vector?) -> Quat`

Points local -Z along forward and aligns +Y to projected up (default +Y). Zero or nearly parallel directions are errors.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.New

`(x: number, y: number, z: number, w: number) -> Quat`

Creates an unnormalized quaternion; at least one component must be nonzero.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.Normalized

`(self: Quat) -> Quat`

Returns a new unit quaternion with the same rotation.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.Rotate

`(self: Quat, value: vector) -> vector`

Rotates a vector using the normalized quaternion; does not scale the vector.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.Slerp

`(a: Quat, b: Quat, t: number) -> Quat`

Interpolates normalized rotations along the shortest arc, with t clamped to [0, 1].

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.ToEuler

`(self: Quat) -> vector`

Returns Z-X-Y Euler degrees with X in [-90, 90] and Y/Z in (-180, 180], matching the inspector.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.__eq

`(self: Quat, other: Quat) -> boolean`

Compares the four components exactly, like vector equality; q and -q are the same rotation but compare unequal.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.__mul

`((a: Quat, b: Quat) -> Quat) & ((rotation: Quat, value: vector) -> vector)`

Composes quaternions (right operand applied first), or rotates a vector as Rotate does.

All modes; LoadTime|Runtime|Test; pure/read.

### Quat.w

`number`

The local w component; writes preserve a nonzero finite quaternion.

All modes; LoadTime|Runtime|Test; pure/read; local setter.

### Quat.x

`number`

The local x component; writes preserve a nonzero finite quaternion.

All modes; LoadTime|Runtime|Test; pure/read; local setter.

### Quat.y

`number`

The local y component; writes preserve a nonzero finite quaternion.

All modes; LoadTime|Runtime|Test; pure/read; local setter.

### Quat.z

`number`

The local z component; writes preserve a nonzero finite quaternion.

All modes; LoadTime|Runtime|Test; pure/read; local setter.

## RandomGenerator

An independent value-owned seeded stream; its methods never change session randomness.

### RandomGenerator.Bool

`(self: RandomGenerator, probability: number?) -> boolean`

Draws a boolean with the given probability, default one half.

All modes; Runtime|Test; pure/read.

### RandomGenerator.Choice

`<T>(self: RandomGenerator, array: {T}) -> T`

Chooses from a nonempty dense array using only this stream.

All modes; Runtime|Test; pure/read.

### RandomGenerator.Integer

`(self: RandomGenerator, min: number, max: number) -> number`

Draws an inclusive bounded integer from this stream.

All modes; Runtime|Test; pure/read.

### RandomGenerator.Number

`(self: RandomGenerator, min: number?, max: number?) -> number`

Draws in [min,max); no arguments means [0,1), one means [0,max).

All modes; Runtime|Test; pure/read.

### RandomGenerator.Seed

`(self: RandomGenerator, seed: number) -> ()`

Reseeds this local generator with a nonnegative exact integer.

All modes; Runtime|Test; pure/read.

### RandomGenerator.Shuffle

`<T>(self: RandomGenerator, array: {T}) -> {T}`

Shuffles a mutable dense array in place using only this stream.

All modes; Runtime|Test; pure/read.

### RandomGenerator.UnitVector

`(self: RandomGenerator) -> vector`

Draws a uniform direction from this stream with deterministic trigonometry.

All modes; Runtime|Test; pure/read.

## ReflectionProbe

A baked reflection of the probe's surroundings that glossy surfaces inside its box reflect instead of the Environment; lighting.bake bakes it.

### ReflectionProbe.BlendDistance

`number`

The distance inside the box's faces over which its reflection fades to the Environment's, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### ReflectionProbe.BoxProjection

`boolean`

Whether reflections are corrected for the box (parallax-corrected), so a room's reflection lines up with its walls.

All modes; Runtime|Test; pure/read; setter mutates.

### ReflectionProbe.Data

`AssetRef?`

The baked reflection, an Environment that lighting.bake writes beside the scene and assigns; null (not baked) reflects nothing.

All modes; Runtime|Test; pure/read; setter mutates.

### ReflectionProbe.Intensity

`number`

The unitless strength of the reflection.

All modes; Runtime|Test; pure/read; setter mutates.

### ReflectionProbe.Resolution

`number`

The bake's capture cube face, in texels: a power of two.

All modes; Runtime|Test; pure/read; setter mutates.

### ReflectionProbe.Size

`vector`

The box whose surfaces reflect the probe, in metres, centred on the entity in its local axes.

All modes; Runtime|Test; pure/read; setter mutates.

## RigidBody

Simulates the entity with Jolt physics.

### RigidBody.AddAngularImpulse

`(self: RigidBody, impulse: vector) -> ()`

Apply an angular impulse to a Dynamic body.

All modes; Runtime|Test; mutates.

### RigidBody.AddForce

`(self: RigidBody, force: vector) -> ()`

Apply force to a Dynamic body.

All modes; Runtime|Test; mutates.

### RigidBody.AddForceAtPosition

`(self: RigidBody, force: vector, position: vector) -> ()`

Apply force at a world position to a Dynamic body.

All modes; Runtime|Test; mutates.

### RigidBody.AddImpulse

`(self: RigidBody, impulse: vector) -> ()`

Apply an impulse to a Dynamic body.

All modes; Runtime|Test; mutates.

### RigidBody.AddTorque

`(self: RigidBody, torque: vector) -> ()`

Apply torque to a Dynamic body.

All modes; Runtime|Test; mutates.

### RigidBody.AllowSleeping

`boolean`

Whether the body may sleep when it comes to rest.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.AngularDamping

`number`

How quickly angular velocity decays, per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.EnhancedInternalEdgeRemoval

`boolean`

Removes ghost collisions with internal edges of compound shapes; set it on rolling bodies.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.Friction

`number`

The friction coefficient of the body's surfaces. A contact uses the geometric mean of both bodies' values, so 0 on one body removes friction from all its contacts.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.GetAngularVelocity

`(self: RigidBody) -> vector`

Read the angular velocity the last physics step left, in radians per second; a Kinematic body's follows a MoveKinematic as GetLinearVelocity's does.

All modes; Runtime|Test; pure/read.

### RigidBody.GetLinearVelocity

`(self: RigidBody) -> vector`

Read the linear velocity the last physics step left. A Kinematic body's is that of the sweep the step made to its Transform's pose, so a MoveKinematic shows here only once the next physics step has swept the body: the fixed step that called it (OnFixedUpdate, tasks, test cases) still reads the previous step's.

All modes; Runtime|Test; pure/read.

### RigidBody.GravityFactor

`number`

The multiplier of the project's gravity for this body (0 floats).

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.InitialAngularVelocity

`vector`

The world-space angular velocity the body starts with, in radians per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.InitialLinearVelocity

`vector`

The world-space velocity the body starts with, in metres per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.IsSleeping

`(self: RigidBody) -> boolean`

Whether the body is sleeping.

All modes; Runtime|Test; pure/read.

### RigidBody.Layer

`string`

Physics layer name from project settings. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.LinearDamping

`number`

How quickly linear velocity decays, per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.LockRotation

`{boolean}`

Per world axis: whether the body cannot rotate about it.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.LockTranslation

`{boolean}`

Per world axis: whether the body cannot move along it.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.Mass

`number`

Mass in kilograms (Dynamic only).

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.MaxAngularVelocity

`number`

The angular speed the body is clamped to, in radians per second; raise it for fast rolling balls.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.MaxLinearVelocity

`number`

The speed the body is clamped to, in metres per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.MotionQuality

`MotionQuality`

Discrete or continuous (LinearCast) collision detection.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.MoveKinematic

`(self: RigidBody, position: vector, rotation: Quat) -> ()`

Set a Kinematic target: the entity's world pose changes at once, and the next physics pre-step (the same tick's when called from OnFixedUpdate, a task or a test case) sweeps the body there in one fixed step, with the velocities that take it there.

All modes; Runtime|Test; mutates.

### RigidBody.Restitution

`number`

Bounciness, from no bounce (0) to a perfectly elastic bounce (1). A contact uses the larger of both bodies' values, so 1 on a ball makes it bounce elastically off every surface.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.SetAngularVelocity

`(self: RigidBody, velocity: vector) -> ()`

Set a Dynamic body's angular velocity.

All modes; Runtime|Test; mutates.

### RigidBody.SetLinearVelocity

`(self: RigidBody, velocity: vector) -> ()`

Set a Dynamic body's linear velocity.

All modes; Runtime|Test; mutates.

### RigidBody.Teleport

`(self: RigidBody, position: vector, rotation: Quat?) -> ()`

Place a body immediately, preserving velocity and resetting interpolation.

All modes; Runtime|Test; mutates.

### RigidBody.Type

`BodyType`

Static never moves; Kinematic is moved by script; Dynamic is simulated.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.WakeUp

`(self: RigidBody) -> ()`

Wake a sleeping body.

All modes; Runtime|Test; mutates.

## RigidBody2D

Simulates the entity in the 2D world: moves along world X and Y and rotates about world Z.

### RigidBody2D.AddAngularImpulse

`(self: RigidBody2D, impulse: number) -> ()`

Apply an angular impulse about +Z to a Dynamic 2D body.

All modes; Runtime|Test; mutates.

### RigidBody2D.AddForce

`(self: RigidBody2D, force: vector) -> ()`

Apply a force in the XY plane to a Dynamic 2D body.

All modes; Runtime|Test; mutates.

### RigidBody2D.AddForceAtPosition

`(self: RigidBody2D, force: vector, position: vector) -> ()`

Apply a force in the XY plane at a world position of the plane to a Dynamic 2D body.

All modes; Runtime|Test; mutates.

### RigidBody2D.AddImpulse

`(self: RigidBody2D, impulse: vector) -> ()`

Apply an impulse in the XY plane to a Dynamic 2D body.

All modes; Runtime|Test; mutates.

### RigidBody2D.AddTorque

`(self: RigidBody2D, torque: number) -> ()`

Apply a torque about +Z, in newton metres, to a Dynamic 2D body.

All modes; Runtime|Test; mutates.

### RigidBody2D.AllowSleeping

`boolean`

Whether the body may sleep when it comes to rest.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.AngularDamping

`number`

How quickly angular velocity decays, per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.EnhancedInternalEdgeRemoval

`boolean`

Removes ghost collisions with internal edges of compound shapes; set it on bodies that roll or slide across tiles.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.FixedRotation

`boolean`

Whether a Dynamic body is kept from rotating.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.Friction

`number`

The friction coefficient of the body's surfaces; a contact uses the geometric mean of both bodies' values.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.GetAngularVelocity

`(self: RigidBody2D) -> number`

Read the angular velocity about +Z in radians per second.

All modes; Runtime|Test; pure/read.

### RigidBody2D.GetLinearVelocity

`(self: RigidBody2D) -> vector`

Read the linear velocity in the XY plane (Z is 0).

All modes; Runtime|Test; pure/read.

### RigidBody2D.GravityFactor

`number`

The multiplier of the project's 2D gravity for this body (0 floats).

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.InitialAngularVelocity

`number`

The angular velocity about world Z the body starts with, in radians per second, counter-clockwise seen from +Z.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.InitialLinearVelocity

`vector`

The velocity along world X and Y the body starts with, in metres per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.IsSleeping

`(self: RigidBody2D) -> boolean`

Whether the 2D body is sleeping.

All modes; Runtime|Test; pure/read.

### RigidBody2D.Layer

`string`

Physics layer name from project settings (the 3D world's layers). A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.LinearDamping

`number`

How quickly linear velocity decays, per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.Mass

`number`

Mass in kilograms (Dynamic only).

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.MaxAngularVelocity

`number`

The angular speed the body is clamped to, in radians per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.MaxLinearVelocity

`number`

The speed the body is clamped to, in metres per second.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.MotionQuality

`MotionQuality`

Discrete or continuous (LinearCast) collision detection.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.MoveKinematic

`(self: RigidBody2D, position: vector, angle: number) -> ()`

Set a Kinematic 2D target in the plane, the angle in degrees about +Z, for the next fixed step.

All modes; Runtime|Test; mutates.

### RigidBody2D.Restitution

`number`

Bounciness, from no bounce (0) to a perfectly elastic bounce (1); a contact uses the larger of both bodies' values.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.SetAngularVelocity

`(self: RigidBody2D, velocity: number) -> ()`

Set a Dynamic 2D body's angular velocity about +Z in radians per second.

All modes; Runtime|Test; mutates.

### RigidBody2D.SetLinearVelocity

`(self: RigidBody2D, velocity: vector) -> ()`

Set a Dynamic 2D body's linear velocity from the vector's X and Y.

All modes; Runtime|Test; mutates.

### RigidBody2D.Teleport

`(self: RigidBody2D, position: vector, angle: number?) -> ()`

Place a 2D body at once in the plane (the entity keeps its Z), the angle in degrees about +Z kept when omitted, preserving velocity and resetting interpolation.

All modes; Runtime|Test; mutates.

### RigidBody2D.Type

`BodyType`

Static never moves; Kinematic is moved by script; Dynamic is simulated.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody2D.WakeUp

`(self: RigidBody2D) -> ()`

Wake a sleeping 2D body.

All modes; Runtime|Test; mutates.

## Script (component)

Attaches Behaviour scripts to the entity, an ordered list of slots.

### Script.Scripts

`{{Script: AssetRef?, Fields: {[string]: any}, ExecutionOrder: number}}`

The behaviours of the entity, in slot order; each slot is its own instance. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

## SkinnedMesh

Deforms the entity's MeshRenderer mesh: its skin follows the nearest Animator on the entity or above it, and its morph targets take weights.

### SkinnedMesh.MorphWeights

`{number}`

One weight in [-1, 1] per morph target of the mesh, in order; a missing weight is the target's default, and an animated weight overrides it.

All modes; Runtime|Test; pure/read; setter mutates.

### SkinnedMesh.SetMorphWeight

`(self: SkinnedMesh, target: string, weight: number) -> ()`

Set the weight of the mesh's morph target named target, growing MorphWeights with the targets' default weights; an error names the target when the mesh has no such target, and a weight outside [-1, 1] raises one.

All modes; Runtime|Test; mutates.

## SphereCollider

A sphere-shaped collider in the entity's space.

### SphereCollider.IsTrigger

`boolean`

Whether the collider only detects overlaps instead of colliding.

All modes; Runtime|Test; pure/read; setter mutates.

### SphereCollider.Offset

`vector`

The sphere centre relative to the entity, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### SphereCollider.Radius

`number`

The sphere radius, in metres; at least 1 mm.

All modes; Runtime|Test; pure/read; setter mutates.

## SpotLight

A cone of light shining down the entity's local -Z axis.

### SpotLight.CastShadows

`boolean`

Whether the light casts soft shadows (shadow atlas budget applies).

All modes; Runtime|Test; pure/read; setter mutates.

### SpotLight.Color

`Color`

The linear light colour.

All modes; Runtime|Test; pure/read; setter mutates.

### SpotLight.InnerConeAngle

`number`

The angle from the axis inside which the light is at full strength, in degrees; below OuterConeAngle.

All modes; Runtime|Test; pure/read; setter mutates.

### SpotLight.Intensity

`number`

The unitless brightness multiplied with Color.

All modes; Runtime|Test; pure/read; setter mutates.

### SpotLight.OuterConeAngle

`number`

The angle from the axis at which the light fades to zero, in degrees.

All modes; Runtime|Test; pure/read; setter mutates.

### SpotLight.Range

`number`

The distance at which the light's falloff reaches zero, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### SpotLight.SourceRadius

`number`

The radius of the emitting disc, in metres; widens highlights and shadow penumbrae.

All modes; Runtime|Test; pure/read; setter mutates.

## SpriteAnimator

Plays a clip of the SpriteRenderer's sprite sheet, one step per clip frame for its ticks, driven by the fixed step.

### SpriteAnimator.Clip

`string`

The name of the sheet's clip to play; empty plays nothing. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteAnimator.Play

`(self: SpriteAnimator, clip: string) -> ()`

Play the sheet's clip named clip from its first step, which shows at the next fixed step's SpriteAnimation phase.

All modes; Runtime|Test; mutates.

### SpriteAnimator.Playing

`boolean`

Whether the clip advances each fixed step; a clip without Loop clears it after its last step and holds its last frame.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteAnimator.Step

`number`

The index of the clip step shown.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteAnimator.StepTicks

`number`

How many fixed steps the current clip step has been shown.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteAnimator.Stop

`(self: SpriteAnimator) -> ()`

Stop advancing the clip; the frame shown stays.

All modes; Runtime|Test; mutates.

## SpriteRenderer

Draws a sprite: a sprite sheet frame or a texture in the entity's XY plane.

### SpriteRenderer.Color

`Color`

The linear colour every texel is multiplied with, with opacity in the fourth component.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.FlipX

`boolean`

Whether the image is mirrored left to right about its pivot.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.FlipY

`boolean`

Whether the image is mirrored top to bottom about its pivot.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.Frame

`number`

The index of the sheet's frame to draw; an index the sheet lacks draws nothing (SPRITE_FRAME_OUT_OF_RANGE).

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.Lit

`boolean`

Whether 2D lights light the sprite; an unlit sprite keeps its texels unchanged.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.NormalMap

`AssetRef?`

A lit sprite's tangent-space normal map, laid like its image; point lights then light it by the angle to the light. Null: flat.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.OrderInLayer

`number`

The draw order within the sorting layer: higher values draw over lower ones, equal values in canonical entity order.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.PixelsPerUnit

`number`

How many texels of the image make one unit of the entity's local space; the sprite's size is its texels divided by this.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.SetFrameByName

`(self: SpriteRenderer, name: string) -> ()`

Set Frame to the index of the sprite sheet's frame named name; an error names the sheet when it has no such frame.

All modes; Runtime|Test; mutates.

### SpriteRenderer.Sheet

`AssetRef?`

The sprite sheet whose frame Frame is drawn; null draws Texture whole.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.SortingLayer

`string`

The sorting layer name from project settings; later layers draw over earlier ones, and an undeclared name draws in the first. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### SpriteRenderer.Texture

`AssetRef?`

The texture drawn whole, pivoted at its centre, when Sheet is null; null draws the built-in white square.

All modes; Runtime|Test; pure/read; setter mutates.

## TaskHandle

An opaque coroutine handle owned by one script VM.

## Terrain

A heightfield laid out under the entity and drawn with up to four material layers blended by a splat map; sculpt and paint it with terrain.edit.

### Terrain.HeightScale

`number`

The height of the largest heightfield sample above the entity's origin, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### Terrain.Heightfield

`AssetRef?`

The heightfield whose samples shape the terrain; null draws nothing.

All modes; Runtime|Test; pure/read; setter mutates.

### Terrain.Layers

`{{BaseColorMap: AssetRef?, NormalMap: AssetRef?, Tint: Color, Tiling: number, Roughness: number, Metallic: number}}`

At most four material layers, weighed by the splat map's red, green, blue and alpha channels; empty draws one white layer.

All modes; Runtime|Test; pure/read; setter mutates.

### Terrain.LodBias

`number`

Scales the distances at which the terrain's chunks drop detail; above 1 keeps detail farther.

All modes; Runtime|Test; pure/read; setter mutates.

### Terrain.ReceiveShadows

`boolean`

Whether shadows fall on the terrain.

All modes; Runtime|Test; pure/read; setter mutates.

### Terrain.SampleHeight

`(self: Terrain, position: vector) -> number?`

Read the world-space height of the terrain's surface under the world point position (its XZ in the terrain's local space, the surface there, back to world space); nil outside the terrain or without a loaded heightfield.

All modes; Runtime|Test; pure/read.

### Terrain.SampleNormal

`(self: Terrain, position: vector) -> vector?`

Read the world-space unit normal of the terrain's surface under the world point position; nil where SampleHeight is nil.

All modes; Runtime|Test; pure/read.

### Terrain.Size

`vector`

The terrain's extent along its local X and Z axes from the entity's origin, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### Terrain.SplatMap

`AssetRef?`

The texture whose channels weigh the layers per point (a data texture, imported with Usage Linear and Compression None); null draws the first layer alone.

All modes; Runtime|Test; pure/read; setter mutates.

## TerrainCollider

The entity's Terrain as a static height field in the physics world, colliding with exactly the surface that is drawn.

### TerrainCollider.Friction

`number`

The friction coefficient of the terrain's own static body (unused when the terrain joins another body, as Layer); a contact uses the geometric mean of both bodies' values.

All modes; Runtime|Test; pure/read; setter mutates.

### TerrainCollider.Layer

`string`

Physics layer name from project settings, of the terrain's own static body (unused when the terrain joins another body: a RigidBody on or above the entity, or a collider-only ancestor's). A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### TerrainCollider.Restitution

`number`

The bounciness of the terrain's own static body, from 0 to 1 (unused when the terrain joins another body, as Layer); a contact uses the larger of both bodies' values.

All modes; Runtime|Test; pure/read; setter mutates.

## Text

Draws a text string on the screen or in the world.

### Text.Alignment

`TextAlignment`

How the lines are aligned to each other.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Anchor

`vector`

Screen space: the point of the viewport the text is placed at, normalized: (0, 0) is the top-left corner and (1, 1) the bottom-right, X to the right and Y down; (0.5, 0.5) is the centre.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Billboard

`boolean`

Whether world-space text always faces the camera.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Color

`Color`

The linear text colour with opacity in the fourth component.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Font

`AssetRef?`

The font; null uses the default font.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Offset

`vector`

Screen space: a further offset from the anchor in pixels at the 1080p reference resolution (scaled with the viewport's height), X to the right and Y down.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Pivot

`vector`

The point of the text block that sits on the anchor (Screen space) or at the entity's origin (World space), normalized to the block: (0, 0) is its top-left corner and (1, 1) its bottom-right.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Size

`number`

The em size. Screen space: pixels at the 1080p reference resolution, scaled by the viewport's height / 1080. World space: an em of Size / 100 metres (100 world-text pixels per metre) before the entity's scale, in the entity's local XY plane or facing the camera with Billboard, so the default 32 is 0.32 m.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Space

`TextSpace`

Whether the text is placed on the screen or in the world.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Text

`string`

The UTF-8 text; a line feed starts a new line. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

## TilemapCollider

The solid cells of a tilemap as 2D colliders: each chunk's solid cells merged into boxes, one static compound without a RigidBody2D.

### TilemapCollider.IsTrigger

`boolean`

Whether the collider only detects overlaps instead of colliding.

All modes; Runtime|Test; pure/read; setter mutates.

### TilemapCollider.Tilemap

`AssetRef?`

The tilemap whose solid cells collide; null takes the entity's TilemapRenderer.Tilemap.

All modes; Runtime|Test; pure/read; setter mutates.

## TilemapRenderer

Draws a tilemap's tiles in the entity's XY plane, each tile as a sprite of its palette entry's sheet frame.

### TilemapRenderer.Color

`Color`

The linear colour every tile's texels are multiplied with, with opacity in the fourth component.

All modes; Runtime|Test; pure/read; setter mutates.

### TilemapRenderer.Lit

`boolean`

Whether 2D lights light the tiles; unlit tiles keep their texels unchanged.

All modes; Runtime|Test; pure/read; setter mutates.

### TilemapRenderer.OrderInLayer

`number`

The draw order within the sorting layer, among the sprites: higher values draw over lower ones, equal values in canonical entity order.

All modes; Runtime|Test; pure/read; setter mutates.

### TilemapRenderer.PixelsPerUnit

`number`

How many texels of a tile's frame make one unit of the entity's local space, as a sprite's.

All modes; Runtime|Test; pure/read; setter mutates.

### TilemapRenderer.SortingLayer

`string`

The sorting layer name from project settings; the tilemap sorts with the sprites, and an undeclared name draws in the first. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### TilemapRenderer.Tilemap

`AssetRef?`

The tilemap whose tiles are drawn; null draws nothing.

All modes; Runtime|Test; pure/read; setter mutates.

## Transform

Positions, rotates and scales the entity relative to its parent.

### Transform.EulerAngles

`vector`

Rotation relative to the parent as Euler angles in degrees, applied Z, then X, then Y.

All modes; Runtime|Test; pure/read; setter mutates.

### Transform.Forward

`(self: Transform) -> vector`

World direction of local -Z.

All modes; Runtime|Test; pure/read.

### Transform.InverseTransformPoint

`(self: Transform, point: vector) -> vector`

Transform a world point into local space.

All modes; Runtime|Test; pure/read.

### Transform.LookAt

`(self: Transform, target: vector, up: vector?) -> ()`

Face the target using world up +Y by default; reject coincident or parallel directions.

All modes; Runtime|Test; mutates.

### Transform.RenderPosition

`vector`

The rendered world position, interpolated between fixed steps, in metres (read-only).

All modes; Runtime|Test; pure/read.

### Transform.RenderRotation

`Quat`

The rendered world rotation, interpolated between fixed steps (read-only).

All modes; Runtime|Test; pure/read.

### Transform.Right

`(self: Transform) -> vector`

World direction of local +X.

All modes; Runtime|Test; pure/read.

### Transform.Rotate

`(self: Transform, eulerDegrees: vector, space: Space?) -> ()`

Rotate using deterministic Z-X-Y Euler degrees in Local space by default.

All modes; Runtime|Test; mutates.

Argument 3: Space (optional); default Local.

### Transform.Rotation

`Quat`

Rotation relative to the parent: a unit quaternion written [x, y, z, w].

All modes; Runtime|Test; pure/read; setter mutates.

### Transform.Scale

`vector`

Scale along the local axes; each component's magnitude is at least 0.0001.

All modes; Runtime|Test; pure/read; setter mutates.

### Transform.Teleport

`(self: Transform, position: vector?, rotation: Quat?) -> ()`

Set optional world pose and reset interpolation.

All modes; Runtime|Test; mutates.

### Transform.TransformDirection

`(self: Transform, direction: vector) -> vector`

Rotate a direction into world space without translation or scale.

All modes; Runtime|Test; pure/read.

### Transform.TransformPoint

`(self: Transform, point: vector) -> vector`

Transform a point using world translation, rotation and scale.

All modes; Runtime|Test; pure/read.

### Transform.Translate

`(self: Transform, delta: vector, space: Space?) -> ()`

Translate in Local orientation by default, or World axes.

All modes; Runtime|Test; mutates.

Argument 3: Space (optional); default Local.

### Transform.Translation

`vector`

Position relative to the parent, in metres.

All modes; Runtime|Test; pure/read; setter mutates.

### Transform.Up

`(self: Transform) -> vector`

World direction of local +Y.

All modes; Runtime|Test; pure/read.

### Transform.WorldPosition

`vector`

Position in world space, in metres; writing it moves the entity through its parent.

All modes; Runtime|Test; pure/read; setter mutates.

### Transform.WorldRotation

`Quat`

Rotation in world space; writing it rotates the entity through its parent.

All modes; Runtime|Test; pure/read; setter mutates.

### Transform.WorldScale

`vector`

Approximate scale in world space (read-only: a sheared parent chain has no exact one).

All modes; Runtime|Test; pure/read.

## UiButton

A UI button: a pointer click or Submit while focused raises OnUiClick.

### UiButton.Focusable

`boolean`

Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiButton.Interactable

`boolean`

Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event.

All modes; Runtime|Test; pure/read; setter mutates.

## UiCanvas

The root of a UI tree, laid out over the whole scene image in scene pixels and drawn after the game's text, or, in World mode, laid out in its own canvas pixels and drawn as a rectangle in the world.

### UiCanvas.DefaultFocus

`Entity?`

The UI widget keyboard and gamepad navigation focuses first while nothing has focus; null: navigation starts only from a pointer press or Ui.SetFocus.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.Font

`AssetRef?`

The font of the canvas's texts whose own Font is null; null uses the default font.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.PixelSnap

`boolean`

Rounds every element's corners to whole scene pixels, so images and clips land on pixel corners.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.PixelsPerUnit

`number`

A World canvas's canvas pixels per world unit: its rectangle is WorldWidth x WorldHeight over this, centred on the entity.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.ReferenceHeight

`number`

MatchHeight: the canvas's height in canvas units, so one unit is the scene's height / ReferenceHeight scene pixels.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.RenderMode

`UiCanvasRenderMode`

Screen draws the canvas over the scene image; World draws it as a rectangle in the world at the entity's transform, depth-tested against the scene.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.ScaleFactor

`number`

ConstantPixelSize: the scene pixels per canvas unit.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.ScaleMode

`UiScaleMode`

How canvas units map to scene pixels.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.SortOrder

`number`

Canvases draw in ascending SortOrder, ties in entity order; a later canvas draws over earlier ones.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.Theme

`{Normal: Color, Hovered: Color, Pressed: Color, Focused: Color, Disabled: Color}`

The state tints of the canvas's widgets (buttons, toggles, sliders, scroll views and text inputs).

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.WorldHeight

`number`

A World canvas's height in canvas pixels (its layout's scene resolution); Screen canvases ignore it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiCanvas.WorldWidth

`number`

A World canvas's width in canvas pixels (its layout's scene resolution); Screen canvases ignore it.

All modes; Runtime|Test; pure/read; setter mutates.

## UiImage

Draws a sprite sheet frame, or a plain colour, over a UI element's rectangle.

### UiImage.BorderScale

`number`

Sliced: the canvas units each border texel takes.

All modes; Runtime|Test; pure/read; setter mutates.

### UiImage.Color

`Color`

The linear tint multiplied with the frame's texels, with opacity in the fourth component.

All modes; Runtime|Test; pure/read; setter mutates.

### UiImage.Filter

`UiImageFilter`

How the frame's texture is sampled.

All modes; Runtime|Test; pure/read; setter mutates.

### UiImage.Frame

`number`

The index of the frame in the sheet's Frames; a frame the sheet lacks draws nothing.

All modes; Runtime|Test; pure/read; setter mutates.

### UiImage.RaycastTarget

`boolean`

Whether the image catches the pointer for its nearest widget; false lets the pointer pass through it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiImage.Sheet

`AssetRef?`

The sprite sheet whose frame is drawn; null draws a plain rectangle of Color.

All modes; Runtime|Test; pure/read; setter mutates.

### UiImage.Type

`UiImageType`

Whether the frame stretches or keeps its 9-slice borders.

All modes; Runtime|Test; pure/read; setter mutates.

## UiLayout

A container that places its children's rectangles in a stack, a grid or a flexible row or column.

### UiLayout.CellSize

`vector`

Grid: the size of each cell in canvas units.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayout.Columns

`number`

Grid: the cells per row (Horizontal) or per column (Vertical); 0 fits as many as the container holds.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayout.CrossAlignment

`UiCrossAlignment`

Where each child sits across the main axis.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayout.Direction

`UiLayoutDirection`

The main axis the children follow.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayout.Kind

`UiLayoutKind`

How the children are placed.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayout.MainAlignment

`UiMainAlignment`

Where the children sit along the main axis when they do not fill it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayout.Padding

`{Left: number, Top: number, Right: number, Bottom: number}`

Insets of the container's rectangle that the children are placed in.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayout.Spacing

`vector`

The gaps between children, lines and cells along x and y, in canvas units.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayout.Wrap

`boolean`

Flex: whether a child that does not fit the line starts a new line.

All modes; Runtime|Test; pure/read; setter mutates.

## UiLayoutElement

How a UI element takes part in its parent's layout container: flex weights, a minimum size, or none.

### UiLayoutElement.Grow

`number`

Flex: the element's share of the main axis's free space.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayoutElement.IgnoreLayout

`boolean`

Whether the element keeps its own placement and takes no space in the container.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayoutElement.MinSize

`vector`

The size the element never goes below in a Stack or Flex container, in canvas units.

All modes; Runtime|Test; pure/read; setter mutates.

### UiLayoutElement.Shrink

`number`

Flex: the element's share, times its preferred size, of the main axis's overflow.

All modes; Runtime|Test; pure/read; setter mutates.

## UiRect

A UI element's rectangle in its parent's (or its canvas's), in canvas units, x right and y down.

### UiRect.AnchorMax

`vector`

The anchor rectangle's bottom-right corner, normalized in the parent's rectangle; equal to AnchorMin for a fixed-size element.

All modes; Runtime|Test; pure/read; setter mutates.

### UiRect.AnchorMin

`vector`

The anchor rectangle's top-left corner, normalized in the parent's rectangle: (0, 0) its top-left corner, (1, 1) its bottom-right.

All modes; Runtime|Test; pure/read; setter mutates.

### UiRect.ClipChildren

`boolean`

Whether the element's descendants are clipped to its rectangle.

All modes; Runtime|Test; pure/read; setter mutates.

### UiRect.Pivot

`vector`

The point of the element that Position places, normalized to the element: (0, 0) its top-left corner, (1, 1) its bottom-right.

All modes; Runtime|Test; pure/read; setter mutates.

### UiRect.Position

`vector`

The pivot's offset from its point in the anchor rectangle, in canvas units, x right and y down.

All modes; Runtime|Test; pure/read; setter mutates.

### UiRect.Size

`vector`

The element's size in canvas units, added to the anchor rectangle's size (negative values inset stretched anchors); a container's child takes it as its preferred size.

All modes; Runtime|Test; pure/read; setter mutates.

## UiScrollView

A UI scroll view: clips its descendants and moves its Content by the scroll position, which the wheel, a drag and focus change.

### UiScrollView.Content

`Entity?`

The UI element below the scroll view that scrolls; null scrolls nothing.

All modes; Runtime|Test; pure/read; setter mutates.

### UiScrollView.Focusable

`boolean`

Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiScrollView.Horizontal

`boolean`

Whether the content scrolls along x.

All modes; Runtime|Test; pure/read; setter mutates.

### UiScrollView.Interactable

`boolean`

Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event.

All modes; Runtime|Test; pure/read; setter mutates.

### UiScrollView.ScrollPosition

`vector`

Canvas units the content is moved left (x) and up (y), read clamped to the content's overflow.

All modes; Runtime|Test; pure/read; setter mutates.

### UiScrollView.ScrollStep

`number`

Canvas units per wheel step and per navigation press.

All modes; Runtime|Test; pure/read; setter mutates.

### UiScrollView.Vertical

`boolean`

Whether the content scrolls along y.

All modes; Runtime|Test; pure/read; setter mutates.

## UiSlider

A UI slider: the pointer drags its value between Min and Max and the directions along it step it, raising OnUiValueChanged.

### UiSlider.Direction

`UiLayoutDirection`

Horizontal puts Min at the left edge; Vertical puts it at the bottom edge.

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.Fill

`Entity?`

A UI element below the slider stretched from the Min edge to the value; null for none.

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.Focusable

`boolean`

Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.Handle

`Entity?`

A UI element below the slider placed at the value; null for none.

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.Interactable

`boolean`

Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event.

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.Max

`number`

The value at the end edge (right, or top when Vertical).

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.Min

`number`

The value at the start edge (left, or bottom when Vertical); below Max.

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.Step

`number`

The change per navigation press along the slider; 0 is a tenth of the range. With WholeNumbers it is rounded to a whole number of at least 1.

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.Value

`number`

The value, read clamped to Min and Max (and rounded with WholeNumbers).

All modes; Runtime|Test; pure/read; setter mutates.

### UiSlider.WholeNumbers

`boolean`

Whether the value is rounded to whole numbers.

All modes; Runtime|Test; pure/read; setter mutates.

## UiText

Draws text in a UI element's rectangle, wrapped and aligned in it.

### UiText.Alignment

`TextAlignment`

How each line is aligned within the rectangle's width.

All modes; Runtime|Test; pure/read; setter mutates.

### UiText.Color

`Color`

The linear text colour with opacity in the fourth component.

All modes; Runtime|Test; pure/read; setter mutates.

### UiText.Font

`AssetRef?`

The font; null uses its canvas's Font, or the default font when the canvas names none.

All modes; Runtime|Test; pure/read; setter mutates.

### UiText.LineSpacing

`number`

Multiplies the font's line height.

All modes; Runtime|Test; pure/read; setter mutates.

### UiText.RaycastTarget

`boolean`

Whether the text catches the pointer for its nearest widget; false lets the pointer pass through it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiText.Size

`number`

The em size in canvas units.

All modes; Runtime|Test; pure/read; setter mutates.

### UiText.Text

`string`

The UTF-8 text; a line feed starts a new line. Scripts read localized strings with Loc.Get. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### UiText.VerticalAlignment

`UiTextVerticalAlignment`

Where the text block sits in the rectangle's height.

All modes; Runtime|Test; pure/read; setter mutates.

### UiText.Wrap

`boolean`

Whether lines break at spaces to fit the rectangle's width.

All modes; Runtime|Test; pure/read; setter mutates.

## UiTextInput

A UI text field: while focused, typed characters insert at its caret and the editing keys edit it, raising OnUiTextChanged; Enter raises OnUiSubmit.

### UiTextInput.Caret

`number`

The caret, in codepoints from the start of Text, read clamped to Text's length; typing and the editing keys move it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiTextInput.Focusable

`boolean`

Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiTextInput.Interactable

`boolean`

Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event.

All modes; Runtime|Test; pure/read; setter mutates.

### UiTextInput.MaxLength

`number`

The most codepoints typing reaches; characters typed past it are dropped.

All modes; Runtime|Test; pure/read; setter mutates.

### UiTextInput.Placeholder

`Entity?`

A UI element below the input shown only while its Text is empty (a hint); null for none.

All modes; Runtime|Test; pure/read; setter mutates.

### UiTextInput.Text

`string`

The field's UTF-8 text: typing inserts at the caret; a script's write raises no OnUiTextChanged. A value a script writes holds at most 4096 bytes of text.

All modes; Runtime|Test; pure/read; setter mutates.

### UiTextInput.TextElement

`Entity?`

A UI element below the input whose UiText shows the input's Text, with the caret while focused; null shows nothing.

All modes; Runtime|Test; pure/read; setter mutates.

## UiToggle

A UI toggle: a pointer click or Submit while focused flips IsOn and raises OnUiValueChanged; Graphic shows while it is on.

### UiToggle.Focusable

`boolean`

Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it.

All modes; Runtime|Test; pure/read; setter mutates.

### UiToggle.Graphic

`Entity?`

A UI element below the toggle (its check mark) that is drawn and hit only while the toggle is on; null for none.

All modes; Runtime|Test; pure/read; setter mutates.

### UiToggle.Interactable

`boolean`

Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event.

All modes; Runtime|Test; pure/read; setter mutates.

### UiToggle.IsOn

`boolean`

Whether the toggle is on.

All modes; Runtime|Test; pure/read; setter mutates.

## AlphaMode

How a material's base colour alpha is used (the glTF 2.0 alpha modes).

- `Opaque`: Alpha is ignored: the surface is fully opaque.

- `Mask`: Pixels whose alpha is below AlphaCutoff are discarded; the others are opaque.

- `Blend`: The surface is blended over what lies behind it by its alpha.

## AnimatorRootMotion (enum)

What an Animator does with its skeleton's root motion.

- `None`: The root motion stays in the pose, so the mesh moves away from the entity.

- `Apply`: The root motion moves the entity's Transform each fixed step; the mesh stays at the entity.

- `Extract`: The root motion is held out of the pose and only reported by Animator:GetRootMotion, for a script to apply.

## AntiAliasing

How edges are anti-aliased.

- `None`: No anti-aliasing.

- `Fxaa`: FXAA on the final image (the default).

- `Taa`: Temporal anti-aliasing: the jittered image resolved against its history (off in pixel-art mode).

## AssetCreateType

What asset.create makes.

- `Material`: A .material file: a PBR material.

- `Scene`: A .scene file: an empty scene named after the file.

- `Prefab`: A .prefab file with one empty root entity named after the file; it takes no values (prefab.create saves an entity built in a scene, its components and children, as a prefab).

- `SoundEffect`: A .sfx sound effect: synthesized layers of oscillators, imported as an AudioClip.

- `Folder`: A folder below Assets/; one that already exists is left as it is (created: false).

- `SpriteSheet`: A .spritesheet sprite sheet: frames of a texture with pivots and 9-slice borders, and clips; without values an empty sheet.

- `Tilemap`: A .tilemap tilemap: a palette of sprite sheet frames and cells in chunks; without values an empty map of unit cells in 16-cell chunks.

- `StringTable`: A .stringtable string table: keyed strings in several locales, read by Loc.Get; without values the locale en and no entries.

- `Heightfield`: A .heightfield heightfield for a Terrain: a flat square of 129 x 129 samples at height 0; takes no values (terrain.importHeightmap converts a heightmap, terrain.edit sculpts it).

## AssetState

Where an asset is in its life (§7.2).

- `Unloaded`: Registered, not loaded.

- `Loading`: A load or the import it needs is running.

- `Loaded`: The current version is loaded.

- `Failed`: The last load failed; uses get the placeholder and a diagnostic is recorded.

## AssetType

The kind of a loadable asset (§7.1).

- `None`: No type; as a filter, every type.

- `Scene`: A scene (.scene).

- `Prefab`: A prefab (.prefab, or the hierarchy of a glTF model).

- `Mesh`: A mesh (a glTF's mesh sub-asset, or a built-in mesh).

- `Material`: A PBR material (.material, or a glTF's material sub-asset).

- `Texture`: A texture (PNG, JPEG, TGA, BMP, or a glTF's texture sub-asset).

- `Environment`: An image-based lighting environment (.hdr or .exr).

- `AudioClip`: A sound (WAV, FLAC, MP3, Ogg Vorbis, Ogg Opus, or a .sfx preset).

- `Script`: A Luau script (.luau).

- `Font`: A font (TTF, OTF) as a signed distance field atlas.

- `Replay`: An input replay (.replay).

- `SpriteSheet`: A sprite sheet (.spritesheet): frames of a texture with pivots and 9-slice borders, and clips.

- `Tilemap`: A tilemap (.tilemap): a grid of cells in chunks, each empty or a palette entry's sprite sheet frame.

- `StringTable`: A string table (.stringtable): keyed strings in several locales, read by Loc.Get.

- `Skeleton`: A skeleton (a glTF's skin sub-asset): named joints in a hierarchy with their rest pose and inverse binds.

- `AnimationClip`: An animation clip (a glTF's animation sub-asset): keyed joint transforms and morph weights, with named events.

- `IrradianceVolume`: A baked irradiance volume (.irradiance, written by lighting.bake): a grid of probes' SH9 irradiance that an IrradianceVolume component lights with.

- `Lut`: A colour-grading 3D lookup table (.cube, or a built-in LUT) that a PostProcess applies after its tonemap curve.

- `Heightfield`: A terrain's heightfield (.heightfield): a square grid of at most 1025 x 1025 16-bit height samples that a Terrain lays out.

## Attenuation

How a spatial sound's volume falls off with distance.

- `None`: No distance attenuation.

- `Inverse`: Inverse-distance falloff, like sound in open air (the default).

- `Linear`: Linear falloff from MinDistance to silence at MaxDistance.

- `Exponential`: Exponential falloff, steeper than inverse-distance.

## AudioDeviceState

The state of the audio engine's playback device.

- `None`: No device by configuration (headless processes): the host pulls the frames.

- `Running`: The device exists and plays.

- `Recreating`: The device stopped unexpectedly; a re-creation is scheduled.

- `Failed`: No device could be created: the engine runs without one.

## AudioGroup

The mixer group a sound plays in.

- `Music`: Background music.

- `Sfx`: Sound effects (the default).

- `Ui`: User-interface sounds.

## AudioStreamMode

Whether an imported audio clip decodes when it loads or streams while it plays.

- `Auto`: Stream clips longer than 10 seconds and decode the shorter ones when they load.

- `Stream`: Always stream the clip while it plays.

- `Decode`: Always decode the whole clip when it loads.

## AudioTimeSource

Who advances the audio engine's voices.

- `Device`: The playback device, in real time.

- `Simulation`: The play session's ticks (lockstep or a test run): one tick of frames per tick.

- `Host`: The host's frames, without a device.

## BodyType

How a rigid body moves.

- `Static`: Never moves; the cheapest body, used for level geometry.

- `Kinematic`: Moved by script or animation; pushes dynamic bodies but is not pushed back, and static bodies never stop it (clamp its motion in the script).

- `Dynamic`: Simulated: moved by gravity, forces and collisions.

## ClearMode

What a camera draws behind the scene's geometry.

- `Skybox`: The scene's Environment skybox (its fallback colour when none is shown).

- `Color`: The camera's ClearColor.

## CommandOrigin

Who issued an undoable command.

- `User`: The editor's user, through its panels.

- `Agent`: An automation client; the undo label carries the prefix "[agent] ".

## CursorMode

Native cursor visibility and capture.

- `Normal`: The Normal input value.

- `Hidden`: The Hidden input value.

- `Locked`: The Locked input value.

## DiagnosticSeverity

How serious a diagnostic is.

- `Warning`: Worth fixing; nothing is blocked by it.

- `Error`: Must be fixed; it can block play or export.

## EngineEventType

The kind of an engine event (§4.9).

- `EntityCreated`: An entity was created.

- `EntityDestroyed`: An entity was destroyed.

- `ComponentChanged`: A component of an entity changed.

- `SceneOpened`: A scene became the open scene.

- `SceneChangedOnDisk`: The open scene's file, or a prefab it uses, changed on disk.

- `PlayStateChanged`: Play mode started, paused, resumed or stopped.

- `AssetReloaded`: An asset was reloaded after a change.

- `AssetImportFailed`: Importing an asset failed.

- `ScriptErrorRaised`: A script raised an error.

- `DiagnosticsChanged`: The project's diagnostics changed.

- `AutomationClientDisconnected`: An automation client disconnected.

- `GpuDeviceLost`: The GPU device was lost and has been recovered, so the frames after it render again (message: the loss's report). A failed recovery ends the process, so it never appears.

- `SceneActivated`: The editor's active scene changed.

- `SceneClosed`: The editor closed an open scene.

## ExportConfiguration

The build configuration an export targets (§2.2).

- `Debug`: Engine asserts on, unoptimized, with the automation server.

- `Release`: Engine asserts on, optimized, with the automation server.

- `Dist`: The shipping configuration: asserts off, no automation server.

## ExportPhase

A step of an export (§14.2), in the order the steps run.

- `Validate`: The project, its scenes, scripts and settings and the build outputs are checked; nothing is written.

- `Cook`: Every included asset is cooked through the asset manager.

- `WritePaks`: Data/Engine.pak and Data/Game.pak are written and verified.

- `NativeBuild`: A game with a Native/ folder gets a Runtime built with its native module linked in; other games skip it.

- `CopyRuntime`: The configuration's Runtime is copied, renamed after the project, with what it needs.

- `PatchResources`: The executable's icon and version (Windows) or Info.plist (macOS) are written.

- `WriteManifest`: Game.json and THIRD_PARTY_NOTICES.txt are written.

- `Sign`: The macOS bundle is signed; nothing happens elsewhere.

- `SmokeTest`: The staged game runs headless when smokeTest asks for it.

- `FeatureTest`: The staged testing export runs --feature-test when featureTest asks for it, and its replays' hashes are read.

- `MoveToOutput`: The staged package replaces the output directory.

- `Done`: The export finished.

## ExportReachReason

Why an export put an asset into its paks (the first reason its walk found).

- `StartScene`: Game.pak: the project's StartScene.

- `BuildScene`: Game.pak: an Export.BuildScenes entry.

- `Testing`: Game.pak: content a testing export packages whatever Export.Exclude says.

- `Dependency`: Another packaged asset (from) references it or one of its sub-assets: an import dependency or a Script field value.

- `LiteralLoad`: A packaged script (from) loads it with Assets.Load and a string literal (at line).

- `Required`: Engine.pak: the exported game asks for this built-in by handle, so every export holds it.

- `ComputedLoad`: Engine.pak: kept with every built-in, because a packaged script (from) computes an Assets.Load reference (at line).

## ExposureMode

How the image is exposed.

- `Manual`: The image is multiplied by 2^ExposureEV.

- `Auto`: A histogram of the image's luminance adapts the exposure towards middle grey, ExposureEV added as compensation.

## FieldType

The underlying reflected value kind.

- `Bool`: A reflected Bool value.

- `Int32`: A reflected Int32 value.

- `UInt32`: A reflected UInt32 value.

- `Float`: A reflected Float value.

- `Vec2`: A reflected Vec2 value.

- `Vec3`: A reflected Vec3 value.

- `Vec4`: A reflected Vec4 value.

- `Quat`: A reflected Quat value.

- `Color3`: A reflected Color3 value.

- `Color4`: A reflected Color4 value.

- `Bool3`: A reflected Bool3 value.

- `String`: A reflected String value.

- `EntityRef`: A reflected EntityRef value.

- `AssetRef`: A reflected AssetRef value.

- `Enum`: A reflected Enum value.

- `Array`: A reflected Array value.

- `Struct`: A reflected Struct value.

- `Map`: A reflected Map value.

- `Variant`: A reflected Variant value.

## GamepadAxis

Up-positive stick axes and zero-to-one triggers.

- `LeftX`: The LeftX input value.

- `LeftY`: The LeftY input value.

- `RightX`: The RightX input value.

- `RightY`: The RightY input value.

- `LeftTrigger`: The LeftTrigger input value.

- `RightTrigger`: The RightTrigger input value.

## GamepadButton

Gamepad button positions.

- `South`: The South input value.

- `East`: The East input value.

- `West`: The West input value.

- `North`: The North input value.

- `LeftBumper`: The LeftBumper input value.

- `RightBumper`: The RightBumper input value.

- `Back`: The Back input value.

- `Start`: The Start input value.

- `Guide`: The Guide input value.

- `LeftThumb`: The LeftThumb input value.

- `RightThumb`: The RightThumb input value.

- `DPadUp`: The DPadUp input value.

- `DPadRight`: The DPadRight input value.

- `DPadDown`: The DPadDown input value.

- `DPadLeft`: The DPadLeft input value.

## GraphNodeCategory

Where the graph editor's node menu lists a node kind.

- `Event`: A behaviour callback: where a flow starts (Event.*).

- `Flow`: Branches, sequences, loops and delays (Flow.*).

- `Member`: The behaviour's own entity, fields and variables (Self.*).

- `Operator`: Arithmetic, comparison, logic, vector and text operators.

- `Call`: A script API function, method or constructor (Call.*).

- `Property`: A script API property or component field (Get.*, Set.*).

- `Constant`: A script API constant (Const.*).

## GraphPinDirection

Whether a graph pin takes or gives its value.

- `Input`: The pin takes a value (or, for Exec, is where the flow enters).

- `Output`: The pin gives a value (or, for Exec, is where the flow continues).

## GraphPinType

What a graph pin carries.

- `Exec`: Control flow, no value.

- `Any`: Any value (Luau's any): links to every data pin.

- `Bool`: A boolean.

- `Number`: A number.

- `String`: A string.

- `Vector`: A native three-component vector.

- `Color`: A linear RGBA Color.

- `Quat`: A quaternion.

- `Entity`: An Entity.

- `Asset`: An AssetRef; typeName names its AssetType when the member restricts it.

- `Enum`: One choice of a registered enum, a string; typeName names the enum.

- `Object`: A value of another named type (a component, a TaskHandle, a record such as CollisionContact); typeName names it.

## GraphValueKind

The kind of a graph field or variable: its Luau Field constructor.

- `Number`: A finite f32 number (Field.Number); Default a number.

- `Integer`: An exact signed 32-bit integer (Field.Integer); Default an integer.

- `Bool`: A boolean (Field.Bool); Default true or false.

- `String`: A string (Field.String); Default a string.

- `Vector`: A native three-component vector (Field.Vector); Default [x, y, z].

- `Color`: A linear RGBA colour (Field.Color); Default [r, g, b, a].

- `Quat`: A quaternion (Field.Quat); Default [x, y, z, w], nonzero, and of unit length for a field.

- `Entity`: An Entity (Field.Entity); Default null.

- `Asset`: An asset reference of one AssetType (Field.Asset); Default null.

- `Enum`: One of EnumValues (Field.Enum); Default one of them.

- `Array`: An array of Element values (Field.Array); Default [].

## InputActionType

How an input action reads its controls.

- `Button`: Pressed or released, driven by the action's Bindings.

- `Axis`: A value in [-1, 1] from the Positive and Negative controls and the Gamepad axis.

## InputEventState

The state of a button-like input event (§13.6).

- `Down`: Pressed on its tick.

- `Up`: Released on its tick.

- `Tap`: Pressed on its tick and released on the next, so both edges are observed.

## InputEventType

The type of an input event (§13.6).

- `Action`: A project action: a button use (state) or an axis value (value).

- `Key`: A keyboard key going down or up.

- `MouseButton`: A mouse button going down or up, optionally at a position.

- `MouseMove`: The cursor moving to a position in view coordinates (the session's view: the game view's image in ordinary play, Window.Width x Window.Height in lockstep, recording, replay and test sessions).

- `MouseDelta`: The cursor moving by a delta.

- `Scroll`: Scrolling by a delta (x right-positive, y up-positive).

- `GamepadButton`: A gamepad button going down or up.

- `GamepadAxis`: A gamepad axis taking a value.

- `Text`: Typed text, one character event per code point.

## InputRecordAction

Start a fresh recording or stop and save it.

- `Start`: Start at tick zero.

- `Stop`: Validate and save the completed recording.

## Key

Keyboard keys by their US-layout position.

- `Space`: The Space input value.

- `Apostrophe`: The Apostrophe input value.

- `Comma`: The Comma input value.

- `Minus`: The Minus input value.

- `Period`: The Period input value.

- `Slash`: The Slash input value.

- `D0`: The D0 input value.

- `D1`: The D1 input value.

- `D2`: The D2 input value.

- `D3`: The D3 input value.

- `D4`: The D4 input value.

- `D5`: The D5 input value.

- `D6`: The D6 input value.

- `D7`: The D7 input value.

- `D8`: The D8 input value.

- `D9`: The D9 input value.

- `Semicolon`: The Semicolon input value.

- `Equal`: The Equal input value.

- `A`: The A input value.

- `B`: The B input value.

- `C`: The C input value.

- `D`: The D input value.

- `E`: The E input value.

- `F`: The F input value.

- `G`: The G input value.

- `H`: The H input value.

- `I`: The I input value.

- `J`: The J input value.

- `K`: The K input value.

- `L`: The L input value.

- `M`: The M input value.

- `N`: The N input value.

- `O`: The O input value.

- `P`: The P input value.

- `Q`: The Q input value.

- `R`: The R input value.

- `S`: The S input value.

- `T`: The T input value.

- `U`: The U input value.

- `V`: The V input value.

- `W`: The W input value.

- `X`: The X input value.

- `Y`: The Y input value.

- `Z`: The Z input value.

- `LeftBracket`: The LeftBracket input value.

- `Backslash`: The Backslash input value.

- `RightBracket`: The RightBracket input value.

- `GraveAccent`: The GraveAccent input value.

- `World1`: The World1 input value.

- `World2`: The World2 input value.

- `Escape`: The Escape input value.

- `Enter`: The Enter input value.

- `Tab`: The Tab input value.

- `Backspace`: The Backspace input value.

- `Insert`: The Insert input value.

- `Delete`: The Delete input value.

- `Right`: The Right input value.

- `Left`: The Left input value.

- `Down`: The Down input value.

- `Up`: The Up input value.

- `PageUp`: The PageUp input value.

- `PageDown`: The PageDown input value.

- `Home`: The Home input value.

- `End`: The End input value.

- `CapsLock`: The CapsLock input value.

- `ScrollLock`: The ScrollLock input value.

- `NumLock`: The NumLock input value.

- `PrintScreen`: The PrintScreen input value.

- `Pause`: The Pause input value.

- `F1`: The F1 input value.

- `F2`: The F2 input value.

- `F3`: The F3 input value.

- `F4`: The F4 input value.

- `F5`: The F5 input value.

- `F6`: The F6 input value.

- `F7`: The F7 input value.

- `F8`: The F8 input value.

- `F9`: The F9 input value.

- `F10`: The F10 input value.

- `F11`: The F11 input value.

- `F12`: The F12 input value.

- `F13`: The F13 input value.

- `F14`: The F14 input value.

- `F15`: The F15 input value.

- `F16`: The F16 input value.

- `F17`: The F17 input value.

- `F18`: The F18 input value.

- `F19`: The F19 input value.

- `F20`: The F20 input value.

- `F21`: The F21 input value.

- `F22`: The F22 input value.

- `F23`: The F23 input value.

- `F24`: The F24 input value.

- `F25`: The F25 input value.

- `Keypad0`: The Keypad0 input value.

- `Keypad1`: The Keypad1 input value.

- `Keypad2`: The Keypad2 input value.

- `Keypad3`: The Keypad3 input value.

- `Keypad4`: The Keypad4 input value.

- `Keypad5`: The Keypad5 input value.

- `Keypad6`: The Keypad6 input value.

- `Keypad7`: The Keypad7 input value.

- `Keypad8`: The Keypad8 input value.

- `Keypad9`: The Keypad9 input value.

- `KeypadDecimal`: The KeypadDecimal input value.

- `KeypadDivide`: The KeypadDivide input value.

- `KeypadMultiply`: The KeypadMultiply input value.

- `KeypadSubtract`: The KeypadSubtract input value.

- `KeypadAdd`: The KeypadAdd input value.

- `KeypadEnter`: The KeypadEnter input value.

- `KeypadEqual`: The KeypadEqual input value.

- `LeftShift`: The LeftShift input value.

- `LeftControl`: The LeftControl input value.

- `LeftAlt`: The LeftAlt input value.

- `LeftSuper`: The LeftSuper input value.

- `RightShift`: The RightShift input value.

- `RightControl`: The RightControl input value.

- `RightAlt`: The RightAlt input value.

- `RightSuper`: The RightSuper input value.

- `Menu`: The Menu input value.

## Light2DType (enum)

What a 2D light lights.

- `Point`: A circle of light around the entity, fading to nothing at its Radius.

- `Global`: The same light at every lit pixel of the view, wherever the entity is.

## LogChannel

The logger an entry came from.

- `Engine`: The engine's own messages.

- `App`: The editor's or the game executable's messages.

- `Script`: Luau Log.* calls and print.

## LogLevel

The level of a log entry.

- `Trace`: Detailed tracing, compiled out in Dist.

- `Info`: Normal operation.

- `Warn`: Something unexpected that the engine handled.

- `Error`: An operation failed.

- `Critical`: The process cannot continue normally.

## MotionQuality

How a moving body is checked for collisions between steps.

- `Discrete`: Collisions are checked at the end of each step (fast bodies can tunnel through thin ones).

- `LinearCast`: Continuous collision detection along the step's motion; prevents tunnelling.

## MouseButton

Mouse button names.

- `Left`: The Left input value.

- `Right`: The Right input value.

- `Middle`: The Middle input value.

- `Button4`: The Button4 input value.

- `Button5`: The Button5 input value.

- `Button6`: The Button6 input value.

- `Button7`: The Button7 input value.

- `Button8`: The Button8 input value.

## NativeTypeRole

What a type of a game's native module is.

- `Component`: Data on entities, saved in scenes and prefabs as Native.<Name>.

- `Resource`: One value per play session, part of the state hash, never saved.

## ParticleBlend

How a particle's colour reaches the image.

- `Alpha`: Straight-alpha blending, the emitter's particles drawn back to front.

- `Additive`: Added to the image (colour times alpha): glows, sparks and fire, in any order.

## ParticleEmitterShape (enum)

Where a ParticleEmitter's new particles start and which way they move.

- `Point`: At the emitter's origin, moving in a uniformly random direction.

- `Sphere`: Uniformly inside a sphere of ShapeRadius, moving away from its centre.

- `Cone`: On a disc of ShapeRadius in the local XY plane, moving within ConeAngle of the local -Z axis.

- `Box`: Uniformly inside a box of BoxHalfExtents, moving down the local -Z axis.

## ParticleSimulationSpace

Which space a ParticleEmitter's live particles move in.

- `World`: Spawned at the emitter's pose, then independent of it: a moving emitter leaves a trail.

- `Local`: Simulated in the emitter's local space, so they move with the emitter.

## Physics2DQueryShape

The shape of a 2D overlap query.

- `Circle`: A circle of radius `radius` around `center`.

- `Box`: A rectangle of half extents `halfExtents` around `center`, rotated by `angle` degrees.

## PhysicsBodyOrigin

Why a physics body exists (§5.3 composition rules).

- `RigidBody`: The owner's RigidBody, with its own colliders and those of collider-only descendants.

- `ImplicitStatic`: Solid colliders without a RigidBody above them: a static body.

- `ImplicitSensor`: Trigger colliders on an entity without its own RigidBody: a kinematic sensor that follows the entity.

- `Character`: The owner's CharacterController; its body is the controller's inner capsule.

## PlayMode

How a play session runs (§5.6).

- `Play`: The game as it ships: scripts and audio run.

- `Simulate`: Play without scripts and audio, viewed through the editor camera.

## PlayRunState

The play state, in the vocabulary of _meta.playState.

- `Edit`: No play session (the editor edits its scene).

- `Play`: A running Play session.

- `Simulate`: A running Simulate session.

- `Paused`: A paused session of either mode.

## PlayStepRender

Which ticks of a play.step extract the play scene's game view (its primary camera) for rendering.

- `Every`: Every tick, one tick per frame.

- `Last`: Only the last tick; the ticks before it run as fast as the frame budget allows.

- `None`: No tick.

## PrefabOverrideKind

What a prefab override changes on one prefab entity of an instance.

- `Field`: One field of one component; Value is the field's value.

- `AddComponent`: A component added to the prefab entity; Value is the whole component.

- `RemoveComponent`: A component of the prefab entity removed from it; Value is null.

- `EntityKey`: An entity key (Name, Active or Tags); Value is the key's value.

- `RemoveEntity`: A prefab entity removed from the instance with its prefab descendants; Value is null.

## ProjectTemplate

What a new project starts with.

- `Empty`: The folder skeleton, .luaurc, .gitignore and an AGENTS.md stub; no scene.

- `Basic3D`: A camera with audio listener, sun, Studio environment and ground collider in a ready-to-edit scene.

## ProjectionType

How a camera projects the scene onto the screen.

- `Perspective`: Perspective projection with the vertical field of view VerticalFov.

- `Orthographic`: Parallel projection whose half height is OrthographicSize.

## RenderDebugView

What a view renders instead of the lit image (a debug view).

- `Lit`: The shaded image.

- `Albedo`: The base colour (factor times map), alpha ignored, sRGB-encoded.

- `Normals`: The shading normal (normal-mapped, world space) as N * 0.5 + 0.5.

- `Roughness`: The perceptual roughness (after its 0.045 clamp) as grey.

- `Metallic`: Metallic as grey.

- `Emissive`: The emitted radiance (Emissive times map times EmissiveStrength), clamped to [0, 1], sRGB-encoded.

- `AO`: The final ambient occlusion (GTAO, before the material's occlusion) as grey; white where it is off and on the background.

- `ShadowCascades`: Each surface coloured by the directional shadow cascade its view distance falls in: 1 red, 2 green, 3 blue, 4 yellow, blended across the blend bands; black means no surface, no shadow-casting directional light, or beyond the shadow distance (ShadowDistance or the camera's far clip, whichever is nearer), into which the last cascade fades over the last tenth of that distance. A uniform colour over an orthographic board means it lies in one cascade, which CascadeCount 1 suits.

- `Overdraw`: The fragments each pixel accepted before depth rejection: 1 blue, 2 cyan, 3 green, 4 yellow, 5 or more red; black where none.

- `LightCount`: The number of lights each pixel's light cluster holds, as a heat map.

## SceneDiffAgainst

What scene.diff compares the open scene with.

- `Saved`: The scene's file on disk.

- `Revision`: The scene as it was at an earlier revision the undo history holds.

## SceneEntityChangeKind

How one entity changed.

- `Created`: The entity did not exist before.

- `Destroyed`: The entity no longer exists.

- `Modified`: The entity exists in both and differs.

## SceneTarget

Which scene a request reads or changes (§13.4 "Target").

- `Edit`: The scene open in the editor; changes are undoable commands.

- `Play`: The running play session's scene; changes are transient and never undone.

## SceneTemplate

What a new scene starts with.

- `Empty`: No entities.

## SceneTreeFormat

How scene.tree reports the hierarchy.

- `Text`: A token-efficient outline with names, short ids, components and translations.

- `Json`: A flat list of entries in canonical order, with depth and parent.

## ScriptEvaluationContext

The scene and VM used by script.eval.

- `Edit`: Fresh read-only editor VM without lifecycle callbacks.

- `Play`: The live Play VM, subject to lockstep ownership.

## ScriptKind

Authenticated kind of the script's load-time return value.

- `Behaviour`: An attachable Script.Define class.

- `Module`: A required module.

- `TestSuite`: A Test.Suite definition.

## ScriptTemplate

The shipped template used to create a source file.

- `Behaviour`: An attachable behaviour class.

- `Module`: A reusable required module.

- `Test`: A Test.Suite source.

## SoundWave

The oscillator of a sound effect layer.

- `Sine`: A pure sine tone.

- `Square`: A square wave with an adjustable duty cycle: hollow, chiptune-like.

- `Triangle`: A triangle wave: soft, flute-like.

- `Saw`: A sawtooth wave: bright and buzzy.

- `Noise`: Seeded noise, resampled at the frequency: hiss, hits and explosions.

## Space

Coordinates relative to the entity's orientation or the world axes.

- `Local`: Entity orientation.

- `World`: World axes.

## SsaoQuality

The sample count of screen-space ambient occlusion (GTAO).

- `Low`: Fewest samples: fastest, noisiest.

- `Medium`: Balanced sample count (the default).

- `High`: Most samples: smoothest, slowest.

## TerrainTool

What a terrain stroke does.

- `Raise`: Raises the surface under the brush by Strength times the falloff, in metres.

- `Lower`: Lowers the surface under the brush by Strength times the falloff, in metres.

- `Smooth`: Moves each sample under the brush towards the mean of its neighbours.

- `Flatten`: Moves the surface under the brush towards Height.

- `Paint`: Paints Layer's weight into the splat map under the brush, keeping the weights normalized.

## TestIsolation

How much state the cases of a test suite share.

- `Suite`: Every case of the suite runs in one play session.

- `Case`: Every case runs in a fresh play session.

## TestMode

A run mode a test suite runs in.

- `Editor`: The editor's play mode, headless lockstep included.

- `Release`: An exported Release build.

- `Dist`: An exported Dist build.

## TestPauseOnError

A suite's PauseOnError override. A script error never pauses a test run, so no value changes the run's time.

- `Inherit`: Use the project's Scripting.PauseOnError, which pauses ordinary play only.

- `Pause`: Accepted for compatibility; the test run still keeps running after a script error.

- `Continue`: Keep running after a script error, as every test run does.

## TextAlignment (enum)

How the lines of a text element are aligned to each other.

- `Left`: Lines start at the left edge of the text block.

- `Center`: Lines are centred in the text block.

- `Right`: Lines end at the right edge of the text block.

## TextSpace (enum)

Where a text element is placed.

- `Screen`: On the screen, positioned by Anchor, Pivot and Offset.

- `World`: In the world, at the entity's transform.

## TextureCompression

Whether an imported texture or environment is stored block-compressed for the GPU.

- `Auto`: Compress by usage: BC7 for colour and linear data, BC5 for normal maps; BC6H for an environment's cubes.

- `None`: Keep the texels uncompressed (RGBA8, or RGBA16F for an environment): sprite, tile and UI art.

## TextureCoordinateSet

The texture coordinates a material map samples (glTF textureInfo.texCoord).

- `UV0`: The mesh's first texture coordinates.

- `UV1`: The mesh's second texture coordinates (glTF TEXCOORD_1); (0, 0) on a mesh without them.

## TextureUsage

How a texture's texels are interpreted.

- `Color`: Colour data (base colour, emissive): sRGB, mips filtered in linear light.

- `Linear`: Linear data (metallic-roughness, occlusion, masks): mips filtered on the stored values.

- `NormalMap`: Tangent-space normals: linear, every generated mip renormalized to unit vectors.

## Tonemapper

The curve that maps HDR scene colours to the display.

- `AgX`: AgX: filmic, hue-preserving highlight roll-off (the default).

- `ACES`: The ACES filmic approximation: higher contrast and saturation.

- `PbrNeutral`: Khronos PBR Neutral: keeps base colours faithful up to the highlights.

- `Linear`: No curve: exposure only, then clamped to the display range.

## UiCanvasRenderMode (enum)

Where a UI canvas is drawn: over the scene image, or in the world.

- `Screen`: Over the scene image, after the scene and its texts, laid out in scene pixels.

- `World`: As a rectangle in the world at the canvas entity's transform, depth-tested against the scene, laid out in its own canvas pixels.

## UiCrossAlignment

Where each child of a container sits across its main axis.

- `Start`: At the start of its line: the top (Horizontal) or the left edge (Vertical).

- `Center`: Centred across the main axis.

- `End`: At the end of its line: the bottom (Horizontal) or the right edge (Vertical).

- `Stretch`: Stretched to fill its line's cross size (a Grid child: its cell's).

## UiImageFilter (enum)

How a UI image samples its texture.

- `Point`: The nearest texel: pixel art, and a 1:1 image reproduces its texels exactly.

- `Linear`: Bilinear filtering between texels.

## UiImageType (enum)

How a UI image fills its element's rectangle.

- `Simple`: The sprite frame is stretched over the rectangle.

- `Sliced`: The frame's 9-slice borders keep their size; the edges and the centre stretch.

## UiLayoutDirection (enum)

The main axis of a UI layout container.

- `Horizontal`: Children follow each other from left to right.

- `Vertical`: Children follow each other from top to bottom.

## UiLayoutKind (enum)

How a UI layout container places its children.

- `Stack`: One after another along Direction, each at its own size.

- `Grid`: In cells of CellSize, Columns cells per row (Horizontal) or per column (Vertical).

- `Flex`: A stack whose children grow into free space and shrink when they overflow, wrapping with Wrap.

## UiMainAlignment

Where a container's children sit along its main axis when they do not fill it.

- `Start`: At the start: the left edge (Horizontal) or the top (Vertical).

- `Center`: Centred along the main axis.

- `End`: At the end: the right edge (Horizontal) or the bottom (Vertical).

## UiScaleMode

How a UI canvas maps its units to scene pixels.

- `ConstantPixelSize`: ScaleFactor scene pixels per canvas unit, whatever the scene resolution.

- `MatchHeight`: The canvas is ReferenceHeight units high, so it scales with the scene's height.

## UiTextVerticalAlignment (enum)

Where a UI text block sits in its element's rectangle.

- `Top`: The first line starts at the rectangle's top edge.

- `Middle`: The block is centred vertically in the rectangle.

- `Bottom`: The last line ends at the rectangle's bottom edge.

## UiWidgetKind

The UI widget ui.createWidget builds.

- `Canvas`: A UI canvas, the root of a UI tree.

- `Panel`: An image stretched over its parent: a background for a group of elements.

- `Image`: A 100 by 100 image.

- `Text`: A centred text.

- `Button`: A button with an image and a Label child.

- `Toggle`: A toggle box with a Check child that shows while it is on, and a Label child.

- `Slider`: A horizontal slider with Fill and Handle children.

- `ScrollView`: A vertical scroll view whose Content child stacks its children.

- `TextInput`: A single-line text field with a Text child that shows what is typed and a Placeholder child.

## ValidationScope

What project.validate checks.

- `Project`: The settings and every scene file under Assets/ (each open scene as it is in memory).

- `Scene`: The scene the request addresses only (the active scene without a scene target).

## ViewportView

Which view viewport.screenshot renders.

- `Scene`: The target scene through the editor's scene-view camera (the editor only).

- `Game`: The target scene through its primary camera: what the game shows.

## AnimationEvent

`{Name: string, Layer: number, Clip: AssetRef, Time: number}`

An animation clip's named event that an Animator layer's time passed: its name, the 1-based layer, the layer's clip and the event's time in it.

## CollisionContact

`{Point: vector, Normal: vector, RelativeSpeed: number, Collider: Entity, OtherCollider: Entity}`

First contact from the receiving body's side, with collider identities for compounds.

## FieldOptions

`{Min: number?, Max: number?, Step: number?, Tooltip: string?}`

Editor metadata and numeric validation bounds for a field.

## RaycastHit

`{Entity: Entity, Body: Entity, Point: vector, Normal: vector, Distance: number}`

A query hit distinguishes the collider entity from its body owner.

## ScriptError

`{id: number, kind: string, script: string, line: number, column: number, message: string, callback: string, entity: string, entityName: string, tick: number, count: number, jsonPointer: string, traceback: {{script: string, line: number, ["function"]: string}}}`

Owned script diagnostic with exclusive occurrence cursor.

## ScriptInstance

`{Entity: Entity}`

Every behaviour instance owns an Entity identity and copies its declared field defaults.

## TestAudioLevels

`{RmsLeft: number, RmsRight: number, Peak: number}`

Stereo RMS and absolute peak of the requested simulation sample interval.

## TestCaseOptions

`{TimeoutTicks: number?}`

Positive tick timeout overriding the suite default.

## TestExtraction

`{Alpha: number, Frame: number}`

The last completed render extraction: its interpolation Alpha, and Frame, the number of extractions completed so far.

## TestSuite (type)

`{Name: string, Body: () -> (), Options: TestSuiteOptions}`

Authenticated suite constructor result.

## TestSuiteOptions

`{CaseTimeoutTicks: number?}`

Default positive tick timeout for cases in a suite.

## UiElementRect

`{Min: vector, Max: vector, Canvas: Entity}`

A UI element's laid-out rectangle: its top-left (Min) and bottom-right (Max) corners as (x, y, 0), x right and y down, in scene pixels for an element of a Screen canvas and in its canvas's own pixels for one of a World canvas, and the canvas it belongs to (Canvas).
