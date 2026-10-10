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

Returns window coordinates as vector(width, height, 0).

All modes; Runtime|Test; pure/read.

### Application.IsEditor

`() -> boolean`

True in the editor; false in the standalone Runtime.

All modes; Runtime|Test; pure/read.

### Application.IsFocused

`() -> boolean`

Returns the current native-window focus state.

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

Loads a path or hexadecimal reference; missing references return nil and loading failures raise a located error.

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

Play a non-spatial one-shot when position is nil, otherwise spatial; volume defaults 1 and group Sfx.

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

Adds UTF-8 text at a world position with the default 16-pixel size.

All modes; Runtime|Test; mutates.

## Field

Persistent behaviour field schemas.

### Field.Array

`(element: FieldDescriptor) -> FieldDescriptor`

Declare an empty array whose element keeps its complete nested field schema.

All modes; LoadTime|Runtime|Test; pure/read.

### Field.Asset

`(type: string) -> FieldDescriptor`

Declare an asset reference restricted to the named asset type, with a null default.

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

Returns cursor window coordinates as vector(x, y, 0).

All modes; Runtime|Test; pure/read.

### Input.GetScrollDelta

`() -> vector`

Returns accumulated scroll, right-positive and up-positive.

All modes; Runtime|Test; pure/read.

### Input.IsActionDown

`(name: string) -> boolean`

Returns a configured action's held state; an unknown name raises INPUT_UNKNOWN_ACTION.

All modes; Runtime|Test; pure/read.

### Input.IsActionPressed

`(name: string) -> boolean`

Returns a configured action's press edge.

All modes; Runtime|Test; pure/read.

### Input.IsActionReleased

`(name: string) -> boolean`

Returns a configured action's release edge.

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

## Log

Script-channel logging with authored source, line, entity and simulation tick; arguments use tostring and tab separators.

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

Creates an independent local stream; allowed in read-only evaluation.

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

Creates immediately with a seeded ID and default name Entity. Runs OnCreate before returning; use IsValid if it may destroy itself.

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

Instantiates through the shared prefab path and invokes OnCreate for the complete subtree before returning.

All modes; Runtime|Test; mutates.

### Scene.Load

`(scene: AssetRef, parameters: {[string]: any}?) -> ()`

Requests a scene/VM replacement at the end of the frame with copied JSON parameters; never destroys the calling VM inline.

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

Feature test suites, assertions, waits and deterministic input.

### Test.CaptureAudio

`(ticks: number) -> TestAudioLevels`

Capture exactly the requested current-and-following ticks; yield until grouped audio is ready.

All modes; Test; mutates.

### Test.Case

`(name: string, fn: () -> (), options: TestCaseOptions?) -> ()`

Register a case while collecting the suite body.

All modes; Test; pure/read.

### Test.Expect

`(condition: boolean, message: string?) -> ()`

Record a failed condition and continue the case.

All modes; Test; pure/read.

### Test.ExpectEqual

`(a: any, b: any, message: string?) -> ()`

Record a failed Luau equality comparison.

All modes; Test; pure/read.

### Test.ExpectNear

`(a: number | vector, b: number | vector, epsilon: number?, message: string?) -> ()`

Compare finite numbers or vector components with an absolute epsilon, default 0.00001.

All modes; Test; pure/read.

### Test.ExpectScriptError

`(pattern: string, withinTicks: number) -> ()`

Claim one nonfatal error occurrence in the current case using a Luau message pattern.

All modes; Test; pure/read.

### Test.Fail

`(message: string) -> ()`

End this case as failed, even inside pcall.

All modes; Test; pure/read.

### Test.GetExtractedPosition

`(entity: Entity) -> vector?`

Read an entity's position in the last render snapshot.

All modes; Test; pure/read.

### Test.GetLastExtraction

`() -> TestExtraction`

Read the last completed render extraction.

All modes; Test; pure/read.

### Test.GetScriptErrors

`(since: number?) -> {ScriptError}`

Read occurrences newer than an exclusive error cursor; claimed errors remain visible.

All modes; Test; pure/read.

### Test.GetStateHash

`() -> string`

Read the canonical simulated state hash.

All modes; Test; pure/read.

### Test.InjectAction

`(name: string, state: string?, value: number?) -> ()`

