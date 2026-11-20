# 0058 — The World Tree program's phase 7: brief 21's fixes and M41 Bone attachments

- **Status:** accepted. Phase 7's plan and M41's milestone record in one document, as ADR 0038 was for M27, alone in its phase.
  - **Approval.** The product owner approved phase 7 on 2026-11-19 ("lets tackle both", after briefs 21 and 22 were reported). The lead's milestone, process and design defaults follow.
  - **Landing.** The plan lands on main as a documentation-only change, with the Roadmap's M41 section, introduction, dependency row and merge train, `Docs/Backlog.md`'s §1 and §2 rows, and Architecture §18's row.
  - **The record.** M41's builder amends the record on `milestone/m41` as each wave lands, in "The W41.1 record" and "The W41.2 record", making decision 3 exact where it leaves a choice to the milestone. The lead's reviews and the landing follow.
  - Lead-owned.
- **Date:** 2026-11-19.
- **Decided by:**
  - the product owner: the phase and its scope;
  - the lead: the milestone, its design defaults, the process and the ledger, and the two design questions the research left open (decision 3, "Which Animator"; decision 3, "Out");
  - recorded by the lead.
- **Base:** main `5b878996` (phase 6 complete: M36 to M40 landed in one batch, engine 0.7.0, ADR 0052). Every value this record calls today's was read from the code and data at that commit.
- **Authority:**
  - Roadmap "Rules for every milestone";
  - ADR 0023;
  - ADR 0028 decisions 10, 11, 12, 14, 16 and 17;
  - ADR 0042, whose program this phase continues;
  - ADR 0051, for brief 21's commits straight to main;
  - ADR 0052, whose form and process this phase keeps;
  - ADR 0034 (M23's skeletal animation) and ADR 0049 (M34's camera pose rule);
  - Architecture §5.2, §5.3, §5.7, §9.3, §11.5, §13.7, §15.5 and §18.
