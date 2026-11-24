# 0062 — M44 Wheeled vehicles

- **Status:** accepted, the milestone's plan and record, in phase 8 of the Skerries program (`Docs/Decisions/0060-skerries-engine-program.md`).
  - **The record.** M44's builder amends this record on `milestone/m44` as each wave lands, in "The W44.1 record" to "The W44.3 record", making decision 3 exact where it leaves a choice. The lead's reviews and the landing follow.
  - Lead-owned.
- **Date:** 2026-11-24.
- **Decided by:** the lead, from ADR 0060's design default for M44 (decision 4) and the vehicles research note (`bin/Reviews/P8/research/vehicles.md`, S02 and "Across the group", kept outside the repository).
- **Base:** main `3aaf4b10`, engine 0.8.0. Every value this record calls today's was read from the code and data at that commit.
- **Authority:**
  - Roadmap "Rules for every milestone";
  - ADR 0023; ADR 0028 decisions 10, 11 and 17; ADR 0051;
  - ADR 0060 decisions 2, 4, 6, 7 and 9, and its owner's answers;
  - ADR 0053 (M36's collider surfaces) and ADR 0054 (M37's joints, the pattern this milestone follows);
  - Architecture §5.3, §5.7, §9, §11.5, §13.5, §13.7 and §15.
- **The brief:** Skerries' S02 (`Projects/Skerries/Design/EngineTasks/S02-WheeledVehicles.md`, high priority, the vertical slice).

## Context