Queue a named button state or analog value for the next unapplied input tick.

All modes; Test; mutates.

### Test.InjectGamepad

`(index: number, buttonOrAxis: string, stateOrValue: string | number) -> ()`

Queue a gamepad button state or engine-convention axis value.

All modes; Test; mutates.

### Test.InjectKey

`(key: string, state: string) -> ()`

Queue a key state for the next unapplied input tick.

All modes; Test; mutates.

### Test.InjectMouse

`(button: string, state: string, position: vector?) -> ()`

Queue a mouse button and optional finite XY position.

All modes; Test; mutates.

### Test.ReloadScript

`(asset: AssetRef) -> ()`

Synchronously reload a script in an editor test.

EditorOnly; Test; mutates.

### Test.Screenshot

`(name: string) -> ()`

Capture a screenshot through the owning host.

All modes; Test; mutates.

### Test.Skip

`(reason: string) -> ()`

End this case as skipped without erasing earlier failures.

All modes; Test; pure/read.

### Test.Suite

`(name: string, body: () -> (), options: TestSuiteOptions?) -> TestSuite`

Construct a suite without executing its body.

All modes; LoadTime|Runtime|Test; pure/read.

### Test.WaitTicks

`(ticks: number) -> ()`

Yield a test thread for simulation ticks.

All modes; Test; pure/read.

### Test.WaitUntil

`(predicate: () -> boolean, timeoutTicks: number) -> ()`

Wait for a predicate through a bounded simulation deadline.

All modes; Test; pure/read.

## Time

Phase-aware simulation time; use fixed time for deterministic gameplay.

### Time.GetDeltaTime

`() -> number`

Returns scaled delta seconds for the current fixed or frame phase.

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

## AssetRef

An opaque immutable asset handle; resolve metadata through Assets.

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

### Behaviour.OnCollisionEnter

`(self: ScriptInstance, other: Entity, contact: CollisionContact) -> ()`

Called when a collision begins, with the other entity and contact.

All modes; Runtime|Test; mutates.

### Behaviour.OnCollisionExit

`(self: ScriptInstance, other: Entity) -> ()`

Called when the corresponding collision or trigger transition occurs.

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

### Behaviour.OnTriggerExit

`(self: ScriptInstance, other: Entity) -> ()`

Called when the corresponding collision or trigger transition occurs.

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

Ray through a viewport pixel center, origin top-left, using the rendered camera pose.

All modes; Runtime|Test; pure/read.

### Camera.VerticalFov

`number`

The vertical field of view of the perspective projection, in degrees.

All modes; Runtime|Test; pure/read; setter mutates.

### Camera.WorldToScreen

`(self: Camera, point: vector) -> vector`

Project to top-left pixel coordinates and reverse-Z depth using the rendered camera pose.

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

### CharacterController.IsGrounded

`(self: CharacterController) -> boolean`

Whether the last character update found supporting ground.

All modes; Runtime|Test; pure/read.

### CharacterController.Layer

`string`

Physics layer name from project settings.

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

A UUID and scene generation identifying an entity; no native address is exposed.

### Entity.AddComponent

`(self: Entity, name: string, values: {[string]: any}?) -> any`

Validate and add a component; Script instances are synchronized before returning.

All modes; Runtime|Test; mutates.

### Entity.AddTag

`(self: Entity, tag: string) -> ()`

Add a nonempty tag if absent.

All modes; Runtime|Test; mutates.

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

Copy live children in sibling order.

All modes; Runtime|Test; pure/read.

### Entity.GetComponent

`((self: Entity, name: "Transform") -> Transform?) & ((self: Entity, name: "MeshRenderer") -> MeshRenderer?) & ((self: Entity, name: "Camera") -> Camera?) & ((self: Entity, name: "DirectionalLight") -> DirectionalLight?) & ((self: Entity, name: "PointLight") -> PointLight?) & ((self: Entity, name: "SpotLight") -> SpotLight?) & ((self: Entity, name: "Environment") -> Environment?) & ((self: Entity, name: "PostProcess") -> PostProcess?) & ((self: Entity, name: "Text") -> Text?) & ((self: Entity, name: "RigidBody") -> RigidBody?) & ((self: Entity, name: "BoxCollider") -> BoxCollider?) & ((self: Entity, name: "SphereCollider") -> SphereCollider?) & ((self: Entity, name: "CapsuleCollider") -> CapsuleCollider?) & ((self: Entity, name: "MeshCollider") -> MeshCollider?) & ((self: Entity, name: "CharacterController") -> CharacterController?) & ((self: Entity, name: "AudioSource") -> AudioSource?) & ((self: Entity, name: "AudioListener") -> AudioListener?) & ((self: Entity, name: "Script") -> Script?)`

