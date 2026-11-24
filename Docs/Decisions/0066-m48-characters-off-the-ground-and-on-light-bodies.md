# 0066 — M48 Characters off the ground and on light bodies

- **Status:** accepted, the milestone's plan and record, in phase 8 of the Skerries program (`Docs/Decisions/0060-skerries-engine-program.md`).
  - **The record.** M48's builder amends this record on `milestone/m48` as each wave lands, in "The W48.1 record" to "The W48.3 record", making decision 3 exact where it leaves a choice to the milestone. W48.2's record opens with the investigation's run on the base, before any fix. The lead's reviews and the landing follow.
  - Lead-owned.
- **Date:** 2026-11-24.
- **Decided by:**
  - the product owner: the program (ADR 0060, accepted), which puts S17 and S19 in one milestone, phase 8's last lander;
  - the lead: the waves, the design defaults, S19's outcome (decision 4, question 1) and the ledger needs;
  - recorded by the lead.
- **Base:** main `3aaf4b10`, engine 0.8.0. Every value this record calls today's was read from the code and data at that commit.
- **Authority:**
  - Roadmap "Rules for every milestone";
  - ADR 0023; ADR 0028 decisions 10, 11 and 17;
  - ADR 0060 decisions 2, 4 (M48), 6, 7 and 9;
  - ADR 0014 decision 8 (M11's frozen `CharacterController.h`); ADR 0049 decisions 1 to 3 (M34's light bodies); ADR 0053 decisions 4, 11 and 12 (M36's ground, pushes and squeeze); ADR 0054 (M37's hinges); ADR 0030 decision 36 (M19's carry);
  - Architecture §9.6, §11.5, §13.5 and §15.6.
- **The briefs:** `Projects/Skerries/Design/EngineTasks/S17-CharactersOffTheGround.md` (medium to high, 2026-11-23) and `S19-CharactersOnLightBodies.md` (low to medium, 2026-11-24).

## Context

- **S17 asks** for a character's vertical velocity in the air, in the fixed step, deterministically, with grounding unchanged (a swimmer stands up in the shallows, a climber steps off at the top) and, with S01's height query, a swimmer held at a depth with no oscillation from the engine. Its checks: a 20 m swim with the feet 1.3 m (± 5 cm) under the sea for 10 s, back to standing; a 6 m ladder up and down; the autopilot's replays verifying everywhere.
- **S19 asks** that a character on a Dynamic body lighter than it stays on its surface, pushing it with its weight and walk and never sinking into it, whatever crossed it before; or, if the rule stays, a `project.validate` warning naming the body and the character. Deterministic as now. Its check: Ridgeback's rope bridge (thirteen 25 kg hinged planks under a 70 kg cartographer) crossed four times, each onto the far rim, the deck still giving underfoot.
- **The research.** S17: `bin/Reviews/P8/research/water.md`, its S17 section and "Across the group" (a per-tick `MoveFree(velocity)`, no field, no hash move). S19 arrived after the research; its part of decision 3 comes from this record's own read of the code below.
- **What the engine has today** (each checked at `3aaf4b10`):
  - **The step's velocity** (`Physics/CharacterController.cpp:613-715`): the desired horizontal part, plus, while grounded, the ground's vertical velocity and the desired vertical part, and in the air the previous vertical velocity plus gravity × dt (`:648-651`). Grounded is Jolt's `OnGround` while not leaving faster than `LeavingGroundSpeed` 0.1 m/s (`:91-94`, `:649-650`). Then `ExtendedUpdate` with stick-to-floor down to `StepHeight` and Jolt's stair step off (`:667-669`, `:679-680`), then the controller's own `WalkStairs` (`:364-418`, `:681`).
  - **The session** keeps the velocity of the last `MoveCharacter` (finite, each component at most `MaxCharacterSpeed` 500 m/s; `Scene/PhysicsSystemCharacters.cpp:23-37`; `Scene/Private/PhysicsSystemState.h:103-104`), and PreStep's item 5 consumes it (`Scene/PhysicsSystem.cpp:1581-1615`). The script method is `CharacterController:Move` (`Scripting/Bindings/Components.cpp:242-253`); the proxy has 7 methods (`:532-550`). The C++ `SetVelocity` (`CharacterController.cpp:741-748`) only seeds the next update and has no binding.
  - **The hash** holds each character's pose and velocity in the physics block (`PhysicsSystem.cpp:2562-2566`).
  - **Light bodies** (M34; ADR 0049 decision 2; Architecture §9.6, "Characters and Dynamic bodies"). A Dynamic body lighter than the character's `Mass` (`IsLighterThanCharacter`, `Physics/Private/CharacterContactRecorder.cpp:25-31`):
    - cannot push it: the recorder turns `mCanPushCharacter` off for every contact with it, the ground's included (`CharacterContactRecorder.cpp:49-60`, `:90-93`);
    - is a vertical wall to the slide up its upward faces, except the walkable ground of the last update (`:62-88`, the test at `:75`);
    - gives no ground velocity (`CharacterController.cpp:640-651`), is never a stair (`:357-362`) and takes at most its own weight (`PushGround`, `:420-444`);
    - never squeezes (`FindSqueeze`, `:458-459`).

    The documented result is that "a light platform never lifts or carries a character (it rises through one it is driven up under …)" (§9.6; `Docs/Backlog.md` §6, the M34 row), pinned by "CharacterController: a light Dynamic platform rising under a character passes up through it" (`Tests/Source/Engine/Physics/CharacterControllerTests.cpp:896-917`).
  - **No committed replay** puts a character on a light Dynamic body. FeatureTest's characters live in five scenes (`CharacterDebris`, `Components`, `Physics`, `Scripting`, `Surfaces`). Only `Surfaces.replay` plays one: its `Rider` (70 kg) stands on the static `Level` belt, with the 5 kg `Crate` 1.3 m beside it (`Projects/FeatureTest/Assets/Scenes/Surfaces.scene`). No tracked demo holds a `CharacterController`.
- **S19's investigation: the most likely cause, UNVERIFIED until W48.2's first run on the base.** The sink is M34's documented rule meeting a deck that moves, and the stall follows from it.
  1. **A character never rises with a light body it stands on.**
     - Jolt adds a penetration's recovery to a contact's constraint velocity (`Vendor/JoltPhysics/Jolt/Physics/Character/CharacterVirtual.cpp:706-717`), then zeroes that whole velocity for a contact that cannot push the character (`:924-926`). So neither the plank's rise nor an overlap moves the character up.
     - The controller gives a grounded character on a light body no vertical velocity (`CharacterController.cpp:640-651`).
     - Stick-to-floor follows the deck down only when the update loses its support (`CharacterVirtual.cpp:1808-1825`).

     So on a light deck the character's height follows the deck's lows, and every rise of the plank under it passes into the capsule.
  2. **Why the first crossing works.**
     - In a fresh session the deck is at rest, and the plank under the character only goes down as it is loaded (`PushGround`).
     - The crossing leaves the deck swinging: the walk pushes the planks with up to `Mass` × 9.81 N (`CharacterVirtual.cpp:764-811`; `CharacterController.cpp:566`), and each plank springs back as the character leaves it.
     - With the default damping of 0.05 /s (`Scene/Components/RigidBodyComponent.h:40-41`), about 60 % of the swing is left 10 s later. So on the way back the planks rise under the character, and the sink builds to about the swing's range.
     - The character stays grounded while the plank's top meets the capsule below its lower hemisphere's centre (`mSupportingVolume`, `CharacterController.cpp:567-568`; `CharacterVirtual.cpp:1118-1134`). So the sink stays below `Radius`: the brief's 0.17 to 0.24 m.
  3. **Why it stalls.** With its feet inside the deck, what lies ahead is a step up from the sunk feet: the next plank's end where the deck climbs, or the rim.
     - A light body is never a stair (`CharacterController.cpp:357-362`; Jolt's own step is off, `:668`).
     - The upward face of a light body that is not yet the ground is a wall (`CharacterContactRecorder.cpp:62-88`).
     - The static rim is climbed only up to `StepHeight` (`CharacterController.cpp:364-418`), which the sink plus the rim's height above the deck can exceed. Stopping 0.3 m short is the default `Radius` (`CharacterController.h:81`) against the rim's face.
  4. **The 4.2 m/s.** `GetVelocity` is Jolt's linear velocity, which the update sets to its step velocity before moving (`CharacterController.cpp:662-663`, `:736-739`). Jolt never replaces it with the distance covered (`CharacterVirtual.cpp:1442-1484`; `:1801-1803` cancels only velocity into steep slopes, off the ground). §9.6 "Pushes and the squeeze" already says so.

  **What a run must confirm:** the plank's upward velocity under the character on the second crossing; the ground contact's depth growing tick by tick with `mCanPushCharacter` off; and which contact stops the character.
- **Corrections to the brief** (to the Skerries session through the product owner):
  - **S17, air control** (ADR 0060 decision 9): in the air, `Move`'s horizontal part already applies in full every tick. Only the vertical is missing.
  - **S17, `SetVelocity`:** the C++ setter only seeds the next update, which adds gravity again, so binding it would not give the brief's behaviour.
  - **S17, the swim:** M47's water lands in the same phase, so M48 proves the depth hold against a moving target height. The swim over real water is the game's check after phase 8.
  - **S19, the rule:** the sink is the documented rule, not a separate defect: a light platform "rises through" a character. ADR 0060's context calls the sink "not that rule", and this record corrects it. Decision 3 changes the rule (outcome (a)).
  - **S19, the velocity:** `GetVelocity` is the velocity the update asked for, not the distance it covered, so a blocked character reports its walk. M48 keeps it, since it is hashed, and says so in its description.
  - **S19, the weight:** a light body takes at most its own weight from the character (M34), so a 25 kg plank is pressed with 25 kg's weight, not 70 kg's. M48 keeps the cap (decision 4, question 2).

## Decisions

### 1. Scope

- **In:**
  - **S17:** `CharacterController:MoveFree(velocity)`, a per-tick free move: in the air, the requested velocity with no gravity and no floor sticking; on the ground, `Move`.
  - **S19:** the investigation; a regression case of the brief's shape that fails on the base; the fix; outcome (a), a light body supporting the character that stands on it. The fix is outcome (a): decision 4, question 1.
- **Out** (`Docs/Backlog.md` §2 at the landing):
  - a locomotion mode field (Walk, Swim, Climb) or engine swim and climb states: scripts compose `MoveFree`;
  - buoyancy and water drag on characters;
  - a light body carrying its rider along with its own motion (M55's `RideGround`, phase 10, decides riding);
  - `GetVelocity` as the distance covered (a hashed value; every character scene's hash would move);
  - a `project.validate` warning about light bodies (outcome (b), not taken).
- **Accepted at landing** (§6):
  - A light body takes at most its own weight from the character (M34's cap, restated for the support).
  - A light body that is not yet the character's ground (one it steps onto, or one rising into it from below while it stands elsewhere) passes into the capsule until an update stands on it. The next update then lifts the character onto its top.
  - A light body never squeezes a character.
  - A free move that starts grounded is a `Move`, so it is stuck to the floor down to `StepHeight` when it leaves a ledge.

### 2. Structure: one milestone, three waves

| Phase | # | Name | Briefs | Lands | Gate | Size (builder-sessions) |
|---|---|---|---|---|---|---|
| 8 | M48 | Characters off the ground and on light bodies | S17, S19 | last in phase 8's fixed train order (M43, M44, M45, M46, M47, M48; ADR 0060 decision 2) | the batch's `CI.py --require-clang-cl` | ~4 (W48.1 ~1.5, W48.2 ~1.5 + 0.5 at risk, W48.3 ~1) |

One builder, on `milestone/m48` in `D:/Engine/.claude/worktrees/m48`, works the waves in order:
- **W48.1, free moves (S17):**
  - `CharacterMoveMode` and `Update`'s mode;
  - the session's mode and `LastMove`;
  - `MoveFree`'s binding;
  - `physics.bodyInfo`'s `lastMove`;
  - the unit cases.

  Its first checkpoint fixes the frozen header's change, M34's way.
- **W48.2, characters on light bodies (S19), in this order:**
  1. The investigation. The four-crossing regression case is written first and run alone on the base. It must fail there, and the record keeps its measured sink and stall per crossing, with the cause confirmed or replaced.
  2. The fix.
  3. M34's rising-platform case rewritten to the new rule.
  4. The rule's comments and §9.6.

  If the base run refutes decision 3's cause, the builder records what it found and the lead decides the fix before the wave goes on.
- **W48.3, coverage and documents:**
  - FeatureTest's scaffold, suite and replay;
  - the Python case;
  - Architecture, the Backlog and the skills;
  - the generated references.

Numbering: M48's record is this ADR, 0066, phase 8's last. The next record takes 0067.

### 3. Design defaults: M48

**What M48 adds:** one script method, one parameter of a frozen function, one `physics.bodyInfo` result field, one FeatureTest scene, suite and replay, and a change to M34's light-body rule.

**What it does not add:** a component or field, a validation code, a namespace, a settings section, an `IScriptHost` member, an automation method, a callback, a stats row, a pass, a format version, a state-hash block, a `PlaySessionPhase`, a fuzz target, a golden, a perf metric or a library.

**S17, free moves (W48.1):**
- **`CharacterController:MoveFree(velocity: vector) -> ()`**: a method beside `Move`, with no field.
  - It is consumed by the next PreStep, as `Move` is, and the last `Move` or `MoveFree` of a tick decides the update.
  - It is checked as `Move` is: `BoundedVector` to `MaxCharacterSpeed`, then the session's finite and range test.
- **The update.** `CharacterController::Update` gains a trailing `CharacterMoveMode mode = CharacterMoveMode::Walk`. `enum class CharacterMoveMode : uint8_t { Walk, Free }` sits in `CharacterController.h`, a reviewed change to a frozen interface (ADR 0014 decision 8), so every existing call keeps its meaning. A free update is:
  - **grounded** (today's test, `:649-650`): exactly `Move`. A swimmer touching the bottom stands, and an upward part leaves the ground as a jump does.
  - **in the air:** the step's velocity is the whole desired velocity, horizontal and vertical, through `ClampSpeed`, with no gravity and with stick-to-floor off for that update (`mStickToFloorStepDown` zero). The controller's `WalkStairs` stays, so a swimmer steps out onto a stair.
  - **afterwards:** the velocity is what the script asked, so a `Move` in the air continues from it plus gravity (a swimmer surging out of the water keeps rising briefly).
- **The session:**
  - `PhysicsSystem::MoveCharacter` gains a trailing `CharacterMoveMode mode = Walk`.
  - `PhysicsCharacterEntry` gains `DesiredMode`, consumed and reset with `DesiredVelocity` in PreStep's item 5 (`PhysicsSystem.cpp:1613-1614`).
  - `PhysicsCharacterState` (`PhysicsSystem.h:368-382`) appends `LastMove`: `Free` when the last update was a free move in the air, `Walk` otherwise. It is derived and not hashed, like `IsSqueezed`.
- **Idioms** (the luau-gameplay skill's `characters-and-input` page):
  - **swim:** target height = surface − depth; vertical = (target − y) / `Time.GetFixedDeltaTime()`, clamped; `MoveFree(horizontal + up × vertical)`;
  - **climb:** a trigger marks the ladder; `MoveFree(intoWall × 0.5 + up × climbSpeed)`; at the top a forward `Move` steps off;
  - **glide:** `MoveFree` with a chosen sink rate;
  - **ledge hold:** `MoveFree(vector.zero)`.

  A character that stops calling `MoveFree` in the air falls from its last velocity.
- **Rejected:**
  - **A mode field** (`Locomotion`). Defaults are serialized (`Projects/FeatureTest/Assets/Scenes/Physics.scene:534-543`), so a new field moves every character scene's hash (M19's precedent, ADR 0030 decision 36), and it is state the script must keep in step with its own.
  - **Binding `SetVelocity` with `GravityFactor` 0.** This is the brief's named workaround, and it needs a hashed field write every tick.

**S19, light bodies (W48.2).** Outcome (a): a Dynamic body lighter than the character supports the character standing on it.
- **Which body:** the walkable ground of the character's last update (`OnGround` and Jolt's ground body), the test `OnContactSolve` already makes (`CharacterContactRecorder.cpp:75`). One helper serves both rules.
- **How:**
  - `ApplyLightBodyRule` (`:49-54`) leaves `mCanPushCharacter` on for that body's contacts.
  - Jolt's contact plane and penetration recovery (`CharacterVirtual.cpp:706-717`) then hold the character on the body's walkable surface. A plank rising under it lifts it at the plank's velocity along the surface's normal, and an overlap left by the last step is pushed out within the update.
- **Per face.** Jolt keeps one `CharacterContactSettings` per body and sub-shape and reuses it within an update (`CharacterVirtual.cpp:495-527`), so the switch is per body. The recorder's `OnContactSolve` answers a contact of the light ground whose surface is too steep to walk as if the body stood still, with no push and no recovery. So only the walkable surface supports, and the ground's sides never shove the character. W48.2 fixes the exact test in its record.
- **What stays M34's:**
  - the body's velocity is never the character's (`CharacterController.cpp:640-651` unchanged). The lift is a displacement; the character's velocity, the hash's and a jump's take-off never take the body's, so nothing launches it;
  - every other light body still cannot push the character and is a wall to its slide;
  - a light body is never a stair;
  - it takes at most its own weight (`PushGround`);
  - it never squeezes (`FindSqueeze`);
  - the walk pushes it with at most `Mass` × 9.81 N.

  The weight and walk push a light body deterministically, as today: main thread, canonical order, the world's no-lock interfaces (ADR 0049 decision 3).
- **The stall** needs nothing more if the cause holds: a character on the surface crosses from plank to plank as it does on today's first crossing. If the four-crossing case still stalls at a joint after the fix, W48.2 records which contact stops it, and the lead decides the change (Risks).
- **Rejected fixes:**
  - **The light ground's vertical velocity as ground velocity, as a heavy ground's.** This is M34's launch: a velocity outlives the ground, and a 0.35 kg plate launched a character to 26.5 m (ADR 0049 fact 8).
  - **A lift of the controller's own after the update** (a capsule query against the light ground). It runs a tick behind a rising deck (8 mm a tick at 0.5 m/s) and needs a second sweep against ceilings, where Jolt's recovery already runs inside its solve.
  - **`OnAdjustBodyVelocity` (`CharacterVirtual.h:163`) zeroing a light body's velocity.** It would change every push against that body (`HandleContact`'s relative velocity, `CharacterVirtual.cpp:777-779`), which today stops a thrown light box.
- **M34's rising-platform case changes by this decision.** Its scenario stays: a 5 kg plate driven up at 1.5 m/s under a standing character. Its name and checks become the new rule: "CharacterController: a light Dynamic platform rising under a character lifts it on its surface and gives it none of its velocity". The character rides within 3 cm of the plate's top, `GetVelocity().y` stays below 0.1 m/s, and it stops when the plate stops. This is a decided change of a documented rule, recorded here, not a weakened test. Every other M34 and M36 case runs unchanged and must pass, among them "a character standing on a light Dynamic body is not launched by it" (`CharacterControllerTests.cpp:734`) and the shard walk-through (`:622`).

**Scripts, automation and the editor:**
- **Scripts.** `MoveFree` is registered on the `CharacterController` type after `Move` (`Components.cpp:533`), mutating and in every mode, with this description: "Supply the velocity of the next fixed character update as a free move: in the air the character takes it whole, its vertical part included, with no gravity and no floor sticking (swimming, climbing, gliding); on the ground it is Move. Call it every OnFixedUpdate; the last Move or MoveFree of a tick wins."
- **Descriptions.**
  - `GetVelocity` (`:536`): "The velocity the last update asked for (its step velocity), not the distance it covered: a blocked character still reports it."
  - `Mass` (`Scene/Registration/PhysicsRegistration.cpp:133-136`): a Dynamic body lighter than the character supports it where it stands on it, and never pushes, launches or carries it.
- **Automation.** No method: 126 methods and 86 MCP tools stay. `physics.bodyInfo`'s result appends `lastMove` ("Walk" or "Free" for a character, empty for other bodies), described, read in `test_physics.py` (rule 5). The add-automation-method skill's steps apply.
- **The editor:** nothing new; the inspector is generic.

**FeatureTest (W48.3; IdSeeds 4501 and 4502, in M48's block 4500-4599 of phase 8's ledger).**
- **`CharacterMoves.jsonl`** makes `CharacterMoves.scene` and its suite `Physics.CharacterMoves` (`Assets/Tests/CharacterMoves/CharacterMoves.test.luau`), bound in `Suites.jsonl`. The scene holds:
  - a shore with a shallow shelf and an open pit for the swimmer;
  - a 6 m ladder (a static wall marked by a trigger) and a platform at its top;
  - a bridge of thirteen 25 kg box planks hinged end to end with `HingeJoint`s, the end planks hinged to the world at two static rims;
  - characters of `Mass` 70.
- **The suite, in the editor, Release and Dist:**
  - **swim:** `MoveFree` holds the swimmer's feet 1.3 m (± 5 cm) under a swell the script computes from the tick (a sine standing in for M47's water) for 10 s while it swims 20 m out and back, then it stands in the shallows and walks with `Move`;
  - **climb:** up the ladder with `MoveFree`, a forward `Move` onto the platform (grounded at the top), then down again;
  - **bridge:** four crossings with `Move` at 4.2 m/s, turning back at once, each onto the far rim. The walker is never more than 3 cm under the plank it stands on, and the plank under it sits lower than unloaded.
- **`CharacterMovesReplay.jsonl`** records `CharacterMoves.replay` from a recording suite: the swim, the climb and one bridge crossing driven by injected input. The determinism suite verifies it with `strictHash` in Debug, Release and Dist, and its input check fails it without its input.
- **Gates:**
  - 2: `CharacterController.MoveFree`;
  - 7: `MoveFree`'s and `lastMove`'s descriptions;
  - 1, 1b, 3, 4, 5, 6, 8 and 9: nothing new.

**Tests.**
- **W48.1:**
  - `CharacterControllerTests.cpp`: the free move in the air, grounded, not stuck to the floor, following a moving target, climbing a wall, and a `Move` after a free move (Acceptance);
  - `PhysicsSystemCharactersTests.cpp`: the refusal and the last call of a tick;
  - `Scripting/Bindings/PhysicsTests.cpp`: the binding;
  - `ScriptProxyTests.cpp`'s write list (`:323`) and `ScriptBindingFuzzTests.cpp`'s families (`:91`) gain `MoveFree`;
  - `PhysicsMethodsTests.cpp`: `lastMove`.
- **W48.2** (`CharacterControllerTests.cpp`): the four-crossing bridge, which fails on the base; a plank swinging up under a standing character; the rewritten platform case; the walkable-surface rule; and the bridge at Jolt thread counts 0, 1 and 8 (the pattern of `PhysicsWorldTests.cpp:706`).
- **The bridge case's shape:**
  - thirteen 25 kg box planks, hinged end to end through `PhysicsWorld::CreateJoint`, the end planks to the world;
  - a default 70 kg character (`Radius` 0.3, `StepHeight` 0.3) at 4.2 m/s, turning back at once at each rim so the deck still swings;
  - each crossing reaches 0.5 m onto the far rim within a tick budget;
  - on the deck, the base is never more than 3 cm under the top of the plank it stands on (`GetGroundState().GroundBody`'s top plane at the character);
  - the plank underfoot sits lower than unloaded.

**Documents.**
- **Architecture:**
  - §9.6: "Characters and Dynamic bodies" (the support), "Pushes and the squeeze" (a light ground's support is not a push) and "Character conventions" (`MoveFree`);
  - §11.5's `CharacterController` row;
  - §13.5's `physics.bodyInfo` result list;
  - §15's FeatureTest scene table (`CharacterMoves.scene`).
- **Header comments:** `CharacterController.h` (`:20-31`, `:55-57`, `:86-89`, `:161-182`), `CharacterController.cpp` (`:48-52`), `CharacterContactRecorder.h` (`:28-35`), `PhysicsSystem.h` (item 5, `MoveCharacter`).
- **The Roadmap's M48 section.**
- **`Docs/Backlog.md`:** §2's rows and §6's M34 row (light platforms now support).
- **The skills:**
  - luau-gameplay's `characters-and-input`: the idioms, and its "Light bodies never move a character" bullet rewritten (`.claude/skills/luau-gameplay/characters-and-input.md:8`);
  - game-debugging: `lastMove`, and a stalled character's velocity.
- **The generated references.**

### 4. The questions, decided by the lead

1. **S19's outcome: (a).**
   - The sink is the documented rule itself, so keeping the rule keeps the trap. ADR 0060 decision 4 asks for the sink fixed, and fixing it is (a).
   - (b) would warn about every rope bridge, raft and dinghy the game wants, and leave the 90 kg workaround the brief names.
   - (a) adds no field, so M55 keeps the next character field to itself.
2. **The weight on a light ground stays capped at its own weight.**
   - With the support positional, a light ground pressed into the floor by the full weight bounces and would lift its rider (M34's plate, ADR 0049 fact 8, restitution 0.8).
   - The deck still gives, which the bridge case checks.
   - §6 restates the cap.
3. **A light ground carries no horizontal motion:** riding is M55's `RideGround`, phase 10.
4. **S17's shape:** the per-tick `MoveFree` (research question 9; ADR 0060 decision 4).
5. **A free move on the ground is `Move`, with stick-to-floor off only in the air** (ADR 0060 decision 4's wording; the research's design).
6. **The swim's acceptance uses a computed swell, not M47's water.** Same-phase milestones share nothing but the ledger (Roadmap rule 3; ADR 0028 decision 17). This departs from the research's "needs S01 in the tree".
7. **`physics.bodyInfo` reports `lastMove`** (the research's observability), since a free move in the air is otherwise invisible to an agent.

### 5. Approvals

None. Nothing is downloaded or vendored.

### 6. Determinism, platforms and goldens

- **S17.** A scene that never calls `MoveFree` simulates bit for bit as before. The mode is per tick, consumed with the desired velocity, and never serialized or hashed; the pose and velocity it produces are hashed as today.
- **S19.**
  - **What moves.** The rule changes every run in which a character stands on a light Dynamic body whose walkable surface moves into it.
  - **No committed hash moves.** No committed replay holds such a run (Context), which `Scaffold.py --check` and the determinism and games suites confirm at the gate. ADR 0060 decision 6's "none" holds for committed hashes. A FeatureTest replay that moved anyway would be recorded again by `Scaffold.py` at the landing and named in the record.
  - **The games.** World Tree's and Skerries' replays in which a character stands on a moving light body are theirs to record again, through the product owner. Skerries' Ridgeback planks are 90 kg today, so they are not light.
- **Determinism.** The rule runs on the main thread in the character's update, as M34's did. The bridge case holds one pose at Jolt thread counts 0, 1 and 8, and `CharacterMoves.replay` holds one hash in Debug, Release and Dist.
- **Platforms.** Only Windows is verified (ADR 0028 decision 10). M48 adds no platform code.
- **Goldens.** None changes or is added: all 56 stay byte-identical. No fuzz target is added; `ScriptBindingFuzzTests` gains a family.

### 7. Process

As ADR 0060 decision 7:
- **No contract.** One builder works the waves in its worktree. The lead runs one independent review per wave and has every confirmed finding fixed, nits included.
- **Frozen header.** `CharacterController.h` changes only as decision 3 names, as a reviewed change to a frozen interface (ADR 0014 decision 8).
- **Checkpoint commits** run their targeted checks:
  - the filtered unit cases (`CharacterController:*`, `PhysicsSystem:*`, `PhysicsBindings:*`, `ScriptProxy*`, `PhysicsMethods:*`);
  - `test_physics.py`;
  - the feature suite filtered to `Physics.CharacterMoves`;
  - `Scaffold.py --check`;
  - Lint and Format.

  GPU runs go through `Test.py` only.
- **The landing.** M48 is phase 8's last lander. `landing/phase8` merges M43 to M47 in train order, then M48, which appends after their entries in every shared file. The lander regenerates FeatureTest and sets `EngineVersionString` to 0.9.0 (`Scaffold.py` on the merged tree, which re-records every FeatureTest replay at 0.9.0). The gate is one `CI.py --require-clang-cl` on the merged tree. Main fast-forwards only after it is green, is pushed, and the work pauses for the owner's go-ahead.

### 8. Reservation needs

Phase 8's reservation ledger, committed in `Docs/Decisions/0060-skerries-engine-program.md` as its section "Phase 8 reservation ledger", is the rule: the rows below are M48's rows there, with its values, orders and owners, in the fixed train order M43, M44, M45, M46, M47, M48. A deviation needs a ledger amendment on main. Today's values are read at `3aaf4b10`.

| Allocation | Today | M48 |
|---|---|---|
| ADR numbers | 0059 the last committed; 0060 the program, 0061 to 0065 phase 8's other milestones | 0066, this record; the next takes 0067 |
| FeatureTest IdSeeds | 101 scaffolds, the highest 3902 (`GoldenPlanarReflections.jsonl`) | `CharacterMoves.jsonl` 4501 and `CharacterMovesReplay.jsonl` 4502 (M48's block 4500-4599), listed after M43 to M47's and before `Suites.jsonl` (1515) and `Replays.jsonl` (1516): 118 scaffolds and the highest seed 4502 on the merged tree |
| Suites, replays and scenes | 116, 26 and 104 | +1 suite (`Physics.CharacterMoves`), +1 replay, +1 scene: 123, 32 and 116 on the merged tree |
| Components and categories | 64 components, 16 categories (`BuiltinComponentsTests.cpp:44`) | none; `CharacterController` stays version 1, no field (M55 adds `RideGround` later); `Mass`'s description changes |
| Validation codes | 75, the last `RENDER_REFLECTION_INVALID` (`ProjectValidatorTests.cpp:1452-1453`) | none |
| Settings sections | as ADR 0059's ledger | none |
| Script namespaces and proxies | 22 namespaces; the `CharacterController` proxy has 7 methods (`Components.cpp:532-550`) | no namespace; `CharacterController:MoveFree` after `Move` (`Components.cpp:533`; 8 methods), the last of phase 8's 18 proxy methods; `GetVelocity`'s description |
| `IScriptHost` | ends with `GetMusic` (`Scripting/ScriptHost.h:250`) | none (`MoveFree` reaches physics through `GetPhysics`, `Components.cpp:161-167`) |
| Automation methods and MCP tools | 126 and 86 (`Docs/Reference/Methods.json`, `Tools/MCP/catalog.json`) | unchanged; `physics.bodyInfo` appends `lastMove`, after M44's and M47's fields and before `tick` |
| Stats rows, pipelines, passes, `ViewConstants`, `DrawConstants`, draw flags | as ADR 0059's ledger | none |
| Goldens, perf metrics, fuzz targets | 56 goldens on `nvidia-62x`; as ADR 0059's ledger | none |
| State-hash blocks, formats, `PlaySessionPhase` | the physics block's character entry: pose and velocity (`PhysicsSystem.cpp:2562-2566`) | none; PreStep's item 5 changes in place |
| `EngineVersionString` | 0.8.0 (`Core/BuildId.h:38`) | 0.9.0 at M48's merge (the last lander's) |

**M48's own files:**
- **New:** `Assets/Tests/CharacterMoves/` and its scripts, the two scaffolds, `CharacterMoves.scene` and `CharacterMoves.replay`.
- **Changed:**
  - `Physics/CharacterController.{h,cpp}` and `Physics/Private/CharacterContactRecorder.{h,cpp}`;
  - `Scene/PhysicsSystem.{h,cpp}`, `PhysicsSystemCharacters.cpp` and `Private/PhysicsSystemState.h`;
  - `Scripting/Bindings/Components.cpp` and `Scene/Registration/PhysicsRegistration.cpp`;
  - `Automation/Methods/PhysicsMethods.{h,cpp}`;
  - `Core/BuildId.h`, at its merge (`EngineVersionString` 0.9.0);
  - their tests.

**Contended files and M48's regions** (M48 lands last and adapts; the generated references are regenerated, never merged):

| File | Phase 8's owner | M48's region |
|---|---|---|
| `Physics/CharacterController.h` (frozen by M11) | M48 alone | the file comment's light-body paragraph (`:20-31`) and frozen list (`:55-57`); `CharacterMoveMode` before the description (`:75`); `Mass`'s comment (`:86-89`); `Update` (`:161-182`) |
| `Physics/CharacterController.cpp` | M48 alone (M44's unwrapping is in `PhysicsContactListener.cpp`) | the file comment (`:31-81`) and `Update` (`:613-715`) |
| `Physics/Private/CharacterContactRecorder.{h,cpp}` | M48 alone | `ApplyLightBodyRule` (`:49-54`), `OnContactSolve` (`:62-88`), the comment (`.h:28-35`) |
| `Scene/PhysicsSystem.cpp` | M44 (M47 and M48 append) | inside PreStep's item 5, the update call and the consumed state (`:1608-1614`); M44's item 4b and M47's item 4c go, in that order, between `WakeOwners` (`:1579`) and item 5 (`:1581`); M48's change is inside item 5, after both |
| `Scene/PhysicsSystem.h` | M44 | the PreStep list's item 5 (`:72`, after M44's 4b and M47's 4c), `PhysicsCharacterState` (`:368-382`), `MoveCharacter` (`:513-518`) |
| `Scene/Private/PhysicsSystemState.h` | M44 | `PhysicsCharacterEntry` (`:98-112`) |
| `Scene/PhysicsSystemCharacters.cpp` | M48 alone in phase 8 (M55 in phase 10) | `MoveCharacter` (`:23-37`), `MakeCharacterState` (`:39-61`) |
| `Scene/Registration/PhysicsRegistration.cpp` | M44 (`MassProperties`); M48 the description only | `CharacterController`'s `Mass` description (`:133-136`) |
| `Scripting/Bindings/Components.cpp` | M48 alone | `Utils::CharacterMove` (`:242-253`) and the `CharacterController` type (`:532-550`) |
| `Automation/Methods/PhysicsMethods.{h,cpp}`, `PhysicsMethodsTests.cpp`, `Tests/Automation/test_physics.py` | M44 (M44's `centerOfMass`, then M47's `buoyancy`, then M48's `lastMove`, all before `tick`) | `PhysicsBodyInfoResult`'s last field before `tick` (`.h:55-82`, `.cpp:205-233`) |
| Tests | append | `CharacterControllerTests.cpp`, `PhysicsSystemCharactersTests.cpp`, `Scripting/Bindings/PhysicsTests.cpp`, `ScriptProxyTests.cpp` (`:323`), `ScriptBindingFuzzTests.cpp` (`:91`, after M47's water calls) |
| FeatureTest `Scaffolds.json` and `Suites.jsonl` (never `Replays.jsonl`) | append in train order | M48's entries last |
| `Engine/Source/Engine/Core/BuildId.h` | M48 | `EngineVersionString` 0.9.0 (`:38`) |
| Architecture §9.6, §11.5, §13.5 and §15; Backlog; the skills | merged by hand | as decision 3's documents |

Not touched: `BuiltinComponents.h`, `ProjectValidator.cpp`, `RegisterBindings.cpp`, `RegisterMethods.cpp`, `MakeValidationFixtures.py`, the renderer and the shaders.

## Acceptance

The Roadmap's M48 acceptance, each a named test or check:
- "CharacterController: a free move in the air takes the whole velocity asked, and no gravity acts"
- "CharacterController: a free move while grounded is a Move"
- "CharacterController: a free move in the air is not stuck to the floor"
- "CharacterController: a character free-moved toward a moving target height follows it within 1 mm"
- "CharacterController: a climber free-moved along a wall rises along it and steps off at the top onto the floor"
- "CharacterController: a Move in the air after a free move continues from its velocity plus gravity"
- "PhysicsSystem: a free move faster than MaxCharacterSpeed is refused"
- "PhysicsSystem: the last Move or MoveFree of a tick decides the next update"
- "PhysicsBindings: CharacterController:MoveFree reaches the next update as a free move and refuses what Move refuses"
- "PhysicsMethods: physics.bodyInfo reports a character's last move"
- "CharacterController: a character crosses a bridge of thirteen hinged 25 kg planks four times, each onto the far rim, and never sinks into a plank" (fails on the base)
- "CharacterController: a character on a light Dynamic plank that swings up under it rides its surface and keeps none of its velocity"
- "CharacterController: a light Dynamic platform rising under a character lifts it on its surface and gives it none of its velocity" (M34's case, rewritten by decision 3)
- "CharacterController: only the walkable surface of the light body a character stands on supports it, and no light body pushes it sideways"
- "CharacterController: four bridge crossings end at one pose at Jolt thread counts 0, 1 and 8"
- Every existing M34 and M36 character case, unchanged.
- `test_physics.py`'s `lastMove` case.
- The `Physics.CharacterMoves` suite passes in the editor, Release and Dist; `CharacterMoves.replay` verifies at one hash in Debug, Release and Dist.
- `Scaffold.py --check`, the determinism and games suites: no committed hash moves.

**S17's "How the game will check it":**
- **The swim:** feet 1.3 m (± 5 cm) under the sea for 10 s, 20 m out and back, standing in the shallows.
  - "follows it within 1 mm" and "a free move while grounded is a Move";
  - the suite's swim against a computed swell;
  - the game's suite over M47's water after phase 8 lands.
- **The ladder:** 6 m up, off at the top, down again. "a climber free-moved along a wall …" and the suite's climb.
- **The autopilot's replays still verify.** A scene without `MoveFree` simulates as before, and `CharacterMoves.replay` verifies in every configuration.

**S19's "How the game will check it":**
- **`Ridgeback: the rope bridge carries the cartographer back again, and again` with `PLANK_MASS` 25:** four crossings onto the far rim with the deck giving.
  - The four-crossing case plays the brief's numbers, as does the suite's bridge.
  - The game's own suite is its check after the landing, its planks back at 25 kg.
- **Deterministic:** the thread-count case and the replay.

## Risks

- **The cause is UNVERIFIED.** If the unit bridge does not fail on the base (its swing differing from Ridgeback's), W48.2 reproduces the brief's repro with `HingeJoint` components in a `PhysicsSystem` case (cross, then turn back at once) before any fix.
- **A second stall mechanism.** At a joint, the next plank's upward face is a wall until the character's ground switches to it (`CharacterContactRecorder.cpp:75`). If the fixed bridge still stalls there, that rule needs its own change (+0.5 session).
- **The rule reaches M34's cases.**
  - A shard that becomes a character's ground would now lift it. The shard walk-through, the launch case and `Physics.CharacterDebris` run unchanged and must pass.
  - Jolt's per-body settings make the face rule depend on `OnContactSolve`. A Jolt update re-reads both (`Vendor/JoltPhysics/VENDOR.md`'s procedure).
- **Contention.** M48 lands last and adapts:
  - PreStep's item 5 follows M44's 4b and M47's 4c;
  - `physics.bodyInfo`'s result gains M47's fields too;
  - `Components.cpp`, `PhysicsRegistration.cpp`, and Architecture §9.6 and §13.5 are shared.
- **The game's part.** The swim over real water needs M47, and replays with a character on a moving light body are re-recorded. Both go to the Skerries and World Tree sessions through the product owner.

## Before milestone completion

- [ ] W48.1 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] W48.2 committed, its record holding the base run (the bridge case failing, its sink and stall, the cause); [ ] the lead's review pass taken and every finding fixed.
- [ ] W48.3 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] The landing: phase 8's batch, M48 last, appending after M43 to M47 in every shared file; FeatureTest regenerated and `EngineVersionString` 0.9.0; `CI.py --require-clang-cl` green; main fast-forwarded and pushed.
- [ ] S17's and S19's "How the game will check it" met as the Acceptance maps them, and reported to the Skerries session through the product owner with the corrections above. The light-body rule's change is reported to the World Tree session too.
- [ ] Architecture, the Roadmap, the skills, the Backlog (§2, and §6's M34 row) and the generated references describe what landed.
