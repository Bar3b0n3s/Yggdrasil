# Rolling Ball

The second demo game of the backlog program (Roadmap M27, `Docs/Decisions/0038-m27-rolling-ball.md`, "ADR 0038" below): a three-level physics ball game built entirely through the editor's automation (the engine MCP tools), with its transcript in `Automation/BuildLog.jsonl` and its provenance in `Automation/Provenance.json`. Every engine-consumed file was written by an automation method; this file is the only one written by hand.

Status: session S1 (2026-11-14) built the whole game: the three levels, the kit, the scripts, the graph, the HUD, the sounds, the look, the five suites and the three replays, and met ADR 0038 decision 8's checklist. Session S2 (2026-11-14, engine 0.4.0+6366f1ca7228) found nothing to upgrade, ran `test_run {}` 18/18 at S1's hashes, restored decision 4's bottom-left `HelpText` over a new `BottomBar` carried into `HUD.prefab` with `prefab_apply` (undoing S1's workaround of friction F04), took the ten look captures again, re-recorded and verified the three replays (the HUD is hashed), and met decision 8's checklist again. Session S3 (2026-11-14, engine 0.4.0+b9d2dae34703) found nothing to upgrade, ran `test_run {}` 18/18 at S2's hashes, fixed the review findings R01 to R14 of the lead's review of the game after S2 (a `Leaving` flag so the game requests one scene transition at most, an `Actions` module for the action names, the banner shown exactly `BannerTicks` ticks, a platform's phased start, the `Track.Project` window case, the autopilot's par read from the scene, Restart and the spawn rule tested, the transition to Level 3 as its own case, Level 3's `entity_bounds`), re-recorded and verified the three replays (their hashes did not change: see "Replays"), took the ten look captures again and met decision 8's checklist again (`test_run {}` 26/26). Session S4 (2026-11-15, engine 0.4.0+6278a3006b5f), a short fourth session the product owner approved, found nothing to upgrade, ran `test_run {}` 26/26 at S3's hashes, and fixed the milestone review's findings G1 to G6 on the suites: a case for the CHECKPOINT banner's `BannerTicks` ticks (G1), Level 2's next level checked as Level 3 and Level 3's arrival as a case of its own (G2), the support check's failure report naming solid colliders only (G3), the HUD's `LevelText` and `ParText` read on each level (G4), the phased platform's first-tick speed (G5), and a `Frames` suite on a ScriptedClock that presses Restart in the frame the finished level's delayed load falls due (G6). Each new or strengthened check was shown failing against the old behaviour with a temporary version of its suite. Only `RollingBall.test.luau`, the new `Frames.test.luau` and `Testing.Suites` changed, so the replays are unchanged and verify at their committed hashes; decision 8's checklist items 1 to 5, 10 and 11 met again (`test_run {}` 33/33), items 6 to 9 standing from S3. What the lead runs between sessions (the games suite, the provenance audit, the soak) is not run by the builder.

## Project settings

- `Name` RollingBall; `StartScene` `Assets/Scenes/Level1.scene`; `Export.BuildScenes` the three level scenes; `Export.Exclude` `Assets/Tests/**`; `Version` 1.0.0; `Company` Engine Demos; no icon; `PruneUnreferenced` false.
- `Window`: "Rolling Ball", 1280 x 720, VSync, not fullscreen, not resizable. Every deterministic session runs at this size.
- `Simulation`: FixedHz 60, MaxStepsPerFrame 5, Seed 27, MaxEntities 65536. The game draws no random number.
- `Physics`: gravity (0, -9.81, 0); layers `Default`, `Ball`, `Track`, `Trigger`; only Ball-Track and Ball-Trigger collide.
- `Rendering`: ShadowMapSize 2048, SsaoHalfResolution false.
- `Scripting`: MemoryLimitMB 256, CallbackBudgetMs 1000, PauseOnError true, BlockPlayOnTypeErrors true.
- `Ui`: Navigation false, StickNavigation false (the HUD has no widget).
- `Testing`: the six suites below (`Frames` added in S4, G6); `Replays` `Assets/Tests/Replays/*.replay`; CaseTimeoutTicks 600; SuiteTickLimit 36000; CoverageGates false.

### Input actions

| Action | Type | Bindings |
|---|---|---|
| `MoveX` | Axis | Positive Key.D, Key.Right; Negative Key.A, Key.Left; Gamepad.LeftX |
| `MoveZ` | Axis, positive forward | Positive Key.W, Key.Up; Negative Key.S, Key.Down; Gamepad.LeftY, `Invert` false |
| `Restart` | Button | Key.R, Gamepad.Start |

`MoveX` and `MoveZ` steer relative to the ball's own `Heading` (fixed-step state), never the camera. Every script reads and every suite injects these names only through `Assets/Scripts/Actions.luau` (S3, R09).

## Scenes

`Assets/Scenes/Level1.scene`, `Level2.scene` and `Level3.scene`, one per level, each with the roots:

- `/Camera`: perspective camera (VerticalFov 60, NearClip 0.1, FarClip 600, Primary, Clear Skybox) with `FollowCamera.luau` (`Ball` = `/Ball`); it is also the audio listener.
- `/Sun`: `DirectionalLight` (decision 6 values).
- `/Environment`: `Environment` and `PostProcess` (the look below).
- `/Backdrop`: the terrain backdrop (decision 5).
- `/Level`: `RigidBody {Type: Static, Layer: Track, Friction: 0.8, Restitution: 0}` at the origin. Every track piece below it is collider-only and joins one static compound. Moving platforms, checkpoints and the goal have their own bodies. `/Level/Path` holds the path nodes and the explicit gap markers.
- `/Ball`: a `Ball.prefab` instance at (0, 0.6, -3), its `KillHeight` an instance override (-8, -9.5, -8).
- `/Game`: `Game.luau`, ExecutionOrder 10, its entity fields set per scene (`Ball` `/Ball`, `Goal` `/Level/Goal`, `Hud` `/HUD`).
- `/HUD`: a `HUD.prefab` instance.

## The kit (`Assets/Prefabs/`)

Every visible part is `engine://Meshes/Cube` with a default `BoxCollider` (half extents 0.5), so the collider is exactly the drawn box. Decks are 0.5 m thick.

- `Straight` (scale (4, 0.5, 8), `Track` material), `Ramp` (the same box, `Ramp` material), `Narrow` (scale (1.2, 0.5, 8), `Narrow` material): single boxes, pivot at the centre, sized per instance by `Scale`.
- `CurveLeft`: a 90-degree left turn, centreline radius 6 m, width 4 m. Six deck boxes of 15 degrees (deck k at mid angle 15k - 7.5, translation (-6 + 6 cos, -0.25, -6 sin), scale (4, 0.5, 2.106)) and six outer rails (`Rail` material, (-6 + 8.1 cos, -0.05, -8.1 sin), scale (0.2, 0.9, 2.159)). Pivot: the entry edge's centre on the deck top, heading local -Z; exit (-6, 0, -6) heading +90.
- `CurveRight`: `CurveLeft` mirrored (x and yaw negated); exit (6, 0, -6) heading -90.
- `Gap`: a `Kicker` box at (0, -0.045, -1.049), pitch 11.31, scale (4, 0.5, 2.04) (a 2 m run rising 0.4 m), then a 3 m void, and a `GapMarker` child at (0, 0, -3.5), scale (4.4, 6, 3.2), tag `Gap`. Exit (0, 0, -5).
- `MovingPlatform`: one box (scale per instance, 4 x 0.5 x 4), `RigidBody {Type: Kinematic, Layer: Track, Friction: 1}`, `MovingPlatform.luau`, `Platform` material.
- `Checkpoint` (tag `Checkpoint`): `BoxCollider {HalfExtents: (2, 1.5, 0.5), Offset: (0, 1.5, 0), IsTrigger: true}`, `RigidBody {Type: Static, Layer: Trigger}` (an explicit sensor body on its own layer), `Checkpoint.graph` (`Order`, `ActiveMaterial` CheckpointActive, `Chime` Checkpoint.sfx); children `PostL` and `PostR` (Cylinder at (+-2.3, 1.25, 0), scale (0.2, 2.5, 0.2), `Post`), `Bar` (Cube at (0, 2.6, 0), scale (4.8, 0.2, 0.2), `CheckpointIdle`) and `Burst` (particles at (0, 2.6, 0)).
- `Goal` (tag `Goal`): as `Checkpoint` with `HalfExtents` (2, 1.5, 1), `Goal` material on posts and bar, a `Pad` (Cube (0, 0.01, 0), scale (4, 0.02, 2), no collider), and the `Sparkle` and `WinBurst` emitters. No script.
- `Ball` (tag `Ball`): Sphere mesh (radius 0.5) with the `Ball` material, `SphereCollider {Radius: 0.5}`, `RigidBody {Dynamic, Mass 1, Friction 0.8, Restitution 0, LinearDamping 0.1, AngularDamping 0.4, MotionQuality LinearCast, AllowSleeping false, Layer Ball, MaxAngularVelocity 120, EnhancedInternalEdgeRemoval true}`, the roll's `AudioSource {Clip Roll.sfx, Loop, PlayOnStart, Volume 0, Pitch 1, Spatial, MinDistance 10, MaxDistance 60, Group Sfx}` and `Ball.luau` (`FallSound` Fall.sfx).
- `HUD`: the Screen canvas below.

## The levels

Laid out exactly from ADR 0038 decision 3's tables (pieces, checkpoints, goals, platforms, markers and path nodes, every translation, rotation and scale as written there), one `edit_batch` per level. Path nodes are entities `PathNode001`... below `/Level/Path`, tagged `PathNode` plus at most one hint (`Fast`, `Slow`, `Board`); gap markers are entities tagged `Gap` whose unit box covers a void.