- **The brief.** A wheeled vehicle on a Dynamic `RigidBody`: per wheel its suspension, steering, brakes and tyre friction; an engine with a torque curve; an automatic or manual transmission; differentials; anti-roll bars. Driven from the fixed step (forward, steering, brake, handbrake), with reads (speed, revs, gear; per wheel its contact, normal, collider, suspension length, rotation, steer and slip), wheel meshes in the rendered pose, a centre-of-mass offset with an optional inertia override, grip by surface, deterministic and hashed. The buggy crosses Ridgeback and Ashfall and runs the time trial.
- **What the engine has today** (each checked at `3aaf4b10`):
  - **Jolt's vehicles are vendored and compiled:** every `Jolt/Physics/Vehicle/*.cpp` is in `Vendor/JoltPhysics/premake5.lua:424-447`, so no approval and no build change.
  - **What Jolt's vehicle is:**
    - `VehicleConstraint` is a `Constraint` and a `PhysicsStepListener` (`VehicleConstraint.h:65`). It holds a raw pointer to its body (`VehicleConstraint.cpp:80`) and asserts on bad settings (`VehicleConstraint.cpp:87-89`, `Wheel.cpp:70-80`, `WheeledVehicleController.cpp:87-91,183-208`, `VehicleDifferential.cpp:50`).
    - Its `OnStep` runs the wheel casts and the controller inside `PhysicsSystem::Update`, in step-listener jobs of `mStepListenersBatchSize` 8 (`PhysicsSettings.h:39`), one job per batch up to the job system's concurrency (`PhysicsSystem.cpp:243,683-709`). With 8 vehicles or fewer, every `OnStep` runs in one job.
    - A wheel's ground friction is `SetCombineFriction`'s callback (`VehicleConstraint.h:85-90`), called per wheel with the ground body and sub-shape; the default is `sqrt(tyre * body.GetFriction())` (`VehicleConstraint.h:238-244`), M36's geometric mean.
    - Wheel casts use the vehicle body's object layer through the system's filters, skip sensors (`VehicleCollisionTester.cpp:56,170,290`) and ignore the vehicle's own body (`VehicleCollisionTester.cpp:27`). The sphere tester has one radius for every wheel (`VehicleCollisionTester.cpp:138-142`).
    - `GetWheelLocalTransform` gives a wheel's pose in the body's origin space through Jolt's own trigonometry (`VehicleConstraint.cpp:127-145`). A wheel's contact getters assert without a contact (`Wheel.h:85-97`), and its steer angle is positive to the left (`Wheel.h:136`).
    - The transmission's three switch timers are private (`VehicleTransmission.h:80-84`); `VehicleEngine::SaveState` and `VehicleTransmission::SaveState` are public (`VehicleEngine.h:86`, `VehicleTransmission.h:76`). `VehicleConstraint::SaveState` also writes Jolt body IDs and a step counter (`VehicleConstraint.cpp:656,670`).
  - **The engine wraps none of it.** `PhysicsWorld.h` (frozen by M11, line 46) has bodies, surfaces (line 244), queries, `Step` (line 303) and M37's joints section (lines 316-348). `Step` calls Jolt's `Update` alone (`PhysicsWorld.cpp:908-939`). M37's joint record holds a `TwoBodyConstraint` (`Physics/Private/PhysicsWorldState.h:48-56`), which a `VehicleConstraint` is not.
  - **No centre-of-mass override.**
    - `BodyDescription` (`PhysicsWorld.h:74-133`) has `Mass`, and `RigidBodyComponent` (`Scene/Components/RigidBodyComponent.h:34-53`) likewise. A Dynamic body's mass properties come from `Detail::GetDynamicMassProperties` (`Physics/Private/PhysicsMassProperties.h:50`), which `PhysicsShape::CheckDynamicBody` shares (`PhysicsShape.cpp:539-547`).
    - Jolt's way is `OffsetCenterOfMassShape`: its centre of mass is the inner one plus the offset (`OffsetCenterOfMassShape.h:49`), its mass properties are translated by it (lines 62-67), and its submerged volume is overridden (line 79). A decorated shape passes sub-shape IDs through unchanged (`DecoratedShape.h:49`, `DecoratedShape.cpp:57-59`).
  - **What would read a wrapped shape wrongly** (each a site that tests or uses the Jolt body's own shape):
    - `ResolveColliderIndex` tests for `EShapeType::Compound` (`Physics/Private/PhysicsContactListener.cpp:16-30`), and `ResolvePhysicsCollider` passes `body.GetShape()` (line 35). Contacts (lines 125-126, 146-147) and a character's ground collider (`CharacterController.cpp:347`) go through it.
    - `GetSubShapeBounds` tests the same type and places sub-shapes from the body's centre of mass (`PhysicsWorldQueries.cpp:467-487`).
    - `SetShape` gives the joints the new engine shape's centre of mass minus the old body shape's (`PhysicsWorld.cpp:628,651`).
    - The queries resolve colliders through the record's own engine shape (`PhysicsWorldQueries.cpp:340,366,412,438`), and joints convert frames through the body's shape's centre of mass (`PhysicsWorldJoints.cpp:203`). Both stay right under a wrapper.
  - **Surfaces.** `ColliderSurface` (`ColliderSurfaceComponent.h:22-27`) gives a collider its friction. The world keeps it in `PhysicsBodyRecord::Surfaces` (`PhysicsContactListener.h:47-68`), which `FindPhysicsBodyRecord` and `FindColliderSurface` read from any thread (`PhysicsContactListener.cpp:38-51`). A terrain's implicit static body takes its `TerrainCollider`'s friction (`Scene/PhysicsSystem.cpp:323-329`). Materials per height-field cell are a §2 row (`Docs/Backlog.md:89`), M54's.
  - **Joints, the pattern** (ADR 0054 decisions 2 and 5):
    - every remover destroys a body's joints first: `PhysicsWorld.cpp:321-322,535-536` and `Scene/PhysicsSystem.cpp:1166-1167,1414-1419,2144-2145`;
    - the physics hash's joint part follows the bodies and characters (`Scene/PhysicsSystem.cpp:2529-2571`);
    - `physics.jointInfo` (`Automation/Methods/PhysicsMethods.cpp:286`), `JointDebugDraw`'s hook (`Scene/RenderAnnotations.cpp:135`) and `RegisterJoints` (`Scripting/RegisterBindings.cpp:57`);
    - the 0, 1 and 8 thread case (`Tests/Source/Engine/Session/JointDeterminismTests.cpp:110`).
  - **The step.** PreStep's rebuilds, surfaces and joints (`Scene/PhysicsSystem.cpp:1482-1493`), kinematic moves and teleports (1495-1579), then the characters' update (1581-1615); PostStep's joint breaks (1700), write-back (1702-1720) and the §9.4 dispatch (from 1722); Architecture §5.7 steps 5 to 7 (lines 681-683).
  - **Scripts.** `Physics.SphereCast` and the other queries (`Scripting/Bindings/Physics.cpp:227-243`), `RigidBody:AddForceAtPosition` (`Bindings/Components.cpp:520`), and `Transform:Forward`, local -Z (`Bindings/Components.cpp:509`).
  - **The Backlog.** §2's "Jolt's other constraints" (`Docs/Backlog.md:92`) does not name vehicles, so S02 adds a capability and closes no row.
- **Corrections to the brief** (each goes to the Skerries session through the product owner at the phase's end):
  - **The centre of mass is a `MassProperties` component beside `RigidBody`** (ADR 0060 decision 9), with a per-axis `InertiaScale` rather than a tensor. A `RigidBody` field would change the canonical form of every scene that holds a body (`.claude/skills/add-component/SKILL.md:46`).
  - **Terrain grip.** A `TerrainCollider`'s `Friction` is collider-wide today, and `ColliderSurface`s work now. Surfaces per terrain layer come with M54 (S05) through the hook this milestone leaves.
  - **Wheel meshes.** A `VehicleWheel` component on each child makes it a wheel, not its name. The engine writes that child's pose every tick, so its mesh rides it.
  - **The steer angle reads positive to the right,** as `Steering` +1 steers, which is Jolt's angle negated.
  - **The ghost of the best run is the game's** (recorded poses). The engine gives the determinism it replays from.
  - **The boat (S01).** `MassProperties` lands with M44, the first checkpoint of W44.1. M47 builds in parallel in phase 8 and cannot depend on it before M44 lands, so the boat floats without an offset until then. The game adds the component after phase 8. Buoyancy must see through the wrapper: Jolt's own submerged-volume query does (`OffsetCenterOfMassShape.h:79`), and a buoyancy that walks the shape itself descends the decorator. M47's merge adds a case of a floating body with an offset centre of mass.

## Decisions

### 1. Scope

S02, as one milestone in phase 8 (ADR 0060 decision 2). **In:**
- `MassProperties` (a centre-of-mass offset and an inertia scale on any Dynamic body);
- `WheeledVehicle` on the body's entity and `VehicleWheel` on each wheel child, driving Jolt's `VehicleConstraint` with its `WheeledVehicleController`;
- the world's vehicles and the session's composition, kept wheel frames, input, wheel poses and the hash's vehicle part;
- script reads, `physics.vehicleInfo`, `physics.bodyInfo`'s centre of mass, the debug view and two validation codes;
- the per-layer friction hook for M54;
- FeatureTest in every mode, with a replay.

**Out** (`Docs/Backlog.md` §2 rows at landing):
- tracked vehicles and motorcycles (ADR 0060 decision 1; Jolt's controllers are compiled but unwrapped);
- a per-wheel suspension direction or steering axis (caster, king-pin) and Jolt's fixed suspension force point;
- a per-vehicle gravity override;
- wheels that are bodies or colliders of their own;
- vehicles in 2D and in native modules (`NativeApiVersion` stays 1);
- a full inertia tensor and a script read of the centre of mass;
- vehicle presets as an asset;
- a vehicle perf metric (decision 4).

**Accepted at landing** (§6 rows):
- A wheel turning more than half a turn per tick aliases on screen, since rotation interpolates by the shortest arc (about 188 rad/s at 60 Hz). This is presentation only.
- Wheels never meet characters: their casts use the chassis's object layer, and characters' inner bodies pair with sensors only (`PhysicsLayers.h:100-103`, Architecture §9.2 "Layers").
- A write to a wheel's field, the bars or the cast makes the vehicle again with its body untouched (decision 3).
- Until M54, a terrain's grip is its `TerrainCollider`'s, whatever layer lies under the wheel.

### 2. Structure: one milestone, three waves

| Phase | # | Name | Brief | Lands | Gate | Size (builder-sessions) |
|---|---|---|---|---|---|---|
| 8 | M44 | Wheeled vehicles | S02 | second in phase 8's fixed train order (M43, M44, M45, M46, M47, M48), merging over M43's regions; owns `PhysicsSystem.{h,cpp}`, `PhysicsWorld.{h,cpp}` and the physics state headers for the phase, M47 and M48 appending after its entries | phase 8's batch gate, `CI.py --require-clang-cl` | ~7.5 (W44.1 ~3, W44.2 ~2, W44.3 ~2.5), +1 at risk |

One builder, on `milestone/m44` in `D:/Engine/.claude/worktrees/m44`, works the waves in order. There is no contract: each wave's first checkpoint fixes what a contract would freeze, M34's way.

- **W44.1, mass and the world's vehicles:**
  - **Its first checkpoint is `MassProperties`:**
    - the component and its registration;
    - `PhysicsMassOverride` and `BodyDescription::MassOverride`;
    - the wrapper and its fallout fixed at every site the Context lists;
    - `CheckDynamicBody`'s overload;
    - `physics.bodyInfo`'s `centerOfMass`;
    - `PHYSICS_MASS_UNUSED` with its fixture;
    - its cases.

    It is reviewed as a frozen-interface change and is the checkpoint the game can build on once M44 lands.
  - **Then the world's vehicles:**
    - `Physics/PhysicsVehicle.h` and `PhysicsWorld.h`'s vehicles section, with `Physics/PhysicsWorldVehicles.cpp`;
    - the removal order, refusals, priorities, the friction hook and the world's cases, the thread-count case included.
- **W44.2, vehicles in the session:**
  - `Scene/PhysicsVehicles.{h,cpp}` (composition) and `Scene/PhysicsSystemVehicles.cpp` (hooks in `PhysicsSystem.cpp`);
  - kept wheel frames, input and waking, wheel poses, the hash's vehicle part;
  - `PHYSICS_VEHICLE_INVALID` with its fixture;
  - the session cases and the committed-hash case.
- **W44.3, scripts, automation, editor, coverage and documents:**
  - `Scripting/Bindings/Vehicles.cpp`, `physics.vehicleInfo` and `Scene/VehicleDebugDraw.{h,cpp}`;
  - FeatureTest's scaffolds, suite and replay, and the Python case;
  - Architecture, the Backlog, the skills and the generated references.

Numbering: M44's record is this ADR.

### 3. Design defaults: M44 Wheeled vehicles

**What M44 adds:** three components, one category, one public physics header, two validation codes, one automation method, one result field and twelve proxy methods.

**What it does not add:** a namespace, a callback, an `IScriptHost` member, a settings section, an `AssetType`, a `PlaySessionPhase`, a state-hash block (a part inside the physics block instead), a stats row, a render pass, a pipeline, a shader struct, a golden, a perf metric, a fuzz target, a format version, a module or a vendored library.

- **`MassProperties`** (version 1, category Physics, appended after `ColliderSurface` in `PhysicsRegistration.cpp` and `BuiltinComponents.h:88`; requires Transform and RigidBody):
  - `CenterOfMassOffset`: vec3, default 0, in metres in the entity's own axes (its rotation, never its scale), each component within `PhysicsShape::MaxColliderSize`. It is added to the colliders' centre of mass.
  - `InertiaScale`: vec3, default (1, 1, 1), each component in [0.01, 100]. It scales the body-space inertia as `S^1/2 I S^1/2`, which keeps it symmetric and positive definite.
  - **The world.**
    - `PhysicsTypes.h` appends `PhysicsMassOverride {CenterOfMassOffset, InertiaScale}`, and `BodyDescription` appends `MassOverride`. The world honours it for Dynamic bodies only.
    - A non-zero offset makes `CreateBody` and `SetShape` wrap the Jolt shape in `OffsetCenterOfMassShape`. The mass properties are the wrapper's (translated, as `OffsetCenterOfMassShape.h:62-67`), scaled to `Mass`, then scaled by `InertiaScale`, then conditioned and limited inside `GetDynamicMassProperties`.
    - So `PhysicsShape::CheckDynamicBody(mass, dofs, override)` (an appended overload) and the world refuse the same bodies.
    - The record keeps the override, as it keeps `Mass`, so `SetShape` wraps again.
    - A zero offset and a unit scale take exactly today's path, bit for bit.
  - **The fallout, fixed in the same checkpoint:**
    - `ResolveColliderIndex` unwraps a `Decorated` shape before its compound test.
    - `GetSubShapeBounds` unwraps and places the sub-shapes from the inner shape's frame (the centre of mass minus the offset).
    - `SetShape` gives the joints the difference of the two wrapped centres of mass.
    - `PhysicsValidation.cpp:104-112` and `ColliderDebugDraw.cpp:153` pass the override, so the edit-time refusal (`PHYSICS_INVALID_SHAPE`) matches the world's.
  - **The session.** `MakeBodySettings` reads the component (`Scene/PhysicsSystem.cpp:297-331`), and `SameBodySettings` compares it. A write creates the body again with its velocities (§9.2's RigidBody rule, line 1364), and its signals are `OnComponentChanged`'s.
  - **`PHYSICS_MASS_UNUSED`** (Warning, not fixable): the entity's RigidBody is not Dynamic (Static, Kinematic, a trigger) or makes no body. Its code lives in its own array, `PhysicsMassDiagnosticCodes`, in a new `Scene/PhysicsMassValidation.{h,cpp}`, as M36's, and is reported in `CheckPhysics` after the joints' and before the vehicles'.
  - **`physics.bodyInfo`** appends `centerOfMass` (world, m), from the appended `PhysicsWorld::GetCenterOfMassPosition`, through `PhysicsBodyReport::CenterOfMass`.
- **`WheeledVehicle`** (version 1, category Vehicles; the seventeenth category and the first of phase 8's four new ones, `RegisterVehiclesComponents` in a new `Registration/VehiclesRegistration.cpp`; requires Transform and RigidBody; excludes CharacterController). On the body's entity.
  - **Driver input** (written by scripts or graphs every tick, applied in place as M37's motor targets are; component data, so hashed):
    - `Throttle` [-1, 1], 0; `Steering` [-1, 1], 0, +1 steering right; `Brake` and `HandBrake` [0, 1], 0;
    - `Gear`: int, 0, the gear selected in Manual (-reverse count to forward count, 0 neutral, clamped to the ratios' counts when applied); `Clutch` [0, 1], 1 (Manual: 1 engaged).
  - **Engine** (Jolt's defaults, `VehicleEngine.h:34-39`, `VehicleEngine.cpp:25-28`):
    - `MaxTorque` 500 N m; `MinRpm` 1000 and `MaxRpm` 6000; `EngineInertia` 0.5 kg m²; `EngineDamping` 0.2;
    - `TorqueCurve`: `VehicleCurveKey {X, Y}` list, (0, 0.8), (0.66, 1), (1, 0.8). X is RPM over `MaxRpm`, as Jolt evaluates it (`VehicleEngine.h:63`), increasing in [0, 1].
  - **Transmission** (`VehicleTransmission.h:33-41`):
    - `Transmission`: enum `VehicleTransmissionMode` {Auto, Manual}, Auto; `GearRatios` {2.66, 1.78, 1.3, 1.0, 0.74} and `ReverseGearRatios` {-2.90};
    - `SwitchTime` 0.5 s, `ClutchReleaseTime` 0.3 s, `SwitchLatency` 0.5 s; `ShiftUpRpm` 4000, `ShiftDownRpm` 2000; `ClutchStrength` 10.
  - **Drivetrain:**
    - `Differentials`: list of `VehicleDifferential {LeftWheel, RightWheel: EntityRef; Ratio 3.42; LeftRightSplit 0.5; LimitedSlipRatio 1.4, where 0 is open (Jolt's FLT_MAX); EngineTorqueRatio 1}`, empty by default (an empty list drives no wheel);
    - `DifferentialLimitedSlipRatio` 1.4, 0 open;
    - `AntiRollBars`: list of `VehicleAntiRollBar {LeftWheel, RightWheel: EntityRef; Stiffness 1000 N/m}`;
    - `MaxPitchRollAngle` 180 degrees (off).
  - `WheelCast`: enum `VehicleWheelCast` {Ray, Sphere, Cylinder}, Cylinder (the wide tyre). Sphere casts with the vehicle's smallest wheel `Radius`, since Jolt's sphere tester takes one radius.
  - **Axes are the engine's:** forward local -Z and up +Y, with no axis field. Jolt's `mForward` is set to (0, 0, -1).
  - **Caps** (proposed; W44.1's record fixes them): 16 wheels, 8 differentials, 8 bars, 8 gears of each kind and 8 curve keys.
  - EntityRefs inside list structs are remapped by prefab instantiation (`Scene/PrefabInstantiator.cpp:59-60`). A changed list is one whole-list override (`.claude/skills/add-component/SKILL.md:23`).
- **`VehicleWheel`** (version 1, category Vehicles; requires Transform). On a descendant of the vehicle's entity.
  - **Which vehicle:** the nearest `WheeledVehicle` above it, with no other RigidBody on the way. Wheels take Jolt's wheel index in canonical order below the vehicle.
  - **Fields** (Jolt's defaults, `Wheel.h:35-40`, `WheeledVehicleController.h:31-37`, `WheeledVehicleController.cpp:45-56`):
    - `Radius` 0.3 m (> 0) and `Width` 0.1 m (≥ 0), in metres, never scaled;
    - `SuspensionMinLength` 0.3 m, `SuspensionMaxLength` 0.5 m and `SuspensionPreload` 0 m;
    - `SpringFrequency` 1.5 Hz (> 0) and `SpringDamping` 0.5;
    - `MaxSteerAngle` 70 degrees, in [0, 90];
    - `MaxBrakeTorque` 1500 N m and `MaxHandBrakeTorque` 4000 N m;
    - `Inertia` 0.9 kg m² and `AngularDamping` 0.2;
    - `LongitudinalFriction` (`VehicleCurveKey`s by slip ratio: (0, 0), (0.06, 1.2), (0.2, 1)) and `LateralFriction` (by slip angle in degrees: (0, 0), (3, 1.2), (20, 1)).
  - **The wheel's neutral pose is its authored one.**
    - The wheel hangs at its authored centre at full droop, so the attachment point is that centre plus the body's up times `SuspensionMaxLength`.
    - Its authored rotation gives camber and toe (Jolt's `mWheelUp` and `mWheelForward`), with the axle along local X.
    - The suspension runs along body -Y and steers about body +Y.
- **The world** (`Physics/PhysicsVehicle.h`; `PhysicsWorld.h` appends a "Vehicles (M44)" section after the joints section, a reviewed frozen-interface change; `Physics/PhysicsWorldVehicles.cpp`):
  - **Vocabulary:**
    - `VehicleHandle`, from 1 in creation order and never reused;
    - `PhysicsVehicleCast` and `PhysicsVehicleCurveKey`;
    - `PhysicsVehicleWheelDescription`, the settings above in body space and radians;
    - `PhysicsVehicleSettings {Engine, Transmission, Differentials, DifferentialLimitedSlipRatio, AntiRollBars, MaxPitchRollAngle}`;
    - `PhysicsVehicleDescription {Body, Wheels, Settings, Cast, Priority}`;
    - `PhysicsVehicleInput {Forward, Right, Brake, HandBrake, Gear, Clutch}`;
    - `PhysicsVehicleState {EngineRpm, Gear, ClutchFriction, Shifting, Wheels}`, each wheel with `Touching`, `Point`, `Normal`, `ContactBody`, `ContactSubShape`, `ContactCollider`, `SuspensionLength`, `SuspensionForce` (N: Jolt's suspension lambda over the collision step), `Angle`, `SteerAngle`, `AngularVelocity`, `LongitudinalSlip`, `LateralSlip` and `LocalPose` (`GetWheelLocalTransform`).
  - **API:**
    - `CreateVehicle -> Result<VehicleHandle>`, `DestroyVehicle`, `IsVehicleValid`;
    - `SetVehicleInput`;
    - `SetVehicleSettings`, in place through Jolt's writable engine, transmission and differentials and `SetMaxPitchRollAngle`;
    - `SetVehiclePriority`, `GetVehicleState` and `GetVehicleCount`;
    - `GetCenterOfMassPosition`.
  - **Each vehicle:**
    - `AddConstraint` plus `AddStepListener`, with a tester of the chosen cast on the body's object layer;
    - `mNumStepsBetweenCollisionTest*` kept at 1 (`VehicleConstraint.h:226-227`).
    - Jolt's `Set` drives Manual's gear and clutch (`VehicleTransmission.h:51`).
  - **Nothing invalid reaches Jolt.**
    - Every constructor assert becomes a refusal (`InvalidArgument`, nothing created):
      - a Body that is not a live Dynamic body;
      - no wheel, or more than the cap;
      - min length above max, a frequency or radius not positive, a steer above 90 degrees;
      - `MinRpm` above `MaxRpm`;
      - ratios of the wrong sign;
      - `ShiftUpRpm` not above `ShiftDownRpm`, or not below `MaxRpm` in Auto;
      - a clutch strength not positive;
      - a split outside [0, 1], a limited slip in (0, 1];
      - curve X not increasing;
      - a wheel index out of range, or a differential or bar naming one wheel twice;
      - engine torque ratios not summing to 1 within 1e-3.
    - Every value is finite. The combine callback produces no NaN under MSVC Debug's floating-point exceptions.
  - **Lifetimes (the research's dangling-pointer risk):**
    - `DestroyBody` destroys the body's vehicle before its joints and the body, removing the step listener and the constraint. The world's destructor destroys every vehicle before every joint and body.
    - `SetShape` keeps the vehicle: the body pointer and attachment points are unchanged, since the points are relative to the origin (`VehicleConstraint.cpp:192`).
    - Reads never touch a wheel's per-step `mContactBody` pointer. They resolve `GetContactBodyID` through `FindPhysicsBodyRecord`, which checks the sequence number, so a ground body destroyed since the step reads as no collider.
  - **The friction hook for M54:**
    - The combine callback calls one resolver, `Detail::ResolveGroundFriction(records, body, subShape)` in `PhysicsContactListener.{h,cpp}`: the collider's `ColliderSurface` friction through `ResolvePhysicsCollider` and `FindColliderSurface`, else the body's friction. It combines by the geometric mean with each tyre curve's value.
    - M54 adds a height field cell's layer surface inside that one function, and `ContactSubShape` lets it name the layer in the wheel reads.
    - Records are written only between steps, so any thread may read them.
  - **Priorities.** Vehicles solve after the joints: joints keep M37's ranks 0 to j-1, and vehicles take j onward in UUID order. Both are refreshed whenever either set in force changes, so no two constraints share a priority (ADR 0054 decision 5).
- **The session** (`Scene/PhysicsVehicles.{h,cpp}`, `Scene/PhysicsSystemVehicles.cpp`, hooks in `PhysicsSystem.cpp` as M37's):
  - **Composition.** `ComposePhysicsVehicles(scene, bodies)` is pure, shared by the session, `project.validate` and the debug view. Per vehicle it gives a `PhysicsVehiclePlan {Entity, Body, Wheels, IsCreatable}`, and diagnostics sorted by (entity, code, subject).
  - **Signals.** The two components' signals mark the vehicles (`VehiclesChanged` and the entity), never `ComponentsChanged`, so a per-tick input write composes nothing. From M44's merge, the composition's reaction to reparents keys on M43's `Scene::GetStructureRevision()`, so a per-tick `Throttle` write, which moves the data revision (`Scene/Scene.h:169-170`), never re-records the hierarchy.
  - **Synchronization.** `SynchronizeVehicles` runs after `SynchronizeJoints` in `Rebuild` (`SynchronizeVehiclesAfterRebuild`), PreStep and `SynchronizeBodies`.
    - A change of the engine, transmission, differentials or `MaxPitchRollAngle` applies in place.
    - A change of a wheel's field, the wheel set, the bars or the cast makes the vehicle again on the same body, carrying over its RPM, gear, clutch and each kept wheel's spin and angle (`SetCurrentRPM`, `VehicleTransmission::Set`, `Wheel::SetAngularVelocity`, `SetRotationAngle`).
    - A vehicle whose entity is leaving play keeps what it has until `FlushDestroyed`.
  - **Kept wheel frames.** When a vehicle is first created, each wheel's attachment point, up and forward are formed in the body's origin space from the scene's poses of that moment, and kept relative to the origin. They are formed again only when the formation differs: the body, the wheel set and order, or the chain of entities from a wheel up to the vehicle (a reparent). So a body made again mid-play gets its wheels back where they were authored, although the engine has since written the wheels' Transforms. A disabled entity keeps its frames; a destroyed one or a removed component drops them.
  - **PreStep.**
    - Vehicles are synchronized after the joints.
    - Then a new item 4b, after the kinematic moves and teleports and `WakeOwners` and before item 5's characters, applies each vehicle's input in canonical order (on the milestone branch by `Scene::GetCanonicalOrder()`; M43's `CollectCanonical` at M44's merge, its visits counted in `EntitiesVisited`). It wakes a sleeping body whose `Throttle` is not 0 or whose input differs from the last applied one, since Jolt only resets the sleep timer while wheels spin (`VehicleConstraint.cpp:311-322`).
  - **PostStep.** After the write-back and before §9.4's dispatch, each wheel entity's local `Translation` and `Rotation` are written from `LocalPose` (suspension, steer and spin). They go into the wheel's parent space through the chain of local matrices from the vehicle's entity, as M41's placement does, and are written only when they differ bit for bit, through `Entity::Patch` (so M43's change tracking marks them; ADR 0061's `banned-transform-write`), or in place from `PhysicsSystem.cpp` with `Scene::MarkTransformWritten`. So collision callbacks see the wheels placed, and render interpolation draws them in the rendered pose with no renderer change.
  - **Removal.** `RemoveBody` destroys the body's vehicle before its joints (every path: a RigidBody change, a refusal after an edit, the disable and destroy flushes). `FlushDestroyed` removes the leaving vehicles beside `RemoveLeavingJoints`. The destructor destroys every vehicle before the joints.
  - **The hash's vehicle part.** It goes after the joint part, inside `AppendStateHash`, for every vehicle whose wheel frames the session keeps, in canonical order of its entity:
    - its UUID and each wheel's kept frame;
    - while in force, the bytes `VehicleEngine::SaveState` and `VehicleTransmission::SaveState` write through a `JPH::StateRecorderImpl` (RPM, gear, clutch friction and the three timers);
    - then per wheel, the bits of its angular velocity, angle, steer angle and suspension length.

    Never `VehicleConstraint::SaveState`, whose body IDs and step counter depend on history; no existing block hashes a Jolt ID. The early return also needs no kept vehicle frame, and a scene without a vehicle hashes as before.
  - **Run modes and pause.** All modes, Play and Simulate; Simulate runs no script, so a vehicle keeps its authored input. Under the world pause PreStep, Step and PostStep hold (§5.7), so vehicles hold. Edit scenes simulate nothing.
- **Validation: `PHYSICS_VEHICLE_INVALID`** (Error, not fixable). One per vehicle, the first reason, each with its Subject:
  - "body": no Dynamic body;
  - "wheels": no wheel, or more than the cap;
  - "wheel": a wheel under another RigidBody or a CharacterController;
  - "collider": a solid collider on or below a wheel, which its body would gather;
  - "suspension": min above max;
  - "engine";
  - "transmission";
  - "differential" or "bar": a wheel that is not the vehicle's, one wheel twice, torque ratios;
  - "curve".

  The code lives in its own array `PhysicsVehicleDiagnosticCodes` in `Scene/PhysicsVehicles.h`. `CheckPhysics` appends the composition's diagnostics after M37's joints' and the mass diagnostics (`ProjectValidator.cpp:1258-1260`). The session raises the same once each, with "refused" for a description the world refuses. Its fixture is `Tests/Data/Validation/PHYSICS_VEHICLE_INVALID/`, and `PHYSICS_MASS_UNUSED/` is beside it (`MakeValidationFixtures.py`; `COMPONENT_ORDER` gains `MassProperties` after `ColliderSurface` and the two vehicle components after `FixedJoint`, before `ValidatorTarget`). At its merge, M44 appends `VehicleWheel` to M43's `SCENE_STATIC_MOVES` list (`Scene/StaticScenery.cpp`).
- **Scripts** (`ScriptBindings::RegisterVehicles` in `Scripting/Bindings/Vehicles.cpp`, one line after `RegisterJoints`, the first of phase 8's three; `RunModes::All`, not mutating):
  - **`WheeledVehicle`:**
    - `GetSpeed()`: m/s along its forward, signed;
    - `GetEngineRpm()`, `GetGear()` and `IsShifting()`.
  - **`VehicleWheel`:**
    - `IsTouching()`;
    - `GetContact() -> (vector?, vector?, Entity?)`: the point, the normal and the collider entity;
    - `GetSuspensionLength()` (m) and `GetSuspensionForce()` (N);
    - `GetRotation()` (degrees, [0, 360)), `GetSteerAngle()` (degrees, positive right) and `GetAngularVelocity()` (deg/s);
    - `GetSlip() -> (number, number)`: the ratio, and the angle in degrees.
  - They read `PhysicsSystem::GetVehicleInfo`, which takes the vehicle's entity or a wheel's: the state after the last step. A vehicle not in force raises "entity has no vehicle in force", and a host without the session's physics raises "physics is unavailable" (ADR 0054 decision 8). Fields go through the generated proxies.
- **Automation: `physics.vehicleInfo {entity}`.**
  - Registered by `RegisterPhysicsMethods` and `RegisterPhysicsMethodTypes`, so `RegisterMethods.cpp` is unchanged. It is in the observe domain, read-only, batch-allowed, in the Runtime subset, and not an MCP tool, as `physics.jointInfo` is.
  - Result: `{entity, body, created, speed, engineRpm, gear, clutch, shifting, wheels: [{entity, touching, point, normal, collider, suspensionLength, suspensionForce, rotation, steerAngle, angularVelocity, longitudinalSlip, lateralSlip}], tick}`.
  - Errors: as `physics.jointInfo`'s, with `NotFound` at `/entity` for an entity that is neither a vehicle nor a wheel. `SceneRelation::None`, its `SceneTargetTests.cpp` row the first of phase 8's two.
- **The editor.** The inspector is generated, with the Physics and Vehicles groups. `VehicleDebugDraw` (`Scene/VehicleDebugDraw.{h,cpp}`) is called once in `RenderAnnotations.cpp` (M45's file for the phase; this call is M44's region) after the joints' (line 135), under the Colliders flag. It draws each wheel's circle at its pose, its suspension travel, and in play its contact point and normal; a vehicle the session did not make is drawn refused. It uses lines only, with no pipeline and no golden (ADR 0054 decision 11).
- **Stats and perf:** none (decision 4).
- **FeatureTest** (IdSeeds 4101 and 4102, in M44's block 4100-4199 of phase 8's ledger; every gate in each mode).
  - **`Vehicles.jsonl`** makes `Assets/Scenes/Vehicles.scene`, `Assets/Scripts/Vehicles/VehicleDriver.luau` (keys W, S, A, D and Space to input) and `Assets/Tests/Vehicles/Vehicles.test.luau`.
    - **The scene:**
      - a ground with a 2 m ledge;
      - a slick strip (`ColliderSurface` Friction 0.3) beside a rough one (1.0);
      - a small terrain;
      - a four-wheel **Buggy** (Auto, Cylinder, `MassProperties` lowered, two differentials, two bars);
      - a **Cart** (Manual, Ray);
      - a parked **Rig** (Sphere, handbrake on, `MaxPitchRollAngle` set);
      - every field non-default somewhere.
    - **The suite** (`Isolation: Case`, Editor, Release and Dist) checks:
      - the brief's four checks;
      - the slick-against-rough stop;
      - Manual's gears;
      - every read and its errors;
      - the fields through the proxies.
  - **`VehiclesReplay.jsonl`** records `Assets/Tests/Replays/Vehicles.replay`: 900 ticks of planned keys (launch, a full-lock turn, the drop, a stop), with checkpoints. The determinism suite verifies it in Debug, Release and Dist, and its input check requires it to fail without its input.
  - **Gates:** 1 (the three components), 1b (`VehicleTransmissionMode` and `VehicleWheelCast`, every value), 2 (the twelve methods), 4 (every field), 5 (`physics.vehicleInfo` in `test_physics.py`), 6 (the two fixtures) and 7 (every description); 3, 8 and 9 nothing new. The soak gains a subject at the program's end.
- **Documents:**
  - **Architecture:** §5.3 (three rows); §5.7 (steps 5 and 7); §9.2 (the override and the wrapper in "Body settings" and "Collider identity"); §9.7 (the vehicle cases); a new §9.11 "Vehicles"; §11.5 (the two proxies); §13.5 (`physics.vehicleInfo`); §13.7 (two codes and `centerOfMass`); §15.5 (the scene).
  - **The header comments:** `PhysicsWorld.h`, `PhysicsShape.h`, `PhysicsTypes.h`, `PhysicsSystem.h` and `PhysicsContactListener.h`.
  - **The skills:**
    - luau-gameplay gains a `vehicles` page (a `DocExamples`-checked driver) and one index line. `SKILL.md` is 46,171 of its 49,152 bytes.
    - game-building's `prefabs-and-levels` gains the vehicle prefab recipe: wheel children, a lowered `MassProperties`, differentials.
    - game-debugging gains the `validation-codes` rows and `physics.vehicleInfo`.
  - **The Backlog:** §2 and §6.
  - **The generated references.**

### 4. The research's questions, decided by the lead

1. **The centre of mass: a component.** Settled by ADR 0060 (decision 9; owner question 1 of the research): no hash moves and nothing is upgraded.
2. **Vehicle types: wheeled only.** Motorcycles and tracked vehicles are §2 rows (ADR 0060 decision 1).
3. **An optional perf metric (`vehicles/8Buggies/tickMs`): not added.** A buggy costs four casts and one constraint a tick. Phase 8's perf runs only at the program's hardened gate (ADR 0060 decision 7), and S02 sets no budget. The game measures its trial in its export.
4. **The override's inertia: Jolt's.** The wrapper's mass properties are translated by the offset (parallel axis, `OffsetCenterOfMassShape.h:62-67`) before `InertiaScale`. It is what Jolt documents, what the research's "translated inertia" check assumes, and `InertiaScale` tunes it.
5. **What the vehicle part hashes** (the research's list made exact). The transmission's timers are private, so Jolt's public engine and transmission `SaveState` give them. The constraint's own `SaveState` is refused for its history-dependent IDs, and its warm-start lambdas stay out, as contact caches do today. The two-histories case covers what that leaves.
6. **Three waves, not the research's two.** The research's W1 (~4–5 sessions) held two frozen-interface changes (M11's world and shape headers, then the session's), and the lifetime rules. Splitting it lets the world's lifetimes and the world thread-count case be reviewed before the session builds on them, at M42's wave size.
7. **The thread-count cases use 24 vehicles,** a refinement the research did not name. Jolt batches step listeners by 8 (`PhysicsSettings.h:39`, `PhysicsSystem.cpp:243`), so a test with 8 or fewer vehicles runs every `OnStep` in one job at any thread count and proves nothing.

### 5. Approvals

None. Jolt's vehicle code is already vendored and compiled, and the fixtures are made by the scaffolds and the committed generators.

### 6. Determinism, platforms and goldens

- **Determinism.** New components only (ADR 0060 decision 6: "none"). No committed hash moves, which `Scaffold.py --check`, the determinism and games suites and the 200-body pile's `CommittedWorldPileHash` (`Tests/Source/Engine/Physics/PhysicsWorldTests.cpp:108`) confirm.
  - Vehicles run in Jolt's jobs, so the world's and the session's 0, 1 and 8 thread cases and the session's committed constant (Debug and Release unit runs) pin them.
  - `Vehicles.replay` verifies in Debug, Release and Dist.
  - No `project.upgrade`.
- **Platforms.** Only Windows is verified (ADR 0028 decision 10). M44 adds no platform code.
- **Goldens.** None added or changed (debug lines only), and the device class stays `nvidia-62x`. No editor-UI capture opens Add Component, which the lander checks.
- **Fuzz.** No new reader. The random-vehicles case runs under Jolt's asserts in the unit suite; the long fuzz and ASan wait for the program's hardened gate.

### 7. Process

- **One builder**, its own worktree, the three waves in order. One independent review per wave, verified, with every finding fixed, nits included.
- **Frozen headers.** `PhysicsWorld.h`, `PhysicsShape.h`, `PhysicsTypes.h` and `PhysicsSystem.h` (M11's) change only as decision 3 names, as reviewed changes to a frozen interface.
- **Checkpoint commits** run their targeted checks. The unit cases are filtered. The feature and automation suites need a Vulkan device and run once, filtered, at W44.3's end, under the machine locks.
- **Landing.** Phase 8's one batch, in the fixed train order M43, M44, M45, M46, M47, M48, with a merge review: M44 merges second, over M43's regions (decision 8). M44's entries come first in its contended regions, then M47's, then M48's (the fixed train order, the ledger). FeatureTest is regenerated at M44's merge. `EngineVersionString` is M48's, the last lander's (0.9.0).
- **The gate:** phase 8's batch gate, `CI.py --require-clang-cl` (ADR 0060 decision 7). The hardening waits for the program's last phase, with `Docs/Backlog.md` §3's row.

### 8. The milestone's reservation needs

Phase 8's reservation ledger, committed in `Docs/Decisions/0060-skerries-engine-program.md` as its section "Phase 8 reservation ledger", is the rule: the rows below are M44's rows there, with its values, orders and owners, in the fixed train order M43, M44, M45, M46, M47, M48. A deviation needs a ledger amendment on main. Today's values are read at `3aaf4b10`; "at its merge" counts M43's entries, which land first.

| Allocation | Today (`3aaf4b10`) | M44 |
|---|---|---|
| ADR numbers | 0059 the last committed; 0060 the program | 0062, this record |
| BuiltinComponents | 64 components, 16 categories, `RegisterJointsComponents` last | +3: `MassProperties` in Physics after `ColliderSurface`; `WheeledVehicle` and `VehicleWheel` in a 17th category, Vehicles, `RegisterVehiclesComponents`, the first of phase 8's four new categories (then M46's RenderDistance and Wind, M47's Water); 68 at its merge (67 on its branch); `COMPONENT_ORDER`: `MassProperties` after `ColliderSurface`, the two vehicle components after `FixedJoint` and before `ValidatorTarget` |
| Enums and structs | — | `VehicleTransmissionMode`, `VehicleWheelCast`; `VehicleCurveKey`, `VehicleDifferential`, `VehicleAntiRollBar` |
| Validation codes | 75, the last `RENDER_REFLECTION_INVALID` | +2, after M43's `SCENE_STATIC_MOVES`: `PHYSICS_MASS_UNUSED` (Warning; `PhysicsMassDiagnosticCodes`, `Scene/PhysicsMassValidation.h`), then `PHYSICS_VEHICLE_INVALID` (Error; `PhysicsVehicleDiagnosticCodes`, `Scene/PhysicsVehicles.h`): 78 at its merge (77 on its branch); both arrays join the count line; inside `CheckPhysics` after the joints (`:1258-1260`), the mass diagnostics, then the composition's |
| Settings sections | — | none |
| Script API | 22 namespaces, 28 callbacks; `RegisterBindings.cpp` ends with `RegisterJoints` (line 57) | no namespace or callback; `RegisterVehicles` +1 line, the first of phase 8's three (then M46's `RegisterWind` and M47's `RegisterWater`); 12 proxy methods; 3 generated proxy types |
| `IScriptHost` | ends with `GetMusic` (`ScriptHost.h:250`) | none (reads through `GetPhysics()`, line 96) |
| Automation | 126 methods, 86 MCP tools | 127th method `physics.vehicleInfo` (not a tool; M47's `water.sample` is the 128th), registered by `RegisterPhysicsMethods` and `RegisterPhysicsMethodTypes`, with `SceneRelation::None` and the first `SceneTargetTests.cpp` row; `physics.bodyInfo` +`centerOfMass`, first before `tick` (then M47's `buoyancy` and M48's `lastMove`) |
| Stats, pipelines, passes | — | none |
| `ViewConstants`, `DrawConstants`, draw flags | 512 bytes; flags to 16 | unchanged |
| Goldens, perf metrics, fuzz targets | 56; `BUDGETS` 4; 53 | unchanged |
| FeatureTest | 101 scaffolds (highest seed 3902), 116 suites, 26 replays, 104 scenes | +2 scaffolds, `Vehicles.jsonl` 4101 and `VehiclesReplay.jsonl` 4102 (M44's block 4100-4199), after M43's and before `Suites.jsonl` (1515) and `Replays.jsonl` (1516); +1 suite, +1 replay, +1 scene; +2 validation fixtures after M43's |
| State hash | physics block: bodies, characters, joints | a vehicle part after the joint part (`PhysicsSystem.cpp:2569`), and the early return (`:2533`) also requires that no vehicle frames are kept; no block |
| Formats | — | none; each component version 1 |
| `EngineVersionString` | 0.8.0 (`Core/BuildId.h:38`) | none: M48, the last lander, sets 0.9.0 |

**M44's contended regions** (line numbers at `3aaf4b10`; M47 and M48 append after M44's entries):

- **`Scene/PhysicsSystem.cpp`** (M44 owns it for the phase):
  - M43 lands first with its regions in this file (ADR 0061 decision 8: `:567-588`, `:949-997`, `:1497`, `:1702-1720`, `AppendStateHash`'s order, `:269-294`, `:2365`); M44 merges over them, and its vehicle composition keys on M43's `GetStructureRevision()`.
  - M44's regions:
    - `MakeBodySettings` and `SameBodySettings` (297-341);
    - `ConnectSignals` and `DisconnectSignals` (505-545);
    - `RemoveBody` (1166-1167), `Rebuild`'s end (1382-1383) and the destructor (1414-1419);
    - PreStep: the vehicles' synchronization after line 1493, and item 4b after `WakeOwners` (1579);
    - `SynchronizeBodies` (1637, 1646);
    - PostStep: the wheel poses between the write-back (1702-1720) and the dispatch (1722);
    - `FlushDestroyed` (2145), `GetBodyInfo` (2433-2507) and `AppendStateHash` (2533, 2569).
  - After M44: M47's buoyancy is the next PreStep item (4c), after 4b and before the characters. M48 changes item 5's update call and consumed state (1608-1614); the write-back (1702-1720, the characters' part included) is M43's rewritten loop.
- **`Scene/PhysicsSystemVehicles.cpp`:** new, M44's own.
- **`Scene/PhysicsSystem.h`:** the PreStep item list's 4b (M47's 4c follows; M48's free move is in item 5, line 72), `PhysicsBodyReport::CenterOfMass` (after line 425; M47's `Buoyancy` follows), a vehicles section after the joints' (582-588; M47's buoyancy section follows) and the frozen paragraph (162-166). M48 changes `PhysicsCharacterState` (368-382) and `MoveCharacter` (513-518) in place.
- **`Scene/Private/PhysicsSystemState.h`:** M43's owner lists land first; a vehicles section after the joints' (409-446); M47's per-body planes follow it; M48's `DesiredMode` goes inside `PhysicsCharacterEntry` (98-112).
- **`Physics/PhysicsWorld.h`:** `BodyDescription::MassOverride` (after line 132), a vehicles section after the joints' (316-348) and the frozen paragraph (46-51). M47's buoyancy section follows.
- **`Physics/PhysicsWorld.cpp`:** the destructor (321-322), `CreateBody` (434-483, 514-521), `DestroyBody` (535-536) and `SetShape` (608-651). M47's follow.
- **`Physics/Private/PhysicsWorldState.h`:** a vehicles section after the joints' (84-92, 106-112). M47's follows.
- **`Physics/Private/PhysicsContactListener.{h,cpp}`:**
  - `PhysicsBodyRecord::MassOverride` (after line 64);
  - `ResolveColliderIndex`'s unwrap (16-30);
  - `ResolveGroundFriction` after `ResolvePhysicsCollider` (cpp 32-36, h 86), which M54 extends in phase 10.
- **M44's alone in the phase:**
  - `PhysicsWorldQueries.cpp`'s `GetSubShapeBounds` (457-492);
  - `PhysicsMassProperties.{h,cpp}` (the override);
  - `PhysicsShape.{h,cpp}` (`CheckDynamicBody`'s overload);
  - `PhysicsTypes.h` (`PhysicsMassOverride` appended).
- **`Scene/PhysicsValidation.cpp` and `ColliderDebugDraw.cpp`:** `CheckDynamicBody` with the override (104-112; 153), M44's alone in the phase.
- **`ProjectValidator.cpp`:** `CheckPhysics` after the joints' (1258-1260): the mass diagnostics, then the vehicles'; both tables' ends, after M43's code.
- **Appended entries, in the ledger's order:**
  - `BuiltinComponents.h` (lines 88, 129 and 158; M43's `Static` lands first) and `RegisterBindings.cpp` (line 57, the first of phase 8's three lines);
  - `PhysicsRegistration.cpp` (M44's for the phase: `MassProperties` after `ColliderSurface`; M48 changes only `Mass`'s description);
  - `RenderAnnotations.cpp` (M45's file for the phase): one call after line 135;
  - `PhysicsMethods.{h,cpp}`, `PhysicsMethodsTests.cpp` and `test_physics.py` (M44's for the phase: `physics.vehicleInfo`, and `centerOfMass` before `tick`, M47's and M48's fields after it);
  - `SceneTargetTests.cpp`: M44's row, then M47's;
  - `Scene/StaticScenery.cpp` (M43's new file): `VehicleWheel` appended to `SCENE_STATIC_MOVES`' list at M44's merge;
  - `MakeValidationFixtures.py`, `Scaffolds.json` and `Suites.jsonl`, after M43's entries;
  - the generated references, regenerated and never merged.

No premake, `ModuleRules.json`, `RegisterMethods.cpp`, renderer or shader file changes.

## Acceptance

The Roadmap's M44 acceptance, each a named test or check:
- **"PhysicsWorld: a centre-of-mass offset moves the body's centre of mass and keeps its origin's pose"**, with "PhysicsWorld: SetShape keeps a body's mass override and tells its joints the wrapped centres of mass" and "PhysicsShape: CheckDynamicBody refuses the bodies whose mass override the world refuses".
- **"PhysicsWorld: bodies without a mass override simulate bit for bit as before"**, with the 200-body pile at `CommittedWorldPileHash`.
- **"PhysicsWorld: a compound with a centre-of-mass offset names each collider in its contacts, its ground and its sub-shape bounds"**, which fails without the unwrap.
- **"PhysicsWorld: a vehicle at rest touches with every wheel within 1 cm of its rest length"**
- **"PhysicsWorld: full throttle reaches the analytic top speed and holds it within 2 %"**: `MaxRpm · 2π / 60 / (top ratio · differential ratio) · radius`.
- **"PhysicsWorld: a full-lock turn at 15 m/s rolls a buggy over without its lowered centre of mass and not with it"**
- **"PhysicsWorld: a vehicle dropped 2 m lands on its wheels"**
- **"PhysicsWorld: twenty-four vehicles step identically with 0, 1 and 8 Jolt threads"**, with these companions:
  - "PhysicsWorld: destroying a body destroys its vehicle first, and the vehicle's handle goes invalid";
  - "PhysicsWorld: vehicle descriptions and settings Jolt would assert on are refused, and nothing reaches Jolt";
  - "PhysicsWorld: a wheel's grip follows the friction of the collider it stands on".
- **"PhysicsVehicles: every path that removes a vehicle's body removes its vehicle first"**, one subcase per path (a RigidBody change, a refusal after an edit, the disable flush, the destroy flush, the session's end), with "PhysicsSystem: random vehicles and edits never reach a Jolt assert", under Jolt's asserts in Debug and Release.
- **"PhysicsVehicles: a vehicle keeps its wheels' frames when its body is created again"**
- **"PhysicsVehicles: wheel entities take their wheel's suspension, steer and spin, written only when they change"**, with "PhysicsVehicles: a driver-input write wakes a sleeping vehicle and composes nothing".
- **"PhysicsVehicles: the same buggy stops shorter on a rough surface than on a slick one, a ColliderSurface's and a TerrainCollider's alike"**
- **"PhysicsVehicles: one state reached through two histories steps identically"**, with "PhysicsVehicles: a scene without vehicles hashes as before".
- **"PhysicsVehicles: a convoy driven by planned input ends in one state with 0, 1 and 8 Jolt threads"**, with "PhysicsVehicles: the convoy's state hash equals the committed constant in every configuration" in the Debug and Release unit runs (`Session/VehicleDeterminismTests.cpp`).
- **"ProjectValidator: PHYSICS_VEHICLE_INVALID reports a vehicle without a Dynamic body, without wheels or with a collider on a wheel"**, with "PhysicsVehicles: each vehicle that cannot be made gets one PHYSICS_VEHICLE_INVALID naming its reason".
- **"ProjectValidator: PHYSICS_MASS_UNUSED reports MassProperties on a body that is not Dynamic"**
- **"VehicleBindings: a vehicle reads its speed, revs and gear, and each wheel its contact, suspension, spin, steer and slip after the last step"**, with these companions:
  - "VehicleBindings: a vehicle not in force and a session without physics raise instead of reading";
  - "PhysicsMethods: physics.vehicleInfo reports a vehicle's drivetrain and wheels in play";
  - "PhysicsMethods: physics.bodyInfo reports the body's centre of mass";
  - "RuntimeAutomationServer: physics.vehicleInfo reports a vehicle of the Runtime's session";
  - "VehicleDebugDraw: a play session draws each wheel at its suspension with its contact, and a vehicle it did not make as refused".
- **The Python case and FeatureTest:**
  - `test_physics.py`'s `test_vehicle_info_reads_a_vehicle_in_play`;
  - the `Vehicles` suite passes in the editor, Release and Dist;
  - `Vehicles.replay` verifies at one hash in Debug, Release and Dist and fails without its input.

**S02's "How the game will check it":**
- **At rest, all four wheels touch within 1 cm of rest:** "a vehicle at rest touches with every wheel within 1 cm of its rest length" and the suite's first case.
- **Full throttle reaches its top speed and holds it:** "full throttle reaches the analytic top speed and holds it within 2 %" and the suite.
- **A full-lock turn at 15 m/s does not roll it with the centre of mass lowered:** the roll case, with its control, and the suite's Buggy.
- **A 2 m drop lands on its wheels:** "a vehicle dropped 2 m lands on its wheels" and the suite's ledge.
- **The trial's autopilot replay verifies with `strictHash` in Debug, Release and Dist:**
  - the thread-count and committed-hash cases;
  - `Vehicles.replay` in the determinism suite;
  - the trial's own replay is the game's.
- **Screenshots of the suspension compressed on landing and the wheels steered:**
  - "wheel entities take their wheel's suspension, steer and spin…" and the debug view;
  - the screenshots are the game's.

## Risks

- **The wrapper's reach.** A missed site that reads a body's Jolt shape would silently rename colliders: contacts, surfaces, the ground collider and collider events. The Context lists every site at `3aaf4b10`. The builder re-greps `GetShape()` at W44.1's start, and the compound case fails without the unwrap.
- **Dangling pointers.** A vehicle holds its body (`VehicleConstraint.cpp:80`), and a wheel holds its ground for one step. Bodies-first removal on every path, reads by ID, and the removal-path cases under Jolt's asserts cover both, as M37's do.
- **Determinism in jobs.** Each `OnStep` writes only its own vehicle and body, so its order does not matter, but only the 24-vehicle cases prove it (decision 4). Internal state outside the body is hashed, or a divergence there would show only later.
- **Tuning the analytic cases.** Jolt's penetration slop, the tyre curves and body damping bear on the 1 cm and 2 % bounds. +1 session at risk; a looser bound needs the lead's review, never a weakened case.
- **Contention in phase 8.** M47's buoyancy and M48's character update share `PhysicsSystem.cpp`'s PreStep, `PhysicsWorld.h` and the state headers. M44's hooks are single calls in named regions (decision 8), so the ledger orders the others after them.
- **The Jolt version.** The hash's vehicle part takes Jolt's state layout, and `EndGearChange` (`Physics/PhysicsWorldVehicles.cpp`) writes `VehicleTransmission::SaveState`'s layout by hand (an int and four floats; `SetVehicleSettings` refuses, `Unsupported`, when Jolt's differs: the W44.2 review's nit 8). A Jolt update that changes either re-records, as any physics change does (§13.6).
- **The hardening waits.** ASan (~40–60 more cases) and the long fuzz run at the program's last gate, on the sharded run (ADR 0060 decision 3).
- **Two sessions in one checkout.** The game sessions work in `D:\Engine`. M44 works in its worktree and reads only `Projects/Skerries/Design/EngineTasks/`.

## The W44.1 record

W44.1 makes decision 3 exact for the mass override and the world's vehicles, M34's way: each checkpoint fixes what a contract would freeze, and its first checkpoint is `MassProperties`, which the game can build on once M44 lands.

### Checkpoint 1: `MassProperties`

- **The component** (`Scene/Components/MassPropertiesComponent.h`; `PhysicsRegistration.cpp` after `ColliderSurface`; `BuiltinComponents.h` after `ColliderSurfaceComponent`: 65 components on M44's branch): `MassProperties`, version 1, category Physics, requires Transform and RigidBody (in that order), the default flags (serialized, script-visible, editor-visible, removable).
  - `CenterOfMassOffset`: `glm::vec3`, default 0, each component in [-`PhysicsShape::MaxColliderSize`, `MaxColliderSize`] (unit "m").
  - `InertiaScale`: `glm::vec3`, default (1, 1, 1), each component in [`MinPhysicsInertiaScale`, `MaxPhysicsInertiaScale`] = [0.01, 100], no unit.
- **The world** (`PhysicsTypes.h` appends `MinPhysicsInertiaScale`, `MaxPhysicsInertiaScale` and `PhysicsMassOverride {glm::vec3 CenterOfMassOffset = 0; glm::vec3 InertiaScale = 1; operator==}`; `PhysicsWorld.h` appends `BodyDescription::MassOverride` and a "Centres of mass" part with `GetCenterOfMassPosition(BodyHandle) -> glm::vec3`, ahead of the vehicles section):
  - `Detail::GetDynamicMassProperties` gains an overload with the override (`Physics/Private/PhysicsMassProperties.{h,cpp}`); the four-argument function calls it with the identity. It checks the override (InvalidArgument: an offset component not finite or above `MaxColliderSize`, a scale component not finite or outside [0.01, 100]) after the Mass check, translates the shape's mass properties by a non-zero offset (`MassProperties::Translate`, exactly what `OffsetCenterOfMassShape::GetMassProperties` gives the wrapper), scales them to Mass, then multiplies each symmetrized element (row, column) by `sqrt(s_row) sqrt(s_column)` in double before the conditioning and the `MaxBodyInertia` limit. A unit scale multiplies by exactly 1 and a zero offset skips the translation, so the identity override is today's path bit for bit.
  - `Detail::MakeDynamicBodyShape(shape, override)` gives the body's Jolt shape: the shape itself for a zero offset, else `OffsetCenterOfMassShapeSettings(offset, shape).Create()` (a ShapeResult error would be `PHYSICS_INVALID_SHAPE`). `Detail::UnwrapBodyShape(shape)` sees through an `OffsetCenterOfMass` subtype.
  - `CreateBody` and `SetShape` take both for a Dynamic body (the inner shape for the mass and for `AddDiscInertiaAboutZ`, the wrapper for Jolt and the motion quality); `PhysicsBodyRecord::MassOverride` keeps it (the identity for any other body). Static and Kinematic bodies ignore it, an invalid one included.
  - `PhysicsShape::CheckDynamicBody(mass, dofs, override)` is the appended overload; the two-argument one calls it with the identity.
- **The fallout, at every site the Context lists** (re-grepped for `GetShape()`, `GetCenterOfMass` and `EShapeType::Compound` at W44.1's start: no other site reads a body's Jolt shape for its type or frame; the queries resolve through the engine shape, `PhysicsEdgeRemoval` collides the shapes whole, and the joints' frames go through the body's own centre of mass):
  - `Detail::ResolveColliderIndex` unwraps first, so contacts, surfaces, `GetSubShapeCollider` and a character's ground collider name the compound's colliders.
  - `GetSubShapeBounds` unwraps and places the sub-shapes from the body's centre-of-mass transform pre-translated by the wrapper's `-GetOffset()` (Jolt's own placement of the inner shape).
  - `SetShape` tells the joints the new wrapped shape's centre of mass minus the old body shape's.
  - `PhysicsValidation.cpp`'s `CheckDynamicBody` and `ColliderDebugDraw.cpp`'s refusal check pass `GetPhysicsMassOverride(owner)`.
- **The session.** `MakeBodySettings` reads the component into `MassOverride` for a Dynamic, non-sensor `RigidBody` plan only (the world would ignore it for any other, so a write to a Static body's `MassProperties` rebuilds nothing), `SameBodySettings` compares it, and the component's signals are `OnComponentChanged`'s. The creation and the shape replacement check `CheckDynamicBody` with it.
- **`Scene/PhysicsMassValidation.{h,cpp}`:** `PhysicsMassUnusedCode`, `PhysicsMassDiagnosticCodes` (1), `GetPhysicsMassOverride(const ConstEntity&)` (the component's values, or the identity) and `ValidateSceneMassProperties(scene, layers)`: one warning per effectively enabled entity with a `MassProperties` that is not the owner of a creatable, non-sensor, Dynamic `RigidBody` plan, component "MassProperties", sorted by entity. `ProjectValidator` appends the code to both tables after M42's (M43's `SCENE_STATIC_MOVES` lands before it), and `CheckPhysics` reports it after the joints'.
- **Automation.** `PhysicsBodyReport::CenterOfMass` (a character's inner body's) and `physics.bodyInfo`'s `centerOfMass`, before `tick`.
- **Tests:** the three acceptance cases of the mass override, "PhysicsWorld: bodies without a mass override simulate bit for bit as before" (the pile at `CommittedWorldPileHash` beside an overridden body, and an identity override written out), "PhysicsWorld: a compound with a centre-of-mass offset names each collider in its contacts, its ground and its sub-shape bounds", "PhysicsRegistration: MassProperties requires a RigidBody and bounds its offset and inertia scale", "PhysicsSystem: a MassProperties moves a Dynamic body's centre of mass, and a write creates the body again with its velocities", "ProjectValidator: PHYSICS_MASS_UNUSED reports MassProperties on a body that is not Dynamic", "PhysicsMethods: physics.bodyInfo reports the body's centre of mass", and the fixture `Tests/Data/Validation/PHYSICS_MASS_UNUSED/` (a Static wall).
- **Documents:** Architecture §5.3 (the row), §9.2 ("Mass overrides" and the unwrap in "Collider identity in compounds"), §13.5 (`centerOfMass`) and §13.7 (the code); game-debugging's `validation-codes` row and `SKILL.md`'s physics line; `Docs/Backlog.md` §2's row for the full inertia tensor and a script read of the centre of mass; the header comments of `PhysicsTypes.h`, `PhysicsShape.h`, `PhysicsWorld.h`, `PhysicsSystem.h` and `PhysicsMethods.h`.
- **FeatureTest** coverage of `MassProperties` lands with W44.3's `Vehicles.jsonl` (decision 2), on the Buggy.

### Checkpoint 2: the world's vehicles

- **`Physics/PhysicsVehicle.h`** (new, the vocabulary), plain value types with `operator==` where the session compares them:
  - `VehicleHandle` (as `JointHandle`: from 1 in creation order, never reused); `PhysicsVehicleCast {Ray, Sphere, Cylinder}`; `PhysicsVehicleTransmissionMode {Auto, Manual}` (decision 3 names only the cast; the mode is the registry enum's twin, as the cast is); `PhysicsVehicleCurveKey {float X, Y}`.
  - **Caps** (decision 3's proposal, fixed): `MaxPhysicsVehicleWheels` 16, `MaxPhysicsVehicleDifferentials` 8, `MaxPhysicsVehicleAntiRollBars` 8, `MaxPhysicsVehicleGears` 8 (of each kind) and `MaxPhysicsVehicleCurveKeys` 8.
  - **Bounds** (a choice the decision leaves: each value finite and bounded so Jolt's drivetrain and tyre impulses stay finite; the components' registry ranges are the same): `MaxPhysicsVehicleLength` 100 m (radius, width, suspension lengths, preload), `MinPhysicsVehicleWheelRadius` 0.001 m, `MaxPhysicsVehicleTorque` 1e6 N m (engine and brakes), `MinPhysicsVehicleInertia` 0.001 and `MaxPhysicsVehicleInertia` 1e6 kg m² (Jolt divides torques by the wheel's and the engine's inertia, so neither may be 0), `MaxPhysicsVehicleRpm` 1e5, `MaxPhysicsVehicleRatio` 100 (gears and differentials), `MaxPhysicsVehicleRate` 1000 (damping rates, spring frequency and damping, clutch strength, a curve's Y), `MaxPhysicsVehicleTime` 100 s (switch, release, latency), `MaxPhysicsVehicleSlipRatio` 1000, `MaxPhysicsVehicleBarStiffness` 1e6 N/m (the W44.1 review's nit 9); a wheel curve's X within ±1000.
  - `PhysicsVehicleWheelDescription {Position; Up (0, 1, 0); Forward (0, 0, -1); Radius 0.3; Width 0.1; SuspensionMinLength 0.3; SuspensionMaxLength 0.5; SuspensionPreload 0; SpringFrequency 1.5; SpringDamping 0.5; MaxSteerAngle 70° in radians; MaxBrakeTorque 1500; MaxHandBrakeTorque 4000; Inertia 0.9; AngularDamping 0.2; LongitudinalFriction; LateralFriction}` with Jolt's default curves.
  - `PhysicsVehicleEngine {MaxTorque 500; MinRpm 1000; MaxRpm 6000; TorqueCurve (0, 0.8), (0.66, 1), (1, 0.8); Inertia 0.5; AngularDamping 0.2}`; `PhysicsVehicleTransmission {Mode Auto; GearRatios {2.66, 1.78, 1.3, 1.0, 0.74}; ReverseGearRatios {-2.9}; SwitchTime 0.5; ClutchReleaseTime 0.3; SwitchLatency 0.5; ShiftUpRpm 4000; ShiftDownRpm 2000; ClutchStrength 10}`.
  - `PhysicsVehicleDifferential {int32_t LeftWheel -1, RightWheel -1 (none); Ratio 3.42; LeftRightSplit 0.5; LimitedSlipRatio 1.4 (0 open: Jolt's FLT_MAX); EngineTorqueRatio 1}` and `PhysicsVehicleAntiRollBar {int32_t LeftWheel 0, RightWheel 1; Stiffness 1000}`: wheel indices, so the world knows no entity. A differential may leave one side empty (Jolt's -1); a bar names two wheels.
  - `PhysicsVehicleSettings {Engine; Transmission; Differentials; DifferentialLimitedSlipRatio 1.4; AntiRollBars; MaxPitchRollAngle pi}`; `PhysicsVehicleDescription {Body; Wheels; Settings; Cast Cylinder; Priority 0}`; `PhysicsVehicleInput {Forward, Right, Brake, HandBrake, int32_t Gear, Clutch 1}`. Forward reverses only in Auto with a reverse gear; Manual drives by a Forward in [0, 1] in its gear's direction (Jolt's own range).
  - `PhysicsVehicleState {EngineRpm; Gear; ClutchFriction; Shifting; Wheels}` and `PhysicsVehicleWheelState {Touching; Point; Normal; ContactBody; ContactSubShape; ContactCollider; SuspensionLength; SuspensionForce; Angle; SteerAngle; AngularVelocity; LongitudinalSlip; LateralSlip; glm::mat4 LocalPose}`. `Angle` and `SteerAngle` are Jolt's (radians, the steer positive to the left; the angle signed in (-2π, 2π), since Jolt keeps it by `fmod`, which keeps the sign of a wheel turning backward: the W44.1 review's finding 6); the session and the reads convert (the reads wrap the spin into [0, 360)).
- **`PhysicsWorld.h`'s vehicles section** (after the centres of mass): `CreateVehicle`, `DestroyVehicle`, `IsVehicleValid`, `FindVehicle(BodyHandle)` (added: tests and the session ask which vehicle a body holds), `SetVehicleInput`, `SetVehicleSettings`, `SetVehiclePriority`, `GetVehicleState`, `AppendVehicleState` (added: the hash's bytes of decision 3's vehicle part, kept in the world so the session never sees Jolt's state), `RestoreVehicleMotion` (added: what a vehicle made again carries over, decision 3's `SetCurrentRPM`, `VehicleTransmission::Set`, `SetAngularVelocity` and `SetRotationAngle`) and `GetVehicleCount`.
- **`Physics/PhysicsWorldVehicles.cpp`:**
  - Jolt's vehicle objects are reference counted. The world owns each through a `Scope` (`CreateScope`, so no naked `new`) and marks it embedded, so the system's references never delete it: `Detail::PhysicsVehicleRecord {Wheels (Scope<WheelSettingsWV>, which Jolt's wheels reference), Tester, Constraint, Body}` in `PhysicsWorld::State::Vehicles` (a map by handle value, ordered), destroyed constraint first. The controller and constraint settings are embedded locals: Jolt copies them.
  - Creation: the wheels with the suspension along body -Y and the steering axis body +Y (`mEnableSuspensionForcePoint` stays false, Jolt's default, so the forces act at the contact); `mUp` (0, 1, 0) and `mForward` (0, 0, -1); the tester on the body's object layer (`VehicleCollisionTesterRay` and `CastSphere`, the latter with the smallest wheel radius, each with Jolt's up and 80° slope; `CastCylinder` with Jolt's convex radius fraction 0.1, without a slope test: the W44.1 review's nit 10); both collision-step counts 1; the combine callback; the priority; `AddConstraint` and `AddStepListener`; the body woken.
  - **Refusals** (InvalidArgument, nothing created): every assert of decision 3's list and the divisions Jolt would make by zero. A body that is not a live Dynamic body or holds a vehicle already; an unknown cast or mode; no wheel or more than 16; a value not finite or outside its bound; a wheel's Up or Forward not unit, or the two parallel (their cross product shorter than 1e-3: no axle); min length above max; a frequency or radius not positive; a steer above 90°; an inertia below 0.001; `MaxRpm` below 1 (Jolt divides the RPM by it) or `MinRpm` above it; no forward gear; a ratio of the wrong sign; `ShiftDownRpm` not positive, `ShiftUpRpm` not above it, or in Auto not below `MaxRpm`; a clutch strength not positive; a split outside [0, 1]; a limited slip in (0, 1]; a curve whose X does not increase strictly (Jolt divides by the step) or with a negative Y (the combine's square root); a wheel index out of range; a differential or bar naming one wheel twice; no driven differential (one that names a wheel), or driven differentials whose `EngineTorqueRatio`, summed in float in list order, are not within 1e-6 of 1 (Jolt's own assert in `WheeledVehicleController::PostCollide`, at every step; the W44.1 review's finding 1 and the lead's amendment of decision 3 below).
  - **Input** clamps every value. `Forward` is clamped to [0, 1] unless the transmission is Auto with a reverse gear (Jolt would shift into gear -1 and read a reverse ratio it does not have; Manual drives by Forward's magnitude). Manual's gear goes through `VehicleTransmission::Set`, clamped to the gear counts, at every call.
  - **Settings in place**: the engine (its RPM clamped to the new range), the transmission (its gear clamped to the new counts), the differentials, the limited-slip ratio, **the anti-roll bars' stiffness** and `SetMaxPitchRollAngle`.
    - **The bars' wheels never change in place** (the W44.1 review's finding 2, which rejected this record's first departure, a change of the bars in place). Jolt writes a wheel's anti-roll impulse only while a bar covers it and keeps it in the wheel's suspension constraint otherwise, a force no state hash sees. So `SetVehicleSettings` refuses bars whose `(LeftWheel, RightWheel)` pairs differ from the vehicle's, in order (InvalidArgument, nothing changed), and a bar added, removed or moved makes the vehicle again (W44.2), as decision 3 and decision 1's accepted limit say.
    - **A gear change in progress ends first** when the transmission's mode, `SwitchTime`, `ClutchReleaseTime` or `SwitchLatency` changes (the W44.1 review's finding 3): its private timers are written to 0 and the clutch engaged through Jolt's public `VehicleTransmission::RestoreState`, in `SaveState`'s layout. Otherwise a new release time of 0 divides the time left by 0, a shorter one makes the clutch negative, and Manual freezes an Auto shift's timers (`Shifting` stays, and the vehicle never sleeps).
    - **A stored negative Forward is dropped** when the transmission becomes Manual or Auto without reverse gears (the W44.1 review's finding 4): Jolt keeps the last input, and Auto would shift into gear -1 at the next step.
  - **Reads**: a wheel's ground by its ID through `TryGetBody` (the sequence number checked), never the per-step pointer; `ContactCollider` through `ResolvePhysicsCollider`; `SuspensionForce` the suspension lambda (its spring and hard-stop parts) over the last collision step's duration; `LocalPose` `GetWheelLocalTransform(i, +X, +Y)`.
  - **`AppendVehicleState`**: `VehicleEngine::SaveState` and `VehicleTransmission::SaveState` through a `JPH::StateRecorderImpl` (RPM, gear, clutch and the three timers), then per wheel the bits of its angular velocity, angle, steer angle and suspension length (decision 4.5).
- **Lifetimes.** `DestroyBody` calls `DestroyVehiclesOf` before `DestroyJointsOf`; the destructor calls `DestroyAllVehicles` before `DestroyAllJoints`. `SetShape` leaves the vehicle (Jolt's `NotifyShapeChanged` of a vehicle does nothing: its wheels are attached relative to the origin).
- **The friction hook.** `Detail::ResolveGroundFriction(records, body, subShape)` in `PhysicsContactListener.{h,cpp}`, after `ResolvePhysicsCollider`: the collider's `ColliderSurface` friction, else the body's. The combine callback takes `sqrt(tyre × ground)` in each direction; both are finite and not negative, so no NaN is made.
- **Priorities.** The world takes the description's and `SetVehiclePriority`'s; the session's ranks, after the joints', are W44.2's.
- **Tests** (`Tests/Source/Engine/Physics/PhysicsWorldVehiclesTests.cpp`): the four acceptance cases of the world, "twenty-four vehicles step identically with 0, 1 and 8 Jolt threads" (the hash of every body and every vehicle's `AppendVehicleState`), and its three companions. The W44.1 review's fixes add the undriven and short torque-ratio refusals and the bars' wheels refusal to the refusal case, and "a vehicle whose differential drives one side only steps", "a vehicle's readings give its wheels' pose, steer, spin and slips, and its clutch and gear changes", "a vehicle made again takes the motion RestoreVehicleMotion carries over", "a Manual vehicle drives in its clamped gear, and an Auto one without reverse gears never reverses", "a stored reverse input never shifts into a reverse gear the transmission no longer has", "a transmission change during a gear change ends it, and the clutch stays finite in [0, 1]" and "a new shape on a vehicle's body keeps its vehicle". The rest case's analytic rest length is Jolt's spring: `k = m_eff (2 pi f)^2`, `m_eff` the effective mass at the wheel's contact (`1 / (1/m + (r x n) I^-1 (r x n))`), so a quarter of the weight compresses each spring by `m g / 4 / k`.
  - The rest case casts rays, so each contact lies under its attachment: a cylinder's deepest contact lies off the wheel's centre plane, which shares the weight unevenly between the left and right wheels by about 9 %.
  - **The top speed needs a rev limiter.** Jolt clamps the engine's RPM at `MaxRpm` but keeps its torque there (`TorqueCurve`'s Y at X = 1), and its clutch passes it on while it slips, so with the default curve (0.8 at 1) a buggy with no drag runs about 16 % past the analytic speed. The case's curve falls to 0 at `MaxRpm` ((0.9, 1), (1, 0)), which is what holds the analytic speed within 2 %; the luau-gameplay `vehicles` page (W44.3) says so.
  - The roll case's buggy hangs its wheels at the chassis's bottom (the attachment 0.8 m below the origin), its centre of mass about 1.6 m above the ground over a 1.1 m track; lowered 1.4 m, it stays upright.
- **The W44.1 review** (`bin/Reviews/P8/M44-W1-review.md`: 0 blocker, 3 major, 3 minor, 5 nit, all fixed in one pass, in a checkpoint of their own before W44.2's):
  1. Torque ratios: Jolt's own rule (above); the bug fixes' cases failed before them.
  2. The bars: only their stiffness changes in place (above).
  3. A transmission change mid-shift ends the shift (above).
  4. A stale negative Forward is dropped, and Manual's Forward is [0, 1] (above).
  5. World cases for `RestoreVehicleMotion`, Manual input, the Auto-without-reverse clamp, `SetShape` on a vehicle's body and the readings (pose, steer sign, spin, slips, clutch, shifting).
  6. The spin's range is documented signed (the reads wrap it).
  7. A refusal of a Dynamic body that only its `MassProperties` causes names `MassProperties` (`CenterOfMassOffset` or `InertiaScale`) with a hint, at edit time (`PhysicsValidation.cpp`) and in the session (`PhysicsSystem::State::RaiseDynamicBodyError`, `Scene/Private/PhysicsBodySettings.h`'s `GetDynamicBodyFault`).
  8. "PhysicsShape: CheckDynamicBody refuses the bodies whose mass override the world refuses" moved to `PhysicsShapeTests.cpp`.
  9. `MaxPhysicsVehicleBarStiffness`.
  10. The casts' slope test (above).
  11. "W44.1 committed" ticked.
- **The lead's amendments of decision 3** (after the W44.1 review): decision 3's "an empty list drives no wheel" and "engine torque ratios not summing to 1 within 1e-3" are replaced by Jolt's rule: an empty or undriven `Differentials` list, and driven ratios more than 1e-6 from 1, are refused (`PHYSICS_VEHICLE_INVALID` "differential"); a vehicle that drives no wheel (a trailer) has one differential, in Manual at gear 0 (the `vehicles` page's recipe, W44.3). And the bars: a bar's stiffness on the same wheel pair applies in place; adding, removing or re-pairing a bar makes the vehicle again, as a wheel field, the wheel set and the cast do.
- **Backlog** (this checkpoint, since it defers them): §2's rows for tracked vehicles and motorcycles, a wheel's own suspension direction and steering axis, its fixed force point and wheels as bodies, and a vehicle's own gravity, 2D and native vehicles, presets and a perf metric; §6's two world limits (wheels never meet characters; a terrain's grip until M54). The wheel-spin aliasing limit is W44.2's, with the wheel poses.

## The W44.2 record

W44.2 brings the vehicles into the play session: the two components, the composition, the session's synchronization, kept wheel frames, input, wheel poses, the hash's vehicle part and `PHYSICS_VEHICLE_INVALID`. W44.3 adds the scripts, `physics.vehicleInfo`, the debug view, FeatureTest, §9.11 and the skills.

### The components

- **`WheeledVehicle`** (`Scene/Components/WheeledVehicleComponent.h`, with the registry enums `VehicleTransmissionMode` {Auto, Manual} and `VehicleWheelCast` {Ray, Sphere, Cylinder} and the registry structs `VehicleCurveKey {X [-1000, 1000], Y [0, 1000]}`, `VehicleDifferential {LeftWheel, RightWheel: EntityRef; Ratio [0, 100] 3.42; LeftRightSplit [0, 1] 0.5; LimitedSlipRatio [0, 1000] 1.4; EngineTorqueRatio [0, 1] 1}` and `VehicleAntiRollBar {LeftWheel, RightWheel: EntityRef; Stiffness [0, MaxPhysicsVehicleBarStiffness] 1000}`) and **`VehicleWheel`** (`VehicleWheelComponent.h`), both version 1 in the seventeenth category, Vehicles (`Registration/VehiclesRegistration.cpp`, `RegisterVehiclesComponents` after `RegisterJointsComponents`; 67 components on the branch). Fields, defaults and order as decision 3 lists them; `Gear` is an `int32_t` in [-8, 8]; `WheeledVehicle` requires Transform and RigidBody, and `VehicleWheel` requires Transform. A precision of decision 3: `WheeledVehicle` declares no exclusion of `CharacterController`, since the `RigidBody` it requires already excludes one (an exclusion is declared by both components, so its own would change `CharacterController`'s registration for nothing). Every field is scriptable, `RunModes::All`.
- **Ranges** are the world's bounds (`Physics/PhysicsVehicle.h`) for each scalar, inclusive. What the registry cannot say (a value that must be above 0 where 0 is in range, a list's cap, an array element's range, a curve's order, a rule that spans fields or entities) is `PHYSICS_VEHICLE_INVALID`'s, never a write error, as the joints' are, so the components need no `Validate` or `Generate` hook.
- **The default `WheeledVehicle` names no differential** (its wheels are EntityRefs, which a default cannot name), so it is `PHYSICS_VEHICLE_INVALID` "differential" until its wheels are named; never a Jolt assert (the lead's ruling after the W44.1 review).
- **Manual's `Throttle`** counts in [0, 1] and the `Gear` gives the direction; a negative one is clamped (the world's rule, the W44.1 review's finding 4).

### The composition (`Scene/PhysicsVehicles.{h,cpp}`)

- `ComposePhysicsVehicles(scene, bodies)` gives `PhysicsVehiclePlan {Entity, Body, Wheels, IsCreatable}` per effectively enabled `WheeledVehicle` in canonical order, and its diagnostics sorted by (entity, code, subject). A `VehicleWheel` belongs to the nearest `WheeledVehicle` at or above its entity; the first `RigidBody` or `CharacterController` on the way (the wheel's own included), or a wheel on the vehicle's own entity, is the "wheel" reason. Wheels are in canonical order.
- **`PHYSICS_VEHICLE_INVALID`**, one per vehicle, the first reason in decision 3's order of subjects: "body" (no plan, or one without colliders; Static, Kinematic or a trigger; a plan a body diagnostic refused is not creatable without a second diagnostic), "wheels", "wheel", "collider" (a solid collider of the body's plan at or below a wheel), "suspension" (min above max, a `SpringFrequency` of 0), "engine" (`MinRpm` above `MaxRpm`), "transmission" (1 to 8 forward gears each in (0, 100], at most 8 reverse each in [-100, 0), `ShiftDownRpm` above 0, `ShiftUpRpm` above it and, in Auto, below `MaxRpm`, `ClutchStrength` above 0), "differential" (at most 8; a side that names an entity that is not one of the vehicle's wheels; one wheel twice; a `Ratio` of 0; a limited slip in (0, 1]; no differential naming a wheel; driven shares not within 1e-6 of 1, summed in float in list order, Jolt's own check), "bar" (at most 8; both sides set, wheels of the vehicle, different), "curve" (at most 8 keys, X strictly increasing; the torque curve's X in [0, 1]). Component "WheeledVehicle", the field where it is the vehicle's.
- **A reference to a disabled wheel of the vehicle** (one of its `VehicleWheel` entities that is effectively disabled) is no wheel: a differential's side becomes -1 and a bar is left out (`MakePhysicsVehicleSettings`), without a diagnostic while the vehicle stays drivable. The lead's amendment after the W44.2 review (finding 3, which rejected this record's first rule, a silent "not creatable"): when its disabled wheels leave it no driven wheel, or driven shares that do not sum to 1, `PHYSICS_VEHICLE_INVALID` "differential" (Field `Differentials`) names them ("its driven wheels 'RL' and 'RR' are disabled, so no differential drives a wheel", "with 'FL' and 'FR' disabled, its driven differentials' EngineTorqueRatio sum to 0.5"; hint "enable the wheels, or give the differentials of its enabled wheels shares that sum to 1"), at edit time and in play alike, in decision 3's order of subjects (after the differentials' own checks). A vehicle never silently fails to exist. A disabled wheel of another vehicle is any foreign wheel ("differential" or "bar": not a wheel of the vehicle).
- **The units:** `MakePhysicsVehicleWheel`, `MakePhysicsVehicleSettings`, `MakePhysicsVehicleInput`, `MakePhysicsVehicleCast` and `StripPhysicsVehicleInput`. Degrees become radians, clamped so 90 degrees of steer and 180 of pitch and roll never round past the world's pi / 2 and pi.

### The session (`Scene/PhysicsSystemVehicles.cpp`, hooks in `PhysicsSystem.cpp`)

- **Signals.** The two components' signals set `VehiclesChanged` and record the entity (`ChangedVehicles`); added or removed components also make the plans stale. They never set `ComponentsChanged`. A synchronization composes the plans again only when they are stale (a rebuild, a component added or removed, a flush), a `VehicleWheel` changed, or a changed `WheeledVehicle` differs from what the plan read beyond its driver input (`StripPhysicsVehicleInput`); a write of driver input alone stops there, and item 4b applies it. A body created or removed (`VehicleBodiesChanged`) checks the vehicles in force against their bodies without composing.
- **Synchronization**, after the joints in `Rebuild` (`SynchronizeVehiclesAfterRebuild`), PreStep and `SynchronizeBodies`, over the entries in UUID order: a vehicle whose entity or one of whose wheels is leaving play keeps what it has until the flush (its plan's diagnostics wait too); one not creatable, or without its body, is not in force; one in force on the same body with the same wheel entities, wheel descriptions (their kept frames included), cast and bar pairs changes its settings in place (`SetVehicleSettings`: the engine, transmission, differentials, limited slip, bar stiffness and pitch-roll angle); anything else makes it again (`CreateVehicle`), carrying over the motion of the vehicle it replaces (`RestoreVehicleMotion`, mapped by wheel entity: RPM, gear, clutch, each kept wheel's spin and angle). A vehicle destroyed with its body (`RemoveBodyVehicles`) keeps its motion for its creation again in the same pass (a RigidBody change), and the motion is dropped at the end of every synchronization, so a vehicle whose body comes back later (a refusal undone, a re-enabled entity) starts afresh, as its body does. A description the world refuses raises `PHYSICS_VEHICLE_INVALID` "refused".
- **The bars.** Decision 3's rule, restored by the W44.1 review (finding 2) and the lead: a bar's stiffness on the same wheel pair applies in place; a bar added, removed or moved makes the vehicle again.
- **Kept frames, per wheel** (a precision of decision 3, amended by the lead after the W44.2 review's finding 1): each wheel's `PhysicsVehicleWheelFrame {Center, Up, Forward}` relative to the body origin, formed from the vehicle's world scale (`TransformSystem::GetWorldScale`, the scale the body's colliders bake: finding 2) and the chain of local matrices below the vehicle down to the wheel (`MakePhysicsVehicleWheelFrame`, through `TransformSystem::DecomposeMatrix`). A frame is formed when its wheel joins a vehicle that is made, and kept once the creation succeeds. Decision 3 forms all of a vehicle's frames again when "the wheel set and order" changes; per wheel, a wheel added or disabled forms only its own frame, so the others keep their authored poses although the engine has written their Transforms. The attachment point is not kept: it is the kept centre plus `SuspensionMaxLength` up at each creation, so a new `SuspensionMaxLength` keeps the wheel hanging at its authored centre.
  - **The lead's amendment (finding 1):** a wheel's kept frame lasts the play session while the wheel belongs to that vehicle (the nearest `WheeledVehicle` above it): across a reparent below the same vehicle, across its `VehicleWheel` or the vehicle's `WheeledVehicle` being removed and added back (the vehicle's entry stays, not in force, with its frames kept and hashed), and while it is disabled. It goes when the wheel's entity is destroyed or another vehicle holds the wheel, and all of them when the vehicle's entity is destroyed. Frames are never formed again from a Transform the engine wrote: decision 3's "formed again when ... the chain of entities from a wheel up to the vehicle (a reparent)" and "a destroyed one or a removed component drops them" are replaced by this rule.
  - **A wheel joining another vehicle** (the review's follow-up, made exact): the session keeps each wheel's authored local Translation and Rotation (`PhysicsWheelAuthoredPose`) when its first frame is kept, before the engine first writes it, and a frame formed for it later comes from that pose under its chain then (`MakePhysicsVehicleWheelFrame`'s second form). The poses go with the wheel's entity and are hashed (below).
- **Item 4b** (`ApplyVehicleInput`, after the kinematic moves, teleports and `WakeOwners`, before the characters): each vehicle in force, in canonical order (on the branch `Scene::GetCanonicalOrder()`, PreStep's own order; M43's `CollectCanonical` at the merge), gets its `WheeledVehicle`'s input through `SetVehicleInput`; a sleeping body wakes when `Throttle` is not 0 or the input differs from the last applied one.
- **Wheel poses** (PostStep, after the write-back, before the dispatch): `CollectWheelPoses` gives each present wheel of a vehicle in force its local Translation and Rotation from `LocalPose`, through the inverse of the vehicle's world scale and the chain of local matrices below it down to the wheel's parent (finding 2: a vehicle under a scaled parent; the wheels' world centres are checked against their contacts); the pass walks PreStep's canonical order on the branch and moves to M43's `CollectCanonical<WheeledVehicleComponent>` at the merge, its visits counted in `EntitiesVisited`, as item 4b does (the review's nit 6; its writes are independent, so their order is not observable); `PhysicsSystem.cpp` writes them directly, only where a bit differs (the bodies' write-back's way; M43's `MarkTransformWritten` joins at the merge, in that file, as ADR 0061's `banned-transform-write` requires). The wheel's Scale stays its own.
- **Removal.** `RemoveBody` destroys the body's vehicle before its joints; `FlushDestroyed` removes the vehicles of entities that left play beside `RemoveLeavingJoints` (a destroyed one's entry and frames go, a disabled one's frames stay) and makes the plans stale; the destructor destroys every vehicle before the joints.
- **Priorities** (`RankConstraints`, replacing M37's `RankJoints`): the joints in force take their ranks in UUID order from 0, then the vehicles in force theirs, whenever either set in force changes.
- **The hash's vehicle part**, after the joint part: for each vehicle with kept frames in canonical order, its UUID, then each kept frame in UUID order of its wheel (the wheel's UUID, the bits of centre, up and forward), then, while in force, `PhysicsWorld::AppendVehicleState`; then each placed wheel's authored pose in UUID order of its wheel (its UUID, the bits of its local Translation and Rotation), which a frame formed for it later comes from. The early return also needs no kept vehicle frame and no authored pose. `CommittedConvoyHash` is re-recorded for the appended poses (`0xf3cdac7de44dc587`, Debug and Release); the simulation is unchanged.
- **Reads** (`PhysicsSystem::GetVehicleInfo(entity)`, the vehicle's or a wheel's entity, for W44.3's scripts and `physics.vehicleInfo`): `PhysicsVehicleReport {Entity, Body, Created, Speed, EngineRpm, Gear, Clutch, Shifting, Wheels}` and `PhysicsVehicleWheelReport {Entity, Touching, Point, Normal, Collider, SuspensionLength, SuspensionForce, Rotation, SteerAngle, AngularVelocity, LongitudinalSlip, LateralSlip}`: the spin wrapped into [0, 360) degrees, the steer positive to the right, the slip angle and spin speed in degrees; the collider through `ResolveHit`, so one disabled or leaving reads as none.

### Validation

`PhysicsVehicleInvalidCode` and `PhysicsVehicleDiagnosticCodes` (`Scene/PhysicsVehicles.h`), appended to both of `ProjectValidator.cpp`'s tables after `PHYSICS_MASS_UNUSED`; `CheckPhysics` appends the composition's diagnostics after the mass diagnostics; the count line adds the array. The fixture `Tests/Data/Validation/PHYSICS_VEHICLE_INVALID/` is a Dynamic sled with a `WheeledVehicle` and no wheel ("wheels"); `COMPONENT_ORDER` gains `WheeledVehicle` and `VehicleWheel` after `FixedJoint`.

### Tests

- `Scene/PhysicsVehiclesTests.cpp` (the composition): which body and wheels, nested vehicles and disabled entities; "PhysicsVehicles: each vehicle that cannot be made gets one PHYSICS_VEHICLE_INVALID naming its reason" (every subject, the first of several); a reference to a disabled wheel; the components in the world's units; a wheel's frame with the vehicle's scale.
- `Scene/PhysicsSystemVehiclesTests.cpp` (the session): the Acceptance cases "PhysicsVehicles: every path that removes a vehicle's body removes its vehicle first" (a RigidBody change, a refusal after an edit, the disable flush with a disabled wheel, the destroy flush, a destroyed wheel, the session's end), "PhysicsSystem: random vehicles and edits never reach a Jolt assert", "PhysicsVehicles: a vehicle keeps its wheels' frames when its body is created again", "PhysicsVehicles: wheel entities take their wheel's suspension, steer and spin, written only when they change", "PhysicsVehicles: a driver-input write wakes a sleeping vehicle and composes nothing", "PhysicsVehicles: the same buggy stops shorter on a rough surface than on a slick one, a ColliderSurface's and a TerrainCollider's alike", "PhysicsVehicles: one state reached through two histories steps identically" and "PhysicsVehicles: a scene without vehicles hashes as before"; the lead's two bar cases, "an anti-roll bar's stiffness changes in place, keeping the vehicle's motion, at one hash with 0, 1 and 8 Jolt threads" (twenty-four buggies) and "a removed anti-roll bar makes the vehicle again and leaves no stale force"; a vehicle that cannot be made raising once; the reads by either entity.
- `Session/VehicleDeterminismTests.cpp`: "PhysicsVehicles: a convoy driven by planned input ends in one state with 0, 1 and 8 Jolt threads" (twenty-four vehicles of every cast and transmission) and "PhysicsVehicles: the convoy's state hash equals the committed constant in every configuration" (`CommittedConvoyHash` `0xed9a4a11609ae85b`, recorded in Debug and verified in Release).
- What the cases observe: the carried motion and the in-place bar stiffness are read at a start flush's synchronization (`SynchronizeBodies`, no step in between), where they must equal the readings before exactly; the wheels' "written only when they change" is observed as a resting wheel's bits staying (with its spin damped to nothing, since Jolt turns a sleeping vehicle's wheels by their last spin), and from M44's merge M43's change tracking makes the writes themselves observable.
- `ProjectValidatorTests.cpp`: "ProjectValidator: PHYSICS_VEHICLE_INVALID reports a vehicle without a Dynamic body, without wheels or with a collider on a wheel".

### Documents

Architecture §5.3 (the two rows), §5.7 (steps 5 and 7) and §13.7 (the code); the game-debugging `validation-codes` row; `Docs/Backlog.md` §6's two limits of the session (the wheel-spin aliasing; a wheel field, the wheel set, the bars' wheels or the cast makes the vehicle again); the generated references. §9.11, the skills' pages and FeatureTest are W44.3's (decision 2); the feature suite reports the vehicle components UNCOVERED until then (the lead's ruling).

### The W44.2 review

`bin/Reviews/P8/M44-W2-review.md`: 0 blocker, 1 major, 4 minor, 8 nit, all fixed in one pass, in a checkpoint of their own before W44.3's; the departures 1 to 3 and 5 to 7 accepted, departure 4 replaced by the lead's amendment (finding 3). Each bug fix's case failed on the unfixed code first (`bin/Reviews/P8/scratch/M44-builder/w2fix-prove*.log`).

1. **Kept frames** (major): the lead's amendment above. "PhysicsVehicles: a wheel keeps its frame through a reparent and its components added back" drives the buggy until its rear left wheel's spin reads between 100 and 260 degrees, then reparents it below a new axle (and writes a wheel field, which makes the vehicle again: a reparent alone changes no body), removes and adds back its `VehicleWheel`, or removes and adds back the `WheeledVehicle`; braked to rest the chassis rides at its first rest height within 2 mm, and at full throttle it drives forward within 5 degrees of its heading.
2. **A scaled parent** (minor): the chain starts from the vehicle's world scale. "PhysicsVehicles: wheels are placed where the body holds them, under a scaled parent and through an axle" checks each wheel entity's world centre on its contact, one `Radius` above it, at its authored place times the world scale; the frame case gains a parent scaled 1.5.
3. **Disabled wheels** (minor): the lead's amendment above, with both outcomes in "PhysicsVehicles: a reference to a disabled wheel is no wheel; a vehicle it leaves drivable stays silent, an undriven one is reported" and the play-time "PhysicsVehicles: a vehicle whose disabled wheels leave it undriven raises PHYSICS_VEHICLE_INVALID in play".
4. **The two histories** (minor): the buggies stand on one Dynamic deck, which each vehicle constraint pushes on, so Jolt sorts the three in one island. The review's tow bars did not suffice: the solve order matters only between constraints that share a Dynamic body, and a re-added vehicle moved only among the other vehicles, never past a joint it shares a body with. Without the vehicles' ranks in `RankConstraints` the case fails.
5. **Registration** (minor): `Scene/Registration/VehiclesRegistrationTests.cpp`: the requirements and the category's place, the `CharacterController` exclusion departure 2 relies on, every ranged field's bounds against `Physics/PhysicsVehicle.h` with a write past each refused, the list elements' bounds, and the enums.
6. The wheel-pose pass's move to `CollectCanonical` at the merge (above).
7. Backlog §6: a sleeping vehicle's wheels keep turning by their last spin (presentation only); §9.11 and the `vehicles` page (W44.3) name it.
8. `EndGearChange`'s layout is checked against `SaveState`'s size before anything is written (`HasTransmissionStateLayout`; refused `Unsupported` otherwise), and named in the Risks.
9. "PhysicsValidation: ... InertiaScale makes too large for Jolt is reported on the InertiaScale" and "PhysicsSystem: ... is refused on the InertiaScale": a 40 km box of the heaviest mass, valid alone, refused with `InertiaScale` 100.
10. `ProjectValidator.cpp`'s include order.
11. `CheckPhysics` composes the bodies once, for the joints and the vehicles.
12. The subcase is "a destroyed wheel a differential names makes its vehicle invalid", and "a destroyed wheel nothing names leaves its vehicle made again on the other wheels" is added.
13. Lint on the changed sources, `Format.py --check` and `MakeValidationFixtures.py --check` ran on the final code, their logs kept in the scratch folder (`w2fix-*.log`).

## Before milestone completion

- [x] W44.1 committed: its first checkpoint `MassProperties`, then the world's vehicles. [x] The lead's review pass taken and every finding fixed.
- [x] W44.2 committed. [x] The lead's review pass taken and every finding fixed.
- [ ] W44.3 committed. [ ] The lead's review pass taken and every finding fixed.
- [ ] The milestone end: `milestone/m44` merged current main, and one independent review of the milestone diff, with every finding fixed.
- [ ] The landing:
  - phase 8's batch, second in train order, merged over M43's regions;
  - M44's entries first in its contended regions, with M47's and M48's after;
  - `BuiltinComponents::Size` and the count lines set by hand;
  - FeatureTest regenerated;
  - phase 8's `CI.py --require-clang-cl` green.
- [ ] Architecture, the Roadmap, the skills, the Backlog (§2 and §6) and the generated references describe what landed.
- [ ] S02's "How the game will check it" met in the cases, the suite and the replay, and reported to the Skerries session through the product owner, with the corrections above. The report includes that `MassProperties` is ready for S01's boat.