- **Amended 2026-11-19 (M42 joins phase 7):**
  - **What changed.** World Tree's brief 23, planar reflections, arrived after this plan, and the lead took it into phase 7 as M42 (`Docs/Decisions/0059-m42-planar-reflections.md`, its plan and record). Phase 7 is now brief 21's three fixes, M41 and M42, and the following supersede decision 2's "alone" and decision 7's "the phase's last (and only) lander".
  - **Merge train:** M41, then M42.
  - **The landing:**
    - One batch by default: `landing/phase7` merges current main, M41 and M42 with their adaptations, then runs one merge review and one `CI.py --milestone` (decision 7's hardened gate, announced to the owner first). Main fast-forwards to it.
    - M41 lands alone ahead of the batch, gated by `CI.py --require-clang-cl`, only if M42 runs far behind (W42.3 not begun when M41's milestone review is done). M42 then takes the hardened gate.
    - The batch's lander takes the duties of decision 6: `EngineVersionString` 0.8.0, FeatureTest regenerated and the editor-UI goldens checked, once for the phase.
  - **Contended files.** M41 owns every file the two share, and M42 appends after M41's entries at the landing:
    - `BuiltinComponents.h`;
    - `ProjectValidator.cpp`'s tables and `ProjectValidatorTests.cpp`'s count line;
    - `MakeValidationFixtures.py`, `Tests/Data/Validation/README.md` and the `validation-codes` page;
    - FeatureTest's `Scaffolds.json` and `Suites.jsonl`;
    - `BuiltinComponentsTests.cpp`;
    - `RenderExtraction.{h,cpp}` and `RenderExtractionTests.cpp`: M41's region is `ComputeRenderedWorldMatrix`, M42's the item in the canonical loop.

    The generated references are regenerated on the landing, never merged.
  - **The ledger.** M42's allocations are ADR 0059 decision 8's: IdSeeds 3900 to 3999; validation code 75, `RENDER_REFLECTION_INVALID`, after M41's; component 64, `PlanarReflection`, in Probes. A record outside the two milestones takes 0060 on.

## Context

- **The briefs.** World Tree's look pass (2026-11-18, engine 0.7.0) reported two briefs, both of low priority, in `Projects/WorldTree/Design/EngineTasks/`:
  - **Brief 21:** a large bake's allocation count, material edits that skip a running session, and a rebuild on every open. Three small issues, each seen once.
  - **Brief 22:** an entity that follows a bone of a skinned mesh, so the Warden's staff can be its own mesh, folded or hidden in the dodge roll.
- **The research.** One read-only report of the lead's (kept outside the repository) read brief 22 and the code at `5b878996`. This record draws M41's design defaults and ledger from it. Where the report and this record differ, this record is the rule.
- **What the engine has today** (each fact checked in the code at `5b878996`):
  - **A script read of a bone's pose exists.** M23 shipped `Animator:GetJointTransform(joint) -> (vector?, Quat?)`:
    - binding at `Scripting/Bindings/Animation.cpp`, built on `FindAnimatorJointWorldTransform` (`Scene/AnimationSystem.h`);
    - it returns the Animator entity's world matrix times the joint's model transform from the last AnimationSystem phase;
    - the luau-gameplay skill already says "Attach a sword or a muzzle flash with it".

    **Brief 22's second option is therefore met.** What is missing is an attachment the engine places itself, which a script cannot do well (below).
  - **The pose belongs to the Animator, not to the SkinnedMesh.**
    - The runtime-only `AnimationPoseComponent` sits on the Animator's entity: never registered, serialized or hashed. It holds `JointModel[j]` in the Animator entity's space and `Skin[j]`.
    - A SkinnedMesh finds its Animator through `FindDrivingAnimator`: its own entity, then its nearest ancestor.
    - Joint names belong to the Animator's Skeleton. The glTF importer keeps a node name verbatim (`DEF-hand.R` stays as written) and makes a duplicate unique with `" <index>"`.
  - **A model's bones are not entities.**
    - Joint nodes become Skeleton joints.
    - A mesh on a joint node is skipped (`ASSET_CONTENT_SKIPPED`).
    - A non-joint node below a joint is placed under the Animator's entity at its rest transform, so it never moves with the joint (`GltfImporter.h`; `Docs/Backlog.md` §6).
    - Entities added under a model instance are user children: updates preserve them, and `prefab.apply` adds them (Architecture §5.5).
  - **The pose is a fixed-step quantity.**
    - It is sampled once per tick in the AnimationSystem phase (4c), after FixedUpdate, Tasks, NativeStep and SpriteAnimation and before PreStepTransformUpdate.
    - The phase runs in Play and Simulate, and not under the world pause.
    - `AnimationSystem::Step` has two passes: stepping every enabled Animator in canonical order, then dispatching `OnAnimationEvent`. It returns at once in a scene with no Animator and no SkinnedMesh.
    - The pose is a pure function of the Animator's fields, the assets and the tick, and the Animation module is on the simulation path (`SimulationPathModules`).
    - Root motion Apply already writes a Transform from the pose in the fixed step, and FeatureTest's `Animation.replay` verifies it at one hash in Debug, Release and Dist.
    - Edit scenes have no pose, and a skinned mesh draws its bind pose there. The one exception is `animation.preview`, which poses one Animator without an undo step.
  - **The skin draws at its Animator's rendered pose.**
    - The palette is `inverse(item.World) * ComputeRenderedWorldMatrix(animator, alpha) * Skin[j]` (`Scene/RenderExtraction.cpp`, `ExtractSkinnedItem`).
    - The Animator entity interpolates between ticks, and the joints within it show the current tick's pose.
    - `ComputeRenderedWorldMatrix` and `TransformSystem::GetRenderPosition` and `GetRenderRotation` (the scripts' `Transform.RenderPosition` and `RenderRotation`) implement the one interpolation rule and must agree (Architecture §5.2).
  - **Why a script cannot place a held object well.**
    - From `OnFixedUpdate` or a task, it reads the previous tick's pose (both run before 4c): one tick late, in the hashed state and the image alike.
    - From `OnUpdate` or `OnLateUpdate`, the write renders uninterpolated (`InterpolationResetTag`) while the hand renders at the Animator's interpolated pose: a judder of up to one tick of the body's motion, and it depends on the frame rate in live play.
    - Even an exact fixed-step placement that interpolates like any entity draws at lerp(A_prev × J_prev, A_cur × J_cur) while the hand draws at lerp(A_prev, A_cur) × J_cur. That leaves about (1 − alpha) times one tick of the bone's own motion, up to about 15 cm in a sword swing at 60 Hz, and every lockstep screenshot (alpha 1) hides it.
  - **The state hash** covers every serialized component through the scene's canonical serialization, every Transform included. M23 added no block.
- **Corrections to the brief** (each to go to the World Tree session through the product owner at the phase's end):
  - `Animator:GetJointTransform` already reads a bone's world pose (M23), with the fixed-step rule the brief asks for.
  - The pose is the Animator's, so the brief's `SkinnedMesh: EntityRef` would always be resolved to an Animator. M41 attaches to the nearest Animator above the entity instead (decision 3).
  - The pose is never evaluated in the frame phase. "In the frame phase for rendering" becomes a rendering rule (decision 3, "Rendering"), not a second placement.
  - A staff exported from Blender as a separate object parented to `DEF-hand.R` would import as a static child at its rest pose.

## Decisions

### 1. Scope

Phase 7 covers World Tree's briefs 21 and 22:
- **Brief 21:** three changes straight to main outside the milestone (decision 8).
- **Brief 22:** one milestone, M41 Bone attachments (decisions 2 and 3), in the form decision 3 fixes:
  - a component the engine places, with no script API added;
  - attached to the nearest Animator above its entity;
  - its placement is fixed-step, hashed state;
  - its rendering follows the skin's rule.

What M41 leaves out is listed in decision 3, "Out", and recorded in `Docs/Backlog.md` §2. A limit it accepts on landing goes to §6.

### 2. Structure: one milestone, two waves

| Phase | # | Name | Brief | Lands | Gate | Size (builder-sessions) |
|---|---|---|---|---|---|---|
| 7 | M41 | Bone attachments | 22 | alone, after brief 21's three fixes reach main | `CI.py --milestone` (decision 7) | ~4.5–5 (W41.1 ~3, W41.2 ~1.5–2) |

- **One builder** on `milestone/m41`, in `D:/Engine/.claude/worktrees/m41`, works the waves in order.
  - **W41.1, attachments in the session:** the component, the attaching pass, the runtime state, the rendering rule in both rendering functions, the validation code with its fixture, and the unit cases.
  - **W41.2, coverage, documents and the editor:** FeatureTest's scaffolds, suite and replay in every mode, the Python automation case, the documents and skills, and the generated references.
- **The dependency on brief 21's third item.** M41's FeatureTest scene saves and reopens skinned model instances that hold user children, the scene shape brief 21's third item is about. So `milestone/m41` merges main once that fix has landed and before W41.2 writes its scaffolds. W41.1 needs no FeatureTest scene and starts at once.
- **Numbering.** M41's record is this ADR. A record outside the milestone takes 0059 on.

### 3. Design defaults: M41 Bone attachments

No part of M41 adds any of these: a namespace, a settings section, an `AssetType`, a built-in handle, a `PlaySessionPhase`, a fuzz target, a stats row, a render pass, a pipeline, a module, a script function or method, a callback, an automation method, a state-hash block, a format version or a vendored library. W41.1's first checkpoint settles what a contract would freeze, M34's way: the component, the placement rule and the validation code.

- **A component, not an API.** `BoneAttachment`, version 1, category Animation:
  - appended after `SkinnedMesh` in `Scene/Registration/AnimationRegistration.cpp` (`RegisterAnimationComponents`), and in `BuiltinComponents.h`'s include and type list after `SkinnedMeshComponent`;
  - requires Transform; script-visible through its generated proxy.

  The script read exists already, and `Animator:GetJointTransform` stays the fixed-step read of a bone's pose. The component is the one thing a script cannot do: place an entity once per tick, at the one point where the tick's pose exists, and draw it with the skin's rule. It gives the game a mesh of its own that it can hide, swap, fold or give its own material, light or effect.
- **Fields.** All three follow the collider fields' types.
  - `Bone`: string, empty by default, at most `MaxSkeletonJointNameBytes` (256) bytes of valid UTF-8. It names a joint of the Animator's Skeleton, matched exactly as `Animator:GetJointTransform` matches it.
  - `Offset`: vec3, default 0, in metres, finite. It is expressed in the bone's axes, in the Animator entity's units.
  - `Rotation`: quat, default identity, unit. It is relative to the bone.

  The fields are named after the brief, not after the engine's "joint", which since M37 also names the physics joints of the neighbouring Joints category. Every description says "a joint of the Animator's Skeleton (a bone)".
- **Which Animator: the nearest Animator above the entity, with no target field** (the lead's decision; it departs from the brief's `SkinnedMesh: EntityRef`).
  - **The rule** is `FindDrivingAnimator` started at the entity's parent: the parent, or its nearest ancestor that holds an Animator. It is the SkinnedMesh rule one level up, so an entity that holds an Animator of its own (a rider) attaches to the Animator above it.
  - **Why.** The placement lives in the Animator's own space, which makes it:
    - exact whatever moves the character later in the same tick (a CharacterController's update, a body's PostStep, root motion Apply), because the hierarchy carries it;
    - unable to dangle or form a cycle, because the Animator is always an ancestor;
    - free of any order across trees.

    An explicit target outside the entity's ancestors would place in world space. It would lag whenever the target's tree moves later in the tick, and it would need cycle, destroyed-target and prefab-remapping rules.
  - **For the game.** The Warden's staff becomes a user child of the Warden model instance's Animator entity, with `Bone` `DEF-hand.R`. A script that hands a weapon to another character reparents it (`Entity:SetParent(armature, false)`) and sets `Bone`. An explicit target stays a §2 row.
- **Where it runs: a third pass of the AnimationSystem phase (4c), "Attaching".**
  - **Order.** The pass runs after every Animator was stepped and posed (root motion Apply included) and before the `OnAnimationEvent` dispatch. So event callbacks, PreStepTransformUpdate, PhysicsPreStep's sweep of an attached Kinematic body, the contact callbacks and the next tick's scripts all see the attachment at the tick's pose.
  - **When.** It runs in Play and Simulate, and not under the world pause, where the attachment holds with the pose. No `PlaySessionPhase` is added.
  - **Code.** The work lives in new files, `Scene/BoneAttachments.{h,cpp}`, called from `AnimationSystem::Step`.
    - The early return also checks the BoneAttachment view.
    - `AnimationSystem.h` (frozen by M23's C23.3) changes only in its comment's list of passes, a reviewed change to a frozen interface.
  - **Edit scenes and `animation.preview`:** nothing is placed. An attached entity shows at its authored Transform.
- **The placement.**
  - **Order.** Attached entities are visited in canonical order, parents first, so an attachment below another attachment sees its parent's new pose.
  - **Pose.** Each effectively enabled entity whose Animator has a pose and whose Skeleton has the joint takes:
    - position P = position(JointModel[j]) + rotation(JointModel[j]) · Offset;
    - rotation Q = rotation(JointModel[j]) · Rotation.
  - **Into the parent's space.** P and Q are taken into the parent's space through the chain of local matrices from the Animator's entity down to the parent, never through cached world matrices. The result is exact when the attached entity is a direct child of the Animator's entity, the documented pattern.
  - **What is patched.** The pass patches only the entity's `Translation` and `Rotation`, and only when either differs bit for bit, so a held pose causes no revision churn. `Scale` stays as authored.
  - **The bone's scale is ignored.** A held object keeps its own scale. A sheared or zero joint scale cannot become a TRS, and a joint whose matrix does not decompose (`TransformSystem::DecomposeMatrix`, a scale below `MinTransformScaleMagnitude` on the way) leaves the Transform as it is that tick.
  - **Determinism.** The arithmetic is IEEE-exact plus DetMath (the simulation-path rule).
  - **Runtime state.** The pass keeps a runtime-only `BoneAttachmentStateComponent {UUID Animator; uint32_t Joint}` on each entity it placed this tick, under `RuntimeComponents.h`'s rules: never registered, serialized or hashed, and invisible to scripts and automation. It removes the component from an entity it did not place. The joint index is cached per Skeleton and name.
- **Rendering: the skin's rule** (decision R).
  - **The rule.** An entity at or below an attached entity X renders at `ComputeRenderedWorldMatrix(A) × (its current pose relative to A)` when:
    - X holds `BoneAttachmentStateComponent`, and
    - X's recorded Animator entity A is still an ancestor of X.

    The relative pose equals `inverse(World(A)) × World(entity)`. The builder records how it is computed.
  - **What it gives.** That is exactly the skinned palette's rule: the subtree takes A's interpolation and the tick's bone pose, whatever X's own reset tags. A is itself rendered by the same rule, so a rider on a mount works.
  - **Both functions follow it.** `ComputeRenderedWorldMatrix` (`RenderExtraction.cpp`) and `TransformSystem::GetRenderPosition` and `GetRenderRotation` share one helper, found within the ancestor walk `FindInterpolationStart` already makes, and a unit case compares them.
  - **No cost without attachments.** The helper looks for an attached ancestor only while the scene holds any `BoneAttachmentStateComponent` (one view emptiness check), so a scene without attachments takes exactly today's path. Every existing image, golden and extraction stays byte-identical.
  - **Rejected alternative:** interpolating attached entities like any other, which leaves the staff sliding in the fist between ticks.
- **Determinism: the bone pose is fixed-step, and the placement is hashed state** (the rule the brief asks to be settled the way M34 settled the camera).
  - **What is hashed.** The attached entity's Transform is written in the fixed step from a pose that depends only on the Animator's fields, the assets and the tick, so it is in the canonical serialization and the hash, as root motion Apply's writes are.
  - **Why not keep it out of the hash.** That would need a second, runtime-only transform that every gameplay reader honours: `WorldPosition`, bodies, triggers and queries. Only the drawing between ticks is presentation (decision R), as the skin's is.
  - **No hash moves.** No hash block is added and no committed hash moves: no existing scene holds the component, and a scene without one takes no new code path.
  - **Timing.** A write in the fixed step (`OnFixedUpdate`, a task, a test case) is in force at that tick's AnimationSystem phase. A write in the frame phase, a physics callback or an `OnAnimationEvent` callback is in force at the next tick's.
  - **`Animator:GetJointTransform` is unchanged.** It reads the Animator entity's cached world matrix. Between ticks (after the tick's transform update), it and an attached entity's `WorldPosition` agree, which a W41.1 case checks.
- **Reach.** Each rule is documented and has a unit case.
  - **Not attached.** A missing bone, an empty `Bone`, a Skeleton that is null or does not load, a non-finite pose, or no Animator above the entity:
    - The entity is not attached. It keeps its Transform and renders by plain interpolation.
    - The AnimationSystem's diagnostics raise `ANIMATION_ATTACHMENT_INVALID` once per entity. For a missing joint, the message names up to three close joint names (`Reflection/FuzzySuggest.h`).
  - **An Animator without a SkinnedMesh** (the brief's "non-skinned target"): an attachment follows the pose of any Animator with a Skeleton, an invisible rig included.
  - **The Animator destroyed, disabled or gone.**
    - Destroying or disabling the Animator's entity destroys or disables the attachment with it, since the attachment is a descendant.
    - Removing the Animator component, or reparenting the attached entity away from it, detaches it: a diagnostic, and the Transform is kept.
    - Under another Animator, the entity attaches to that Skeleton's joint of the same name.
  - **The attached entity disabled.** A disabled attached entity is skipped and placed again at the first AnimationSystem phase after it is enabled.
  - **`Animator.Playing` false** keeps the attachment on the scrubbed pose.
  - **An attachment to an attachment** is handled by canonical order: a rider with its own Animator attaches to its mount's bone, and the rider's weapon to the rider's own bone.
  - **Scale.** The Animator entity's world scale, non-uniform included, reaches the attachment through the hierarchy, as it reaches the deformed mesh. The bone's scale is ignored: a clip that scales a weapon's bone to 0 does not hide an attached entity, and the game sets `Active` to false. A non-uniformly scaled entity between the Animator and the attached entity gives `DecomposeMatrix`'s closest rotation (lossy, documented).
  - **Physics.**
    - An attached entity with solid colliders should hold its own Kinematic RigidBody, which PreStep sweeps with velocities, so a swinging staff pushes debris. A trigger can stay an implicit sensor.
    - A solid collider without a body (an implicit Static body teleported every tick), a collider gathered into an ancestor's RigidBody (a compound rebuilt every tick), a Dynamic RigidBody or a CharacterController on the attached entity each fight the attachment and are reported by `ANIMATION_ATTACHMENT_INVALID`.
    - M37's joints work from an attached Kinematic handle, such as a flail or a hanging lantern.
  - **Overwritten writes.** The attached entity's own `Translation` and `Rotation` writes are overwritten at the next AnimationSystem phase: a script's, automation's, `Transform:Teleport` and its own Animator's root motion. Games write `Offset` and `Rotation` instead.
  - **`entity.get`** reports the component's three fields, and with `target: "play"` the Transform the attachment wrote. No field is added.
  - **The edit view.** A model with no animation playing draws its bind pose, and the attached entity sits at its authored Transform. In Play, an Animator with no clip still poses its rest pose, which the attachment follows.
- **Validation: `ANIMATION_ATTACHMENT_INVALID`** (Warning, not auto-fixable).
  - **Causes:**
    - no Animator above the entity;
    - an empty `Bone`;
    - the Animator's Skeleton loads and has no joint named `Bone` (with suggestions);
    - the entity holds a CharacterController or a Dynamic RigidBody;
    - its solid colliders form no Kinematic body of their own.
  - **Reporting.** `project.validate` reports it through `FindBoneAttachmentIssues(scene, assets)` in `Scene/BoneAttachments.h`, and the phase reports it at run time.
  - **Where the code lives:**
    - in an array of its own in that header (not `AnimationValidation.h`, which M23's C23.3 froze);
    - appended at the end of both of `ProjectValidator.cpp`'s tables after M37's `PHYSICS_JOINT_INVALID`, with `ProjectValidatorTests.cpp`'s count line growing by one;
    - with its fixture `Tests/Data/Validation/ANIMATION_ATTACHMENT_INVALID/` from `MakeValidationFixtures.py` (a rig glTF, as `ANIMATION_SKELETON_MISMATCH`'s fixture has), the game-debugging skill's `validation-codes` row and §13.7's row.
- **Scripts, automation and the editor.**
  - **No script API is added.** The proxy reads and writes `Bone`, `Offset` and `Rotation`, generated (gate 4).
  - **No automation method is added** (§13.5 stays 126, and the MCP tools stay 86). The component goes through `entity.create`, `entity.update` (in play too: a `Bone` write takes effect at the next tick), `edit.batch`, `component.schema` and `prefab.*`. One Python case in `Tests/Automation/test_animation.py` creates an attachment, switches its `Bone` in play and reads the placed Transform with `entity.get` (rule 5).
  - **The editor** uses the generic inspector: `Bone` is a text field, `Offset` and `Rotation` edit as on colliders, and the component appears in Add Component's Animation group. There is no bone picker, which would need a joint list that automation could also read; the validator's suggestions serve agents and people.
- **FeatureTest** (IdSeeds 3800 to 3899; every gate in each mode the feature supports).
  - **`BoneAttachments.jsonl`** (IdSeed 3801) makes `BoneAttachments.scene` and its suite from the imported `Assets/Models/Animation/Skinned.gltf` (joints `Joint0` and `Joint1`, clip `Wave`) and `SkinnedRigs.gltf` (`Hips`, `Spine`, `Tail`, clip `Walk` with root motion Apply). The scene holds:
    - a "Sword" user child of the waving model's Animator entity on `Joint1`, with a non-default `Offset` and `Rotation`, a Kinematic RigidBody and a trigger;
    - a "Gem" PointLight child of the Sword;
    - a second attachment below the Sword, on `Joint0`;
    - a "Pack" on the walker's `Spine`;
    - a "Rider" instance of `Skinned.gltf` attached to the walker's `Hips`.
  - **Its suite** checks:
    - the attached world position within 1 mm of `GetJointTransform` plus the offset at several clip times;
    - a bone switch (the fold) from a task taking effect in the same tick;
    - `Offset` and `Rotation` writes;
    - disable and enable;
    - `RemoveComponent` keeping the last Transform, and `SetParent` away detaching;
    - the world pause holding;
    - the Sword's trigger firing where the bone is.
  - **`BoneAttachmentsReplay.jsonl`** (IdSeed 3802) records `Assets/Tests/Replays/BoneAttachments.replay`: input starts the walk and folds the sword onto another bone. The determinism suite verifies it in Debug, Release and Dist, and it fails without its input.
  - **Gates:**
    - 1: `BoneAttachment` with all three fields non-default;
    - 4: the three fields through the proxy;
    - 6: `ANIMATION_ATTACHMENT_INVALID`'s fixture;
    - 7: every description;
    - 1b, 2, 3, 5, 8 and 9: nothing new.
  - **Goldens.** No new golden scene: goldens render at alpha 1, where decision R changes nothing, so its unit cases are its guard.
  - **The soak** gains one subject.
- **Run modes:** `RunModes::All` (editor Play and Simulate, Release and Dist). Nothing is placed in edit scenes or during `animation.preview`.
- **Documents:**
  - **This record**, amended per wave.
  - **Architecture:**
    - §5.2: the rendering rule for attached subtrees;
    - §5.3: the `BoneAttachmentComponent` row after SkinnedMesh, and the runtime state component;
    - §5.7: step 4c's attaching pass before the dispatch;
    - §9.3: an attached Kinematic body is swept;
    - §11.5: the `Animator` row points to BoneAttachment;
    - §13.7: the validation code's row;
    - §15.5: the scene's row.
  - **`Docs/Backlog.md`:**
    - §6's M23 row that says "scripts attach objects to joints through `Animator:GetJointTransform`" names BoneAttachment;
    - the "Out" rows in §2;
    - the limits accepted at landing in §6.
  - **The header comments** of `AnimationSystem.h` (the passes), `AnimationPoseComponent.h` (its readers), `RenderExtraction.h` and `TransformSystem.h` (the rule), and `PlaySession.h` (phase 4c and "Render interpolation").
  - **The skills:**
    - luau-gameplay's `animation` page: line 49's "Attach a sword … with it" gives way to the BoneAttachment pattern and a `DocExamples`-checked fold example. `SKILL.md` gains at most one index line, since it is 46,128 of its 49,152 bytes.
    - game-building's `prefabs-and-levels`: a held weapon on a model instance.
    - game-debugging's `validation-codes`: the code's row.
  - **The generated references:** `Docs/Reference/*`, `Engine.d.luau` (the proxy type) and `catalog.json`.
- **Out**, as `Docs/Backlog.md` §2 rows:
  - an explicit target field (an attachment to an Animator outside its entity's ancestors);
  - following the bone's scale, and position-only or rotation-only attachments;
  - placement in edit scenes and during `animation.preview`;
  - a bone picker and a joint-name list (`Animator:GetJointNames`);
  - a rendered-pose joint read (an attached entity's `RenderPosition` serves);
  - importing a glTF mesh parented to a joint as an attached entity. That would be a `GltfImporter` version 4, which re-imports every model and changes existing instances: the lead's decision, since the brief's staff is authored as its own entity;
  - attachments in native modules (`NativeApiVersion` stays 1) and in 2D.
- **Accepted at landing**, as §6 rows:
  - An attached subtree draws rigidly at its Animator's rendered pose, so a descendant's own fixed-step motion relative to it shows at the tick rate, as the skin's pose does.
  - Under a non-uniformly scaled intermediate entity, the placement takes the closest rotation.
  - The attached entity's own Transform writes are overwritten.
  - An attachment spawned or reparented after phase 4c is placed from the next tick.

### 4. Approvals

None. Nothing is downloaded or vendored, and the fixtures come from FeatureTest's imported models and the committed generators.

### 5. Determinism, platforms and goldens

- **Determinism.** The attachment is component data and fixed-step Transform writes (decision 3). No hash block is added and no committed hash moves, which `Scaffold.py --check`, the determinism suite and the games suite confirm. `BoneAttachments.replay` verifies in Debug, Release and Dist. A W41.1 unit case steps a clip 600 ticks and ends in a committed constant hash of the attached Transform, in the Debug and Release unit runs.
- **Platforms.** Only Windows is verified (ADR 0028 decision 10), and M41 adds no platform capability.
- **Goldens.** No golden changes and no golden scene is added. The device class stays `nvidia-62x`. No editor-UI capture opens Add Component (ADR 0052's landing note), and the lander checks this on the final tree.

### 6. Process

Phase 6's process, kept as it is (ADR 0052 decision 6, ADR 0051):
- **No contract.** One builder works the waves in order. The lead runs one independent review pass of each wave, verifies its blocker and major findings, and has every confirmed finding fixed, nits included.
- **Frozen headers.** M23's frozen animation headers (`AnimationSystem.h`, `AnimationValidation.h`) are changed only as decision 3 names, as reviewed changes to a frozen interface.
- **GPU discipline.** As phase 6's builders: filtered gpu, golden and automation runs only, every run through `Test.py` under the machine locks.
- **Checkpoint commits** on `milestone/m41` run only the tests and suites their change touches. Agents never run `PreCommit.py` or `CI.py`.
- **The milestone end**, in this order:
  1. `milestone/m41` merges current main, which by then holds brief 21's fixes.
  2. One independent review pass of the milestone diff, with every finding fixed.
  3. The lander's duties: `EngineVersionString` 0.8.0, FeatureTest regenerated by `Scaffold.py` on the final tree, and the editor-UI goldens checked.
  4. One gate (decision 7). Main fast-forwards to `milestone/m41` only after it is green.
  5. Main is pushed to `origin`, and the work pauses for the product owner's go-ahead (ADR 0028 decision 14).
- **World Tree's needs.** As ADR 0052 decision 6. A new brief that arrives during the phase is triaged at its end, unless it lands straight on main under ADR 0051 without disturbing M41.

### 7. The gate

- **The rule.** M41 is the phase's last (and only) lander, and it changes core systems: Scene's render-interpolation rule and the animation phase. By ADR 0028 decision 12 its gate is `CI.py --milestone`. That run also carries the hardening phase 6's gate left to "the next hardened gate" (`Docs/Backlog.md` §3): the ASan unit and feature runs, the debug heap, the long fuzz and a full soak.
- **Before it runs.** The lead announces it to the product owner first: about 7 h or more on a quiet machine, with the World Tree session's rendering editor closed, and the fuzz needs 14 logical CPUs and as many GiB of available memory.
- **If the owner waives the hardening,** the gate is `CI.py --require-clang-cl` (about 2.5 h), and §3's hardening row stays open.

### 8. Brief 21: three changes straight to main

Each item is a change straight to main outside the milestone, with ADR 0051 decision 1's targeted checks and the lead's review. Each gets a builder in its own worktree, and the builder fixes a defect where it arises, with a regression test that failed before the fix, or documents intended behaviour where an agent will find it, with a test of the behaviour as documented. The outcomes are recorded below as each lands; M41's gate runs on a tree that holds all three.

| Item | The brief | Worktree and branch | Outcome |
|---|---|---|---|
| 1 | A 507-probe `IrradianceVolume` bake logged the allocation warning at 2,009 and then 23,521 native allocations; whether the count falls back after the bake is not visible | `b21a`, `fix/brief21-b21a` | pending |
| 2 | `asset.setProperties` on a Material while a session plays is not seen until play restarts (`BaseColor` and `BaseColorMap`) | `b21b`, `fix/brief21-b21b` | pending |
| 3 | A scene holding an instance of `AnimationLibrary_Godot_Standard.glb` reports `rebuiltInstances` on every open and opens dirty, though a save writes the file byte for byte | `b21c`, `fix/brief21-b21c` | **A defect, fixed** (`f4dd2dae`): a scene file holds a fractional number as its float's shortest text (`0.8`), which a load keeps verbatim in an override's `Value`, while the diff recorded the double the float widens to (`0.800000011920929`), so every refresh after a load rewrote the record, counted the instance rebuilt and left the scene dirty (any fractional override, here the Warden's Animator layer). The diff now records each value as a file holds it (`JsonWriter::ToWrittenForm`); saved bytes and hashes are unchanged. Regression cases in `PrefabInstantiatorTests.cpp` and `SceneMethodsTests.cpp` (a skinned model at `/Set/Warden` with fractional overrides opens clean), with `test_prefab.py`'s case |

### 9. Reservation ledger

Today's values are read at `5b878996`:

| Allocation | Today | M41 |
|---|---|---|
| ADR numbers | the last is 0057 (M40) | 0058, this record; a record outside the milestone takes 0059 on |
| FeatureTest IdSeeds | 97 scaffolds in `Scaffolds.json`, the highest seed 3702 (`MusicReplay.jsonl`); 3800 to 3899 free | `BoneAttachments.jsonl` 3801 and `BoneAttachmentsReplay.jsonl` 3802, listed after main's scaffolds and before `Suites.jsonl` (1515) and `Replays.jsonl` (1516) |
| Suites, replays and scenes | 114 `Testing.Suites`, 25 committed replays, 101 FeatureTest scenes | one more of each: the `BoneAttachments` suite, `BoneAttachments.replay` (format 5) and `BoneAttachments.scene` |
| BuiltinComponents | 16 categories, 62 components, `RegisterJointsComponents` the last function | `BoneAttachment` in Animation after `SkinnedMesh`: 63 components, no new category or registration function; `MakeValidationFixtures.py`'s `COMPONENT_ORDER` gains it after `SkinnedMesh` (a mid-list insert, as M36's `ColliderSurface` was) |
| Validation codes | 73, the last M37's `PHYSICS_JOINT_INVALID` | 74: `ANIMATION_ATTACHMENT_INVALID` (Warning), appended to both tables, with its own array in `Scene/BoneAttachments.h` |
| Script API | 22 namespaces, 28 behaviour callbacks; `IScriptHost` ends with `GetMusic`, `RegisterBindings.cpp` with `RegisterJoints` | none: no namespace, function, method, callback or host member; `Engine.d.luau` gains the generated `BoneAttachment` proxy type |
| Automation methods and MCP tools | 126 methods, 86 proxied tools | unchanged |
| State-hash blocks | scene, tick, Random, id draws, physics (the joint part last), native, physics2d, ui, save data, world pause, locale | none: the component and the attached Transform are in the canonical serialization; `BoneAttachmentStateComponent` is runtime-only |
| Formats | entity document 1, replay 5, graph 2, `GltfImporter` 3, pak 2, `NativeApiVersion` 1, protocol {1,0} | unchanged; the new component is version 1 |
| Fuzz targets | 53 | unchanged (no reader) |
| `PlaySessionPhase`, render passes, settings sections, `AssetType`, built-in handles, stats rows | as ADR 0052's ledger left them | none |
| Goldens | 55 on `nvidia-62x` | unchanged |
| `EngineVersionString` | 0.7.0 | 0.8.0 at M41's landing, with FeatureTest regenerated |

**Files.**
- **M41's own, new:**
  - `Scene/Components/BoneAttachmentComponent.h` and `Scene/BoneAttachments.{h,cpp}`;
  - `Tests/Source/Engine/Scene/BoneAttachmentsTests.cpp`;
  - the two scaffolds and the validation fixture.
- **M41's changes to existing files:**
  - `AnimationRegistration.cpp`;
  - `AnimationSystem.{h,cpp}` (the header's comment only), `AnimationPoseComponent.h` (comment);
  - `RenderExtraction.{h,cpp}`, `TransformSystem.{h,cpp}`;
  - `PlaySession.h` (comments);
  - their tests.
- **Shared and append-only:** `BuiltinComponents.h`, `ProjectValidator.cpp` and its tests, `MakeValidationFixtures.py`, the generated references, FeatureTest's `Scaffolds.json` and `Suites.jsonl`, Architecture's lists, the Roadmap, the Backlog and the skills.
- **Brief 21.** Its fixes touch the lighting bake and probe capture, the material and asset reload path, and the prefab instance refresh on open, none of M41's files but the Backlog and Architecture, which merge by hand.

## Acceptance

The Roadmap's M41 acceptance, each a named test or check:
- "BoneAttachments: an attached entity follows its bone across a clip", within 1e-5 m of `FindAnimatorJointWorldTransform` times the offset, with root motion Apply and under a moving CharacterController parent.
- "BoneAttachments: an attachment below another attachment follows its parent's new pose".
- "BoneAttachments: an entity without a valid bone keeps its Transform and is reported", with `ANIMATION_ATTACHMENT_INVALID` and its suggestions.
- "BoneAttachments: a held pose changes no revision".
- "BoneAttachments: the world pause holds an attachment, and Simulate places it".
- "BoneAttachments: an animation event callback reads the placed pose".
- "BoneAttachments: an attached Kinematic body is swept with its bone".
- "BoneAttachments: a clip stepped 600 ticks ends at its committed hash", in Debug and Release.
- "RenderExtraction: an attached subtree renders at its Animator's rendered pose", where the item and `Transform.RenderPosition` equal the palette's bone at alpha 0.5.
- "RenderExtraction: a scene without attachments extracts as before".
- `test_animation.py`'s attachment case.
- The `BoneAttachments` suite passes in the editor, Release and Dist, and `BoneAttachments.replay` verifies at one hash in Debug, Release and Dist.

**Brief 22's "How the game will check it":**
- **The staff stays in the fist within a millimetre in Sword_Attack and Roll.** The suite's 1 mm check against `GetJointTransform` at several clip times, and the first case above.
- **The Warden folds or hides the staff during the dash, and no staff shows below the floor.** The suite's same-tick bone switch, and `Active` false for hiding. The screenshot of the roll's middle is the game's own check.
- **Every replay still verifies.** The attachment is hashed fixed-step state: `BoneAttachments.replay` verifies in every configuration, and no committed hash moves.

## Risks

- **Decision R touches the rule every drawer uses.** It acts only below an entity that holds the runtime state component, behind one view check. A unit case pins byte-identical extraction for scenes without attachments, and another compares the two rendering functions, which must agree (§5.2).
- **Attached state is hashed.** A nondeterministic pose would now fail replays where today it would only draw differently. Root motion Apply's replay already proves the pose path in all three configurations; W41.1 adds the committed constant-hash case and W41.2 the replay.
- **Per-tick writes are cheap only with the right bodies.** An attached collider gathered into an ancestor's compound rebuilds it every tick, and a solid collider without a body is a static body teleported every tick. The validation code reports both, and the skills give the pattern (a Kinematic RigidBody of its own).
- **The model-instance dependency.** M41's FeatureTest scene relies on user children under skinned model instances surviving a reopen, brief 21's third item. `milestone/m41` merges main after that fix lands and before W41.2's scaffolds (decision 2). W41.2 adds a case that an attached user child survives `scene.open`.
- **The game meets parts of the brief itself.** The fold or hide and the roll's screenshot are World Tree's. The staff must sit under the Warden model's Animator entity, not name a target. Both go to the World Tree session through the product owner at the phase's end.
- **The first hardening since M27 ran in full.** It covers phases 5 to 7 at once, so a finding in earlier code lengthens M41's landing. The ASan unit run is the step nearest its bound: 5,506 cases fit its 3,600 s at a free machine's 0.5 s a case (§3's row). It runs on a quiet machine, announced beforehand (decision 7).
- **Two sessions in one checkout.** The World Tree session works in `D:\Engine`. The phase works in worktrees and never touches `Projects/WorldTree` beyond reading its briefs.

## Before milestone completion

- [ ] Brief 21's three items landed on main, each reviewed and its outcome recorded in decision 8.
- [ ] W41.1 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] W41.2 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] The milestone end: `milestone/m41` merged current main, the milestone review's findings fixed, `EngineVersionString` 0.8.0, FeatureTest regenerated and the editor-UI goldens checked on the final tree.
- [ ] The gate green (decision 7), main fast-forwarded and pushed.
- [ ] Brief 22's and brief 21's "How the game will check it" met, and reported to the World Tree session through the product owner with the corrections above.
- [ ] Architecture, the Roadmap, the skills, the Backlog and the generated references describe what landed.