| Level | Pieces | Checkpoints | Platform | Markers | Nodes | KillHeight | Par | `entity_bounds /Level` |
|---|---|---|---|---|---|---|---|---|
| 1 | P01-P13 (wide decks, two ramps, four railed curves) | 2 | none | none | 34 | -8 | 0:30.0 (1800 ticks) | min (-44.4, -0.5, -88), max (2.27, 5.2, 0) |
| 2 | P01-P12 and Backstop (two kicker jumps, a 1.5 m drop over a 3 m void, the lift) | 3 | `Lift`: Travel (0, 4, 0), MoveTicks 180, DwellTicks 120, PhaseTicks 0 | DropGap, LiftShaft (and the two Gap pieces' own) | 27 | -9.5 | 0:45.0 (2700 ticks) | min (-2.4, -2, -76), max (53.5, 5.2, 0) |
| 3 | P01-P16 (four 1.2 m beams, four curves, a ramp up to the high beam) | 2 | `Shuttle`: Travel (0, 0, -8), MoveTicks 240, DwellTicks 120, PhaseTicks 0 | ShuttleGap | 38 | -8 | 1:00.0 (3600 ticks) | min (-34.4, -0.5, -118.1), max (2.27, 4.7, 0) (S3) |

Level 3's bounds agree with decision 3: x from -34.4 (`Checkpoint2`'s posts, 2.4 m left of the x -32 centreline) to 2.27 (the outer rail of `P04`, the first curve, 8.2 m out from its centre at x -6, plus the rail boxes' rotated corners, as in Level 1); y from -0.5 (the decks' undersides and the foot of the `P10` ramp) to 4.7 (the goal bar's top, 2.7 m above the goal's root at deck top 2); z from 0 (the start deck's near edge) to -118.1 (the goal deck `P16`'s far end, -111.1 - 7).

`project_validate {}` reports nothing (no `PHYSICS_ADJACENT_STATIC_BODIES`).

## Scripts (`Assets/Scripts/`)

