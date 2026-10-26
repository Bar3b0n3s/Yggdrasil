# NativeTest

NativeTest is M19's native test game (stream N-E, ADR 0030 decision 35). A C++ native module in `Native/` runs all of its gameplay. The game has no gameplay script. It was built only through the editor's automation (the MCP bridge), and `Automation/Provenance.json` and `Automation/BuildLog.jsonl` record every write.

## Rules this game keeps

- **No hash-sensitive components.** No scene holds a `Script`, `Prefab`, `PrefabLink`, `PostProcess` or physics component (`RigidBody`, any collider, `CharacterController`). Later milestones re-record every game that holds one, and this game must keep its hashes. The Basic3D template's `Post Process` entity was deleted, and the `RigidBody` and `BoxCollider` of its `Ground` were removed. Test scripts are Script assets that the test suites run, not components, so they are allowed.
- **One native module.** The module is `Native/NativeTest.h` and `Native/NativeTest.cpp`. Both are written only with `native.write`, and the sources stay clang-format clean in the engine's style, because `Format.py --check` reads them. A module source includes its own header by its project-relative path, `#include "Native/NativeTest.h"`; the native lint refuses a bare `"NativeTest.h"`.
- **Deterministic.** The module uses DetMath through `INativeStepContext::AngleAxis`, never CRT trigonometry. It reads no wall clock and no randomness, keeps no mutable statics, and reads only the latched step input.
- **"Allow native code" must be on.** `native.build`, `native.load` and an export of this game all need the user's "Allow native code" preference, which is set in the editor's Native panel; no method sets it. Build and load the module before any `--strict` run, or the scene's `Native.*` components report `NATIVE_COMPONENT_UNREGISTERED`.

## Scene

`Assets/Scenes/Main.scene` is `StartScene` and the only entry of `Export.BuildScenes`.

| Entity | Components | Role |
|---|---|---|
| `/Camera` | Camera (perspective, 60°), AudioListener | Looks at the play area from (0, 2.8, 6.5). |
| `/Sun` | DirectionalLight (intensity 2.5, shadows) | Key light. |
| `/Environment` | Environment (Studio, intensity 0.5) | Image-based light and skybox. |
| `/Ground` | MeshRenderer (cube, `Ground.material`) | A 10 × 10 m slab. Purely visual: it has no body. |
| `/Spinner` | MeshRenderer (cube, `Spinner.material`), `Native.Spinner` {DegreesPerSecond 90, Axis [0, 1, 0]} | A bar that turns about Y while Spin is held. |
| `/WheelMount` | Transform (turned 90° about X) | Stands the Wheel up. |
| `/WheelMount/Wheel` | MeshRenderer (cube plate, `Spinner.material`), `Native.Spinner` {DegreesPerSecond -45, Axis [0, 1, 0]} | A plate that turns the other way in the mount's plane. |
| `/Bumper` | MeshRenderer (sphere, `Bumper.material`), `Native.Bumper` {Levels 4, BaseHeight 0.5, StepHeight 0.5} | A ball that climbs one step per Bump press. |

The materials are `Assets/Materials/Spinner.material`, `Bumper.material` and `Ground.material`.

## Input actions

| Action | Type | Bindings | Effect |
|---|---|---|---|
| `Spin` | Button | `Key.Space`, `Gamepad.South` | While it is held, every `Native.Spinner` turns. |
| `Bump` | Button | `Key.B`, `Gamepad.East` | Each press (not each held tick) raises every `Native.Bumper` one level. |

`Simulation.Seed` is 19 and `FixedHz` is 60.

## Native module (`Native/NativeTest.h`, `Native/NativeTest.cpp`)

- **`Native.Spinner`** (component): `DegreesPerSecond` (float, -720 to 720 deg/s), `Axis` (vec3), `AngleDegrees` (float, 0 to 360) and `SpinTicks` (uint32).
- **`Native.Bumper`** (component): `Presses` (uint32), `Levels` (uint32, 1 to 16), `BaseHeight` (float, m) and `StepHeight` (float, 0 to 10 m).
- **`Totals`** (resource, hashed in the native block): `SpinTicks` and `Presses` for the play session. `native.status` reports it in play as `playResources`.
- **System `Spin`** (NativeStep): while `Spin` is down, it adds `DegreesPerSecond × FixedDelta` to `AngleDegrees` and wraps the angle into [0, 360). It then counts the tick, writes `AngleAxis(angle, Axis)` as the entity's local rotation and adds 1 to `Totals.SpinTicks`.
- **System `Bump`** (NativeStep): on `WasActionPressed("Bump")`, it adds 1 to `Presses` and sets the entity's local Y to `BaseHeight + (Presses % Levels) × StepHeight`. It also adds 1 to `Totals.Presses`.

Rebuilding after a source change:
1. `native.write`
2. `native.build`
3. `native.load`
4. Re-record the replay when its hash changes.

Luau cannot read `Native.*` components (`Entity:HasComponent("Native.Spinner")` raises NotFound), so the suites observe the Transforms the systems write.

## Test suites (`Testing.Suites`)

| Suite | Script | Scene, isolation | Modes | Cases |
|---|---|---|---|---|
| `Spinner` | `Assets/Tests/Spinner.test.luau` | Main, Case | Editor, Release, Dist | stays still without input; turns at its speed while Spin is held (Spinner 90°, Wheel -45° after 60 ticks); stops turning when Spin is released; wraps its angle after a full turn (250 ticks gives 15°) |
| `Bumper` | `Assets/Tests/Bumper.test.luau` | Main, Case | Editor, Release, Dist | rests at its base height without input; climbs one step per press; counts a held Bump once; drops back after its last level |
| `Record.SpinAndBump` | `Assets/Tests/Record/SpinAndBump.test.luau` | Main, Suite | Editor | spins and bumps: the recording suite. It holds Spin over ticks 10 to 100 and taps Bump at ticks 40, 70 and 130. It ends with the Spinner at 135° and the Bumper at 2.0 m. |

## Replays (`Testing.Replays`: `Assets/Tests/Replays/*.replay`)

`Assets/Tests/Replays/SpinAndBump.replay` was recorded with `test_run {filter: "Record.SpinAndBump", record: "Assets/Tests/Replays/SpinAndBump.replay"}`.
- **Recording:** format version 3, 191 ticks, 8 input events and 3 checkpoints (ticks 60, 120 and 180). The final state hash is `35a099dd25e3317c`. The input check diverged at tick 60.
- **Verification:** `input_replay {verify: true, strictHash: true}` passed twice, both times with `stateHash` `35a099dd25e3317c`.
- **Input check:** `input_replay {verify: true, strictHash: true, withoutInput: true}` fails at checkpoint tick 60, where the recording's hash is `f4d9becabe6b6a4d` and the run without input gives `6592cb07d32e33da`.
- **Dist:** the testing Dist export's `--feature-test` run reproduced `35a099dd25e3317c`.

To re-record after a change to the module or the scene:
1. Run the recording command above with the same filter.
2. Verify the replay twice.
3. Run the without-input check again.

## Done checks

- `test_run {}`: 10 of 10 rows pass (9 cases and the replay).
- `project_validate {}` and `script_check {}` report no diagnostics.
- `project_upgrade {dryRun: true}` changes nothing.
- `project_export {config: "Dist", smokeTest: true}` reports `smokeTestRan: true`, `smokeTestExitCode: 0` and `nativeModule: true`, with the module linked into the exported Runtime.
- The testing Dist export with `featureTest: true` passes.