Get a component proxy, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.GetParent

`(self: Entity) -> Entity?`

Get the parent, or nil for a root.

All modes; Runtime|Test; pure/read.

### Entity.GetScript

`(self: Entity) -> any`

Get the existing behaviour instance or nil; cast to the module's exported instance type.

All modes; Runtime|Test; pure/read.

### Entity.GetTags

`(self: Entity) -> {string}`

Copy tags in insertion order.

All modes; Runtime|Test; pure/read.

### Entity.GetWorldBounds

`(self: Entity) -> (vector?, vector?)`

Get this entity's render mesh world AABB; nil without a mesh.

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

The entity name.

All modes; Runtime|Test; pure/read; setter mutates.

### Entity.PointLight

`PointLight?`

The entity's PointLight component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.PostProcess

`PostProcess?`

The entity's PostProcess component, or nil when absent.

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

### Entity.SetActive

`(self: Entity, active: boolean) -> ()`

Change local active state and dispatch effective lifecycle transitions before returning.

All modes; Runtime|Test; mutates.

### Entity.SetParent

`(self: Entity, parent: Entity?, keepWorld: boolean?) -> ()`

Reparent to the last sibling, preserving world pose by default; synchronize effective lifecycle transitions.

All modes; Runtime|Test; mutates.

### Entity.SphereCollider

`SphereCollider?`

The entity's SphereCollider component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.SpotLight

`SpotLight?`

The entity's SpotLight component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Text

`Text?`

The entity's Text component, or nil when absent.

All modes; Runtime|Test; pure/read.

### Entity.Transform

`Transform`

The entity's Transform component, or nil when absent.

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

## PointLight

A light shining in every direction from the entity (no shadows).

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

The radius of the emitting sphere, in metres; widens highlights.

All modes; Runtime|Test; pure/read; setter mutates.

## PostProcess

The scene's exposure, tone mapping, ambient occlusion, bloom and anti-aliasing (one per scene).

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

### PostProcess.FxaaEnabled

`boolean`

Whether FXAA anti-aliasing is applied.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsaoEnabled

`boolean`

Whether screen-space ambient occlusion (GTAO) is applied.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsaoIntensity

`number`

The strength of the ambient occlusion.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsaoQuality

`SsaoQuality`

The ambient occlusion sample count.

All modes; Runtime|Test; pure/read; setter mutates.

### PostProcess.SsaoRadius

`number`

The world-space radius within which ambient occlusion is sampled, in metres.

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

The friction coefficient of the body's surfaces.

All modes; Runtime|Test; pure/read; setter mutates.

### RigidBody.GetAngularVelocity

`(self: RigidBody) -> vector`

Read current angular velocity.

All modes; Runtime|Test; pure/read.

### RigidBody.GetLinearVelocity

`(self: RigidBody) -> vector`

Read current linear velocity.

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

Physics layer name from project settings.

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

Set a Kinematic target for the next fixed step.

All modes; Runtime|Test; mutates.

### RigidBody.Restitution

`number`

Bounciness, from no bounce (0) to a perfectly elastic bounce (1).

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

## Script

Attaches a Behaviour script to the entity (one per entity).

### Script.ExecutionOrder

`number`

The script's place in callback order: lower values run first, ties in canonical entity order.

All modes; Runtime|Test; pure/read; setter mutates.

### Script.Fields

`{[string]: any}`

Overrides of the script's declared fields by name, each checked against the field's declared type; an unknown or mismatching override is kept and reported, and the field uses its declared default.

All modes; Runtime|Test; pure/read; setter mutates.

### Script.Script

`AssetRef?`

The Behaviour script to run.

All modes; Runtime|Test; pure/read; setter mutates.

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

## TaskHandle

An opaque coroutine handle owned by one script VM.

## Text

Draws a text string on the screen or in the world.

### Text.Alignment

`TextAlignment`