| Script | Kind | What it does |
|---|---|---|
| `Actions.luau` | Module | The input action names defined once (a frozen table: `MoveX`, `MoveZ`, `Restart`); `Ball.luau` and `Game.luau` read the actions, and the suites inject them, only through it (S3, R09) |
| `Tuning.luau` | Module | Frozen constants: GroundProbe 0.65 m, HeadingMinSpeed 2.5 m/s, HeadingRate 120 deg/s, SpeedFadeStart 0.8, the roll sound's mapping (RollMinSpeed 0.5, RollSpeedRange 10, RollMaxVolume 0.6, pitch 0.7 + 0.05 x speed up to 1.6, steps of 0.05), RespawnLift 0.6, GoalBrake 1 |
| `Clock.luau` | Module | `Clock.Format(ticks)`: whole ticks as `M:SS.t`, by integer arithmetic (`ticks * 10 // 60`) |
| `Motion.luau` | Module | `Motion.Ease(tick, moveTicks, dwellTicks)` (dwell, (1 - cos) / 2 rise, dwell, return), `Motion.Dwell` (at start, at end, ticks of dwell left), `Motion.Forward`, `Motion.Right`, `Motion.HeadingOf`, `Motion.Turn`, `Motion.Wrap` |
| `Track.luau` | Module | `Track.Sample(points, step, offset)`, `Track.InsideBox(localPoint)`, `Track.Lengths`, `Track.Project` (searching only a window of segments, clamped to the polyline), `Track.PointAt` |
| `Ball.luau` | Behaviour on `/Ball` | In the fixed step: the kill plane and respawn (Teleport, both velocities zeroed, Heading reset, `Respawns` and `Teleports` counted, the fall sound), the ground probe (`Physics.Raycast` down 0.65 m on `Track`), the heading (turns towards the horizontal velocity by at most 120 deg/s while grounded and faster than 2.5 m/s), steering (`move` = right(Heading) x MoveX + forward(Heading) x MoveZ, clamped to 1; grounded: `AddTorque(cross(up, move) x Torque)` with the torque along the velocity fading between 80 and 100 % of MaxSpeed; in the air `AddForce(move x AirForce)`), the roll sound, and `OnTriggerEnter` (a checkpoint with a higher `Order`, read from its graph, raises the spawn, so rolling back through an earlier checkpoint never moves it backwards; the goal sets `ReachedGoal`). Once the goal is reached the ball ignores input and brakes to a stop. Fields Torque 6, AirForce 3, MaxSpeed 11, KillHeight, FallSound |
| `FollowCamera.luau` | Behaviour on `/Camera` | `OnLateUpdate`: `Math.SmoothDamp` (SmoothTime 0.15) towards the ball's `RenderPosition` less Distance 7 along forward(Heading) plus Height 2.8 up, then `LookAt` the ball plus 0.6 up; snaps when the ball's `Teleports` changes. Presentation only |
| `MovingPlatform.luau` | Behaviour on each platform | Its authored pose is the cycle's start pose. `OnStart` counts from `PhaseTicks` and, when `PhaseTicks` is not 0, teleports the body to its phased pose, so the first tick moves one tick's step instead of jumping there and launching what rides it (S3, R04); each fixed step `MoveKinematic(start + Travel x Ease)`; exposes `AtStart`, `AtEnd`, `DwellLeft` |
| `Game.luau` | Behaviour on `/Game` | The timer in whole ticks (`StartTicks` from `Scene.GetLoadParameters().ElapsedTicks or 0`, `Ticks`, `TotalTicks`), the HUD (LevelText and ParText once, TimeText when the tenths change), the CHECKPOINT banner for exactly `BannerTicks` (60) ticks (the countdown runs before a new banner shows; S3, R06), the goal (win sound, `WinBurst:Emit(150)`, `LEVEL COMPLETE` with the level's time and par, or `FINISHED  M:SS.t` and `PRESS R TO PLAY AGAIN` on Level 3), `Scene.Load(NextLevel, {ElapsedTicks = TotalTicks})` 120 ticks later with `LoadRequested` and `SentParameters`, and `Restart` (`Scene.Reload()`, or `Scene.Load(FirstLevel, {})` after the finish). Every transition request (both Restart branches and the delayed load) sets `Leaving`, and nothing more is requested once it is set: a request applies at the frame's end, the frame's later fixed steps still run in this scene, and a second request would raise InvalidState (S3, R01; engine friction F17; tested by the `Frames` suite since S4, G6) |
| `Checkpoint.graph` | Graph behaviour on each checkpoint | Fields `Order` (Integer), `ActiveMaterial` (Material), `Chime` (AudioClip); variable `Activated`. On `OnTriggerEnter`, when `other` has the tag `Ball` and not `Activated`: set `Activated`, then (a `Flow.Sequence`) the `Bar` child's material slot 1 to `ActiveMaterial`, `Emit(60)` on the `Burst` child's emitter, and `Audio.PlayOneShot(Chime, <its position>)`. 24 nodes |

`script_check {}` reports no diagnostic.

## The HUD (`HUD.prefab`)

One Screen `UiCanvas {ScaleMode: MatchHeight, ReferenceHeight: 720, PixelSnap: true}`; every `UiText` has `Wrap` false and `RaycastTarget` false, every `UiImage` `RaycastTarget` false. The children in drawing order (a later one draws over an earlier one):

| Element | Rect | Text |
|---|---|---|
| `TopBar` (UiImage, (0, 0, 0, 0.35)) | anchors (0, 0)-(1, 0), pivot (0, 0), (0, 0), size (0, 84): the full width, 84 high, behind the three top texts | |
| `BottomBar` (UiImage, (0, 0, 0, 0.35), plain, `RaycastTarget` false; S2) | anchors (0, 1)-(1, 1), pivot (0, 1), (0, 0), size (0, 56): the full width, 56 high at the bottom, behind `HelpText` | |
| `LevelText` | top-left, (24, 0), 320 x 56 | `LEVEL n/3`, 28, white |
| `TimeText` | top-right, (-24, 0), 240 x 56 | the run's time, 36, right-aligned, white |
| `ParText` | top-right, (-24, 56), 240 x 26 | `PAR M:SS.t`, 18, right-aligned, (0.8, 0.82, 0.85) |
| `Banner` (UiImage, (0, 0, 0, 0.5), inactive) | centre, (0, -140), 640 x 104 | |
| `BannerText` (in Banner) | stretched | 40, centred, gold (1, 0.72, 0.18) |
| `HelpText` | bottom-left: anchors (0, 1), pivot (0, 1), (24, -16), 720 x 24 (decision 4's placement) | `WASD / ARROWS / LEFT STICK  ROLL     R  RESTART`, 16, (0.8, 0.82, 0.85) |

At 1280 x 720 `ui_tree {format: "Json", includeInactive: true}` lays out, in each of the three scenes: `TopBar` (0, 0, 1280, 84), `BottomBar` (0, 664, 1280, 56), `HelpText` (24, 680, 720, 24) with its text in (24, 682.3, 399.5, 19.4), and no `textOverflows` anywhere (S3: read in each level's play scene at tick 10,000).

`BottomBar` was built below Level 1's `/HUD` instance and written into `HUD.prefab` with one `edit_batch` (`entity.create {parent: "/HUD", index: 1}`, the `HelpText` `UiRect` change, `prefab.apply {instance: "/HUD"}`), whose `prefab.apply` listed it in `addedEntities`; Levels 2 and 3 gained it when they were next opened, and were saved.

## Sounds (`Assets/Audio/`)

- `Roll.sfx`: Seed 7; a Noise layer (2 s, LowPass 380, volume 0.7) and a 55 Hz Sine layer (2 s, volume 0.3), envelope 0/0/1/0: a seamless loop on the ball, its volume and pitch quantized and written only when they change.
- `Checkpoint.sfx`: Square, DutyCycle 0.25, E5:0.08 A5:0.16, sound volume 0.5, envelope 0.005/0.05/0.6/0.08. Played by `Checkpoint.graph`.
- `Fall.sfx`: Saw, 0.6 s, 520 to 90 Hz, LowPass 2400, sound volume 0.5, envelope 0.01/0.1/0.7/0.2. Played (non-spatial) at each respawn.
- `Win.sfx`: a Square layer (DutyCycle 0.5, C5 E5 G5 0.1 s each, C6 0.35 s, volume 0.7) and a Triangle layer an octave lower (volume 0.6), envelope 0.005/0.05/0.7/0.12, sound volume 0.5. Played at the goal.

In play `audio.stats` shows one voice, the roll (volume 0.6, pitch 1.2 at 10.5 m/s).

## The look

- Materials (linear `BaseColor`): Track (0.62, 0.64, 0.68) roughness 0.75; Ramp (0.36, 0.52, 0.72) 0.7; Narrow (0.85, 0.55, 0.12) 0.6; Rail (0.18, 0.2, 0.24) 0.5; Kicker (0.9, 0.78, 0.15) with `engine://Textures/Checker`, UVScale (1, 4), 0.6; Platform (0.95, 0.35, 0.08) 0.45; Ball (0.85, 0.08, 0.06) with the Checker, UVScale (2, 2), 0.25; Post (0.85, 0.85, 0.88) metallic 0.8 roughness 0.3; CheckpointIdle (0.05, 0.6, 0.75) emissive (0.1, 0.8, 1) x 3; CheckpointActive (0.1, 0.75, 0.2) emissive (0.2, 1, 0.3) x 3; Goal (1, 0.7, 0.15) emissive (1, 0.65, 0.1) x 4, metallic 1, roughness 0.25.
- Sky: `engine://Environments/Sky`, Intensity 1, ShowSkybox, SkyboxBlur 0, **Rotation -105**: found with an up-looking 160-degree probe camera in play, which showed the HDRI's sun about 50 degrees up ahead-right of the start at rotation 0 and behind-right at -105, where the sun light comes from.
- Sun: `/Sun` EulerAngles (-50, 35, 0); `DirectionalLight {Color (1, 0.95, 0.88), Intensity 3.5, CastShadows, LightAngle 2.5, ShadowDistance 70, CascadeCount 4, CascadeSplitLambda 0.8}`.
- `PostProcess {ExposureMode Manual, ExposureEV -0.5, Tonemap AgX, SsaoEnabled, SsaoRadius 1, SsaoIntensity 1.2, SsaoQuality High, BloomEnabled, BloomIntensity 0.06, AntiAliasing Fxaa, SsrEnabled false}`.
- Particles: checkpoint `Burst` (Sphere 0.6 m, Speed 2-4, Lifetime 0.5-0.9, Size 0.08-0.16, Acceleration (0, -4, 0), Additive cyan fading out, Playing false, MaxParticles 256, Seed 11); goal `Sparkle` (looping, Rate 24, Cone pointed up (EulerAngles (90, 0, 0)), ShapeRadius 1.6, ConeAngle 12, Speed 1-2.5, Lifetime 0.8-1.6, Size 0.05-0.12, Acceleration (0, -0.6, 0), Additive gold, MaxParticles 128, Seed 23) and `WinBurst` (as Burst, gold, Speed 3-6, Lifetime 0.6-1.2, MaxParticles 512, Seed 31).
- Terrain backdrop: one `Assets/Terrain/Valley.heightfield` (129 x 129) shared by the three scenes; `/Backdrop` `Terrain {Size (400, 400), HeightScale 30, one layer Tint (0.16, 0.24, 0.1), Tiling 8, Roughness 0.95, Metallic 0, ReceiveShadows}` at (-221, -45, -242.5), (-174.5, -45, -237) and (-216, -45, -258). Sculpted through Level 1's backdrop in four strokes: `Raise` (radius 35, strength 1) on a ring of 24 points of radius 110 m around the terrain's centre, eight times; `Raise` (radius 40) on a ring of 12 points of radius 165 m, twelve times; `Smooth` (radius 45) over the inner ring; `Raise` (radius 32) on 48 points along the square's edges, 12 m in, eight times, so the valley's rim hides the terrain's end. The valley floor is at -45 m and no sample can exceed HeightScale, so every hill stays below -15 m. No collider.
- Not used (decision 5): auto-exposure, TAA, SSR, baked probes, colour-grading LUTs.

## Tests (`Assets/Tests/`)

| Suite | Script | Scene | Modes | What |
|---|---|---|---|---|
| `Logic` | `Logic.test.luau` | none | Editor, Release, Dist | 5 cases: `Clock.Format`; `Motion.Ease` and `Motion.Dwell` ("Motion: Ease dwells, rises, dwells and returns, and Dwell tells the dwell and the ticks it has left"); headings and turns (wrapping at +-180); "Track: samples, measures, projects onto and walks a polyline, and tells points inside a unit box" (`Sample`, `InsideBox`, `Lengths`, `Project`, `PointAt`); "Track: a projection searches only its window of segments, clamped to the polyline" (a doubled-back polyline whose nearest segment overall lies outside the window; S3, R07) |
| `RollingBall` | `RollingBall.test.luau` | Level1, Isolation Suite | Editor, Release, Dist | 20 cases in one session across the levels (below; 15 until S4, which added five and changed three) |
| `Frames` | `Frames.test.luau` | Level1, Isolation Suite, `Clock` [0.1] | Editor, Release, Dist | 2 cases on a ScriptedClock whose every frame runs 5 fixed steps (below; S4, G6) |
| `Autopilot.Level1` | `Autopilot/Level1.test.luau` | Level1 | Editor | Recording suite of `Level1.replay` |
| `Autopilot.Level2` | `Autopilot/Level2.test.luau` | Level2, Parameters `{ElapsedTicks: 1500}` | Editor | Recording suite of `Level2.replay` |
| `Autopilot.Level3` | `Autopilot/Level3.test.luau` | Level3, Parameters `{ElapsedTicks: 3600}` | Editor | Recording suite of `Level3.replay` |

`RollingBall`'s cases, in order. The suite crosses levels by `Scene.Load` and `Scene.Reload`: the case during which a transition is requested returns in that frame, and the next case runs in the new scene's VM.

1. "Level 1: the main path is supported everywhere but inside its gaps" (samples every 0.25 m from 0.125 m, a downward `Physics.Raycast` on `Track` outside the `Gap` markers, at least 400 samples, the nearest solid piece by `Physics.GetColliderBounds` on a miss, trigger colliders left out so that a miss inside a checkpoint's or the goal's trigger names the track (S4, G3: at Level 1's `Checkpoint1` + (0, 0.5, 0) the report names `P06 of P06 (0.50 m away)`, where it named `Checkpoint1 of Checkpoint1 (0.00 m away)` before); likewise on Level 2 (400) and Level 3 (500)).
2. "Level 1: the HUD shows the level and its par" (`LevelText` `LEVEL 1/3` and `ParText` `PAR ` .. `Clock.Format` of the scene's `ParTicks`; likewise cases 7 and 15 on Levels 2 and 3; S4, G4).
3. "Level 1: a fall respawns the ball at the last checkpoint with no velocity" (the checkpoint's graph activated and its bar's material; the heading expected as `Motion.Wrap` of the checkpoint's own yaw: S3, R14).
4. "Level 1: rolling back through an earlier checkpoint never moves the spawn backwards" (through `Checkpoint2`, back through `Checkpoint1`, then a fall: `LastOrder` 2 and the respawn at `Checkpoint2` + (0, 0.6, 0) with its yaw; S3, R03).
5. "Level 1: the goal stops the timer and loads Level 2 with it" (also expects `Leaving` with the load).
6. "Level 2: the timer carried across Scene.Load continues".
7. "Level 2: the HUD shows the level and its par" (S4, G4).
8. "Level 2: the lift carries the ball up to the upper deck".
9. "Level 2: a platform started part-way through its cycle moves on from its phased pose" (a `MovingPlatform.prefab` instance created by the case, its `PhaseTicks` set halfway up the rise before its first tick; its `Tick` and pose one and two ticks on; S3, R04. Since S4, G5, also its linear speed two ticks after its creation: a test case runs before its tick's physics step and a Kinematic body's velocity is its last sweep's (friction F18), so that is the sweep of the platform's first tick, 2.094 m/s (one tick's step, |Travel| x (Ease(211) - Ease(210)) x 60), expected below 10 m/s; started S2's way, at its authored pose, the same reading is 122.094 m/s, the jump |Travel| x Ease(211) x 60).
10. "Level 2: the main path is supported everywhere but inside its gaps".
11. "Level 2: Restart reloads the level" (`Test.InjectAction(Actions.Restart, "Tap")`; the game sets `Leaving` and requests no load; S3, R02).
12. "Level 2: the restarted level starts again from the time it was entered with" (`Level2` again, `ElapsedTicks` kept and equal to `StartTicks`, the level's own `Ticks` restarted, the ball at its start with no respawn and no checkpoint).
13. "Level 2: its next level is Level 3, which the suite loads with a carried time" (the game's `NextLevel` expected equal to `Assets.Load("Assets/Scenes/Level3.scene")`, then `Scene.Load(NextLevel, {ElapsedTicks = 3600})`; S3, R13; S4, G2).
14. "Level 3: the suite's load arrives with its carried time" (`Level3`, `ElapsedTicks` 3600 and the game's `StartTicks` 3600; S4, G2).
15. "Level 3: the HUD shows the level and its par" (S4, G4).
16. "Level 3: the main path is supported everywhere but inside its gaps" (the support check only since S4, G2).
17. "Level 3: the goal shows the finish with the total time" (no load requested and not `Leaving` 130 ticks later).
18. "Level 3: Restart after the finish starts a new run" (S3, R02).
19. "Level 1: the new run starts on Level 1 with no time" (no `ElapsedTicks`, `StartTicks` 0).
20. "Level 1: a new checkpoint shows the CHECKPOINT banner for BannerTicks ticks" (the new run's `Checkpoint1`: `/HUD/Banner` inactive before, active with `BannerText` `CHECKPOINT` in the tick the case first sees `LastOrder` 1, and its first tick inactive `BannerTicks` (60) ticks later; S4, G1. S2's order, the countdown after `showBanner`, gives 59: shown with S2's `BannerLeft` one lower after the showing tick, the same count read 59).

`Frames`' cases (S4, G6). Its `Clock` [0.1] is more than the project's `MaxStepsPerFrame` (5) fixed steps of 1/60 s, so every frame runs 5 fixed steps (frames start at ticks 5, 10, ...), as a frame of live play may.

1. "Level 1: Restart in the frame its delayed load falls due requests one transition": it reads the frames from `Time.GetFrameCount()`, places the ball on the goal so that the game finishes in tick 18 (F), presses Restart (through `Actions`) so that the game sees it in tick 138 (F + `LoadDelayTicks`; the press's tick returns before `FinishedTicks` counts), and expects in tick 139, the last step of the press's frame (ticks 135 to 139), in which `FinishedTicks` reaches `LoadDelayTicks`: the same frame as the press, no load requested and no script error.
2. "Level 1: the restart reloads Level 1, not Level 2" (`Level1`, not finished, not `Leaving`, its own `Ticks` 1, `StartTicks` 0).

With `Leaving` cleared after the press, as S2's `Game.luau` (which had no such flag) left it, the delayed load's `Scene.Load` raised `InvalidState: The session already has a pending scene transition or stop` at `Game.luau:164` in tick 139, in the press's frame (shown in S4 with a temporary version of the suite): this suite tests R01's guard, which no lockstep suite (one fixed step a frame) can.

`Lib/Autopilot.luau` is the autopilot: pure pursuit 2.5 m ahead of the ball's projection on the path polyline (the projection searches the 3 segments behind and the 3 ahead of the segment it last found, besides that one; S3, R12), wanted speeds 7.5 m/s (11 on `Fast`, 4.5 on `Slow`, 6 where the path turns more than 5 degrees), a speed plan braking at 4 m/s^2 ahead of slower segments and to a stop at a `Board` node; at a `Board` node it waits for the nearest platform to dwell at its start with at least 100 ticks left, rides holding the platform's centre (with its velocity) until it dwells at its end, then follows the path again. The steering is the velocity error times 0.5, clamped to a unit stick and turned into the frame of the ball's `Heading` (`MoveX` along right(Heading), `MoveZ` along forward(Heading)); values are quantized to 1/32 and injected (through `Actions`) only on change. It only reads the scene and injects input. Each case expects the scene's own par (its `/Game`'s `ParTicks`) equal to the suite's constant, which sets the case's `TimeoutTicks` (par + 60) (S3, R08), then the goal reached, no respawn and the level's own ticks below the scene's par, then waits 30 ticks for the finish.

## Replays (`Assets/Tests/Replays/`)

Re-recorded in S3, after the fixes of R01, R06 and R09, with `test_run {filter: "Autopilot.LevelN", record: "Assets/Tests/Replays/LevelN.replay"}`; each verified twice with `input_replay {verify: true, strictHash: true}` (equal `stateHash`), `ReachedGoal` true right after, and failing with `withoutInput: true` at its first checkpoint. Each header records `EngineVersion` 0.4.0, `Seed` 27, `FixedHz` 60 and `Config` Editor. S4 changed no hashed value (only the `RollingBall` and `Frames` suites and `Testing.Suites`), so the replays were not recorded again and verify at these hashes.

| Replay | Header Parameters | Ticks | Events | Checkpoints | FinalStateHash | Without input |
|---|---|---|---|---|---|---|
| `Level1.replay` | `{}` | 1380 | 592 | 22 | `03f4835d637dba17` | fails at tick 60 |
| `Level2.replay` | `{ElapsedTicks: 1500}` | 1326 | 527 | 22 | `d17b4d5e408b7c3a` | fails at tick 60 |
| `Level3.replay` | `{ElapsedTicks: 3600}` | 2693 | 802 | 44 | `b09b5016a15c2064` | fails at tick 60 |

S3's recordings have S2's ticks, events and hashes: the state hash covers the world, never script tables (`Leaving`, the `Actions` names), and R06's banner change hides the CHECKPOINT banner one tick later, on a tick none of the 60-tick checkpoints falls on. S1's hashes were `1f9cac206c443940`, `3c1deb4cdbec7d27` and `752a493764eeca8a`. Level 1 finishes in 0:22.4 (par 0:30.0); Level 3's run ends `FINISHED  1:44.3`.

The 10,000-tick checks (S3, from each saved scene, `play_start {lockstep: true}` without a seed or parameters, no input; `stats.get {views: false}` at ticks 5,001 and 9,501; `script_errors {}` at tick 10,000): Level 1 129 entities, 5 bodies, 1 voice at both reads (script heap 1.25 then 1.28 MB); Level 2 110, 7, 1 (1.20 then 1.31 MB); Level 3 138, 6, 1 (1.18 then 1.31 MB); `script_errors {}` empty in each. With `headlessGameView: true` (which `editor_launch`'s `session` reports since friction F15), the game view renders. A later change of any hashed value (a scene component, the look included, a prefab, a material or sound handle, a script that writes something else, the layers or the input actions) re-records all three through their suites.

## Values changed from ADR 0038

1. `MoveZ`'s gamepad `Invert` is false, not true: the engine's stick axes are up-positive (`GamepadAxis` in the script API), so an inverted LeftY would roll the ball backwards when the stick is pushed forward.
2. `PostProcess.ExposureEV` -0.5 (the ADR's range is -0.5 to 0.5): at 0 the light decks clip to white under the 3.5 sun.
3. `Environment.Rotation` -105 (the ADR left it to the builder).
4. HUD: `HelpText` keeps decision 4's placement (bottom-left, anchors (0, 1), pivot (0, 1), (24, -16), 720 x 24) over a `BottomBar` that decision 4 does not have: a plain `UiImage` (0, 0, 0, 0.35), `RaycastTarget` false, the full width and 56 high at the bottom, placed second among the HUD's children so it draws behind `HelpText`. `TopBar` is 84 high, not 56, so that it also backs `ParText` (at y 56 to 82). `ParText` and `HelpText` are (0.8, 0.82, 0.85) instead of grey (0.55, 0.57, 0.6). Why: the grey texts were illegible over the light decks and the bright sky, so the light texts need a dark panel behind them. History: S1 wanted this bottom bar, but `prefab_apply` then left an entity added below an instance out of its prefab (friction F04), so S1 moved `HelpText` into a taller `TopBar` at (24, 56) as a workaround; with F04 fixed, S2 added the bar through `prefab_apply` and restored decision 4's `HelpText` placement.
5. `Ball.luau`: once `ReachedGoal` is set the ball ignores input and brakes (`Tuning.GoalBrake`), so it stops on the goal deck instead of rolling off its end during the 120-tick load delay (not in the ADR).
6. Autopilot: a boarding waits for at least 100 ticks of dwell (the ADR says 60), so the ball reaches the platform's centre before it moves; curves are planned at 6 m/s; each case also expects no respawn; `TimeoutTicks` is par + 60 for the 30-tick finish after the goal, with the par still enforced by an expectation (against the scene's own `ParTicks` since S3).
7. `RollingBall` lift case: the ball is placed 0.1 m above the lift's top centre (the lift's root is its box's centre, 0.25 m below the top, so "centre + (0, 0.6, 0)" would put the ball inside it), and its start height is measured after 20 ticks of settling, with the lift dwelling more than 45 ticks.
8. Unspecified in the ADR and chosen here: the particle values above, the Win.sfx envelopes and layer volumes, the Checkpoint/Fall sound-level volume 0.5 with layer volume 1, the camera's edit pose (0, 3.4, 4) at pitch -17.45, the terrain strokes.
9. S1's kept screenshots passed `maxDimension: 1280`, since a 1280 x 720 capture was then written at 1024 x 576 (friction F05). Since F05's fix, `viewport_screenshot {view: "Game", target: "Play", width: 1280, height: 720, keep: true}` writes a 1280 x 720 PNG, and S2's and S3's captures pass no `maxDimension`.
10. S2's and S3's look captures place the ball with `script_eval` in the lockstep play session (`RigidBody:Teleport`, zeroed velocities, the ball's `Heading` set to the piece's heading and its `Teleports` counted, so the follow camera snaps behind it along the track), then roll it with `MoveZ`: `L1-Checkpoint` from (-26.5, 3.1, -30) at (-5, 0, 0) m/s with `Heading` 90 through `Checkpoint1`, captured 22 ticks later; `L2-Lift` on the lowered lift at (33.05, -0.9, -43) with `Heading` -90, captured at tick 210, mid-rise; `L3-Beams` at the start of `P05`, (-6.6, 0.6, -32), with `Heading` 90 and `MoveZ` 0.6 for 45 ticks. Setting `Heading` replaces decision 6's `MoveX` along the beam (friction F08) and frames each capture along the track. S3 started the `L2-Lift` and `L3-Beams` sessions with their autopilot suites' `Parameters` (`{ElapsedTicks: 1500}` and `{ElapsedTicks: 3600}`), so their HUDs show a run's time. These are capture setups only; nothing in the game changed.
11. `Checkpoint.graph` sets the `Bar`'s material slot 1, where decision 5 said slot 0 until the lead's review of the game: the script API's material slots are 1-based (`MeshRenderer:SetMaterial` and `GetMaterial` take 1 for the first submesh), so slot 1 is the bar's one material (R11; the lead corrected decision 5).
12. The CHECKPOINT banner shows for exactly `BannerTicks` (60) ticks; until S3 it showed for 59, because the countdown ran in the tick that showed it (R06).
13. `MovingPlatform.luau`: a non-zero `PhaseTicks` starts the platform at its phased pose (the authored pose stays the cycle's start pose), where decision 2 only says that the tick count starts there (R04). Every platform of the three levels has `PhaseTicks` 0, so nothing in the levels moves differently.
14. A sixth suite, `Frames` (S4, G6), beside decision 10's five: frame timing on a ScriptedClock, which decision 10 does not have.