How the lines are aligned to each other.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Anchor

`vector`

The screen point the text is placed at, normalized to the viewport (Screen space).

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

A further offset from the anchor, in pixels at the 1080p reference resolution.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Pivot

`vector`

The point of the text block that sits on the anchor, normalized to the block.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Size

`number`

The text height in pixels at the 1080p reference resolution.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Space

`TextSpace`

Whether the text is placed on the screen or in the world.

All modes; Runtime|Test; pure/read; setter mutates.

### Text.Text

`string`

The UTF-8 text; a line feed starts a new line.

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

## AlphaMode

How a material's base colour alpha is used (the glTF 2.0 alpha modes).

- `Opaque`: Alpha is ignored: the surface is fully opaque.

- `Mask`: Pixels whose alpha is below AlphaCutoff are discarded; the others are opaque.

- `Blend`: The surface is blended over what lies behind it by its alpha.

## AssetCreateType

What asset.create makes.

- `Material`: A .material file: a PBR material.

- `Scene`: A .scene file: an empty scene named after the file.

- `Prefab`: A .prefab file with one root entity named after the file.

- `SoundEffect`: A .sfx sound effect: synthesized layers of oscillators, imported as an AudioClip.

- `Folder`: A folder below Assets/.

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

- `Environment`: An image-based lighting environment (.hdr).

- `AudioClip`: A sound (WAV, FLAC, MP3, or a .sfx preset).

- `Script`: A Luau script (.luau).

- `Font`: A font (TTF, OTF) as a signed distance field atlas.

- `Replay`: An input replay (.replay).

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

- `Kinematic`: Moved by script or animation; pushes dynamic bodies but is not pushed back.

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

## ExportConfiguration

The build configuration an export targets (§2.2).

- `Debug`: Engine asserts on, unoptimized, with the automation server.

- `Release`: Engine asserts on, optimized, with the automation server.

- `Dist`: The shipping configuration: asserts off, no automation server.

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

- `MouseMove`: The cursor moving to a position (window coordinates).

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

Which ticks of a play.step extract the game view.

- `Every`: Every tick, one tick per frame.

- `Last`: Only the last tick; the ticks before it run as fast as the frame budget allows.

- `None`: No tick.

## PrefabOverrideKind

What a prefab override changes on one prefab entity of an instance.

- `Field`: One field of one component; Value is the field's value.

- `AddComponent`: A component added to the prefab entity; Value is the whole component.

- `RemoveComponent`: A component of the prefab entity removed from it; Value is null.

- `EntityKey`: An entity key (Name, Active or Tags); Value is the key's value.

## ProjectTemplate

What a new project starts with.

- `Empty`: The folder skeleton, .luaurc, .gitignore and an AGENTS.md stub; no scene.

- `Basic3D`: A camera with audio listener, sun, Studio environment and ground collider in a ready-to-edit scene.

## ProjectionType

How a camera projects the scene onto the screen.

- `Perspective`: Perspective projection with the vertical field of view VerticalFov.

- `Orthographic`: Parallel projection whose half height is OrthographicSize.

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

Whether a script error pauses one test suite.

- `Inherit`: Use the project's Scripting.PauseOnError.

- `Pause`: Pause the suite on a script error.

- `Continue`: Keep running after a script error.

## TextAlignment

How the lines of a text element are aligned to each other.

- `Left`: Lines start at the left edge of the text block.

- `Center`: Lines are centred in the text block.

- `Right`: Lines end at the right edge of the text block.

## TextSpace

Where a text element is placed.

- `Screen`: On the screen, positioned by Anchor, Pivot and Offset.

- `World`: In the world, at the entity's transform.

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

## ValidationScope

What project.validate checks.

- `Project`: The settings and every scene file under Assets/ (the open scene as it is in memory).

- `Scene`: The open scene only.

## ViewportView

Which view viewport.screenshot renders.

- `Scene`: The target scene through the editor's scene-view camera (the editor only).

- `Game`: The target scene through its primary camera: what the game shows.

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

Last completed render extraction interpolation and frame.

## TestSuite

`{Name: string, Body: () -> (), Options: TestSuiteOptions}`

Authenticated suite constructor result.

## TestSuiteOptions

`{CaseTimeoutTicks: number?}`

Default positive tick timeout for cases in a suite.
