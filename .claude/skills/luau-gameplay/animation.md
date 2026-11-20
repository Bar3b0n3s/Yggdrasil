# Skeletal and morph animation in scripts

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): the script surface of the `Animator`, `SkinnedMesh` and `BoneAttachment` components, blending, root motion, animation events and held objects. The signatures are in `reference/ScriptAPI` (sections `Animator`, `SkinnedMesh`, `BoneAttachment`, `Behaviour` and `Test`).

**Fields** are reached through the component shortcuts, which can be nil: `self.Entity.Animator` (`Skeleton`, `Layers`, `Playing`, `RootMotion`) and `self.Entity.SkinnedMesh` (`MorphWeights`). `Layers` is an array of at most eight `{Clip, Time, Speed, Weight, Loop}`: `Time` in seconds, `Speed` from 0 (held) to 100, `Weight` from 0 to 1. A glTF import gives the skin's entity an Animator with one layer playing the first clip, and each skinned or morphed mesh a `SkinnedMesh`.

**Blending is script-driven:** there is no state machine. Layers blend in order over the skeleton's rest pose, each by its `Weight`, and a layer changes only the joints and morph targets its clip keys, so an upper-body clip on layer 2 leaves the walk of layer 1 on the legs. A crossfade is a script moving two weights in the fixed step:

```luau path=Assets/Scripts/Crossfade.luau
--!strict
export type Crossfade = ScriptInstance & {
	Run: AssetRef?,
	FadeSeconds: number,
}

local Crossfade = {}

Crossfade.Fields = {
	Run = Field.Asset("AnimationClip"),
	FadeSeconds = Field.Number(0.25, { Min = 0.01 }),
}

function Crossfade.OnStart(self: Crossfade)
	local animator = self.Entity.Animator
	if animator and #animator.Layers == 1 then
		-- The next index appends a layer (Speed 1, Weight 1, Loop); start it silent.
		animator:Play(2, self.Run)
		animator:SetWeight(2, 0)
	end
end

function Crossfade.OnFixedUpdate(self: Crossfade, dt: number)
	local animator = self.Entity.Animator
	if not animator or #animator.Layers < 2 then
		return
	end
	local target = if Input.IsActionDown("Run") then 1 else 0
	local weight = animator.Layers[2].Weight
	local step = dt / self.FadeSeconds
	animator:SetWeight(2, math.clamp(weight + math.clamp(target - weight, -step, step), 0, 1))
end

return Script.Define("Crossfade", Crossfade)
```

**Methods:**
- `Animator:Play(layer, clip?, time?)` sets the 1-based layer's `Clip` and `Time` (0 when absent) and keeps its `Speed`, `Weight` and `Loop`; `#Layers + 1` appends a layer. A later index, a ninth layer or a negative time raises an error.
- `Animator:SetWeight(layer, weight)` sets an existing layer's weight in [0, 1].
- `Animator:GetJointTransform(joint) -> (vector?, Quat?)` reads the world position and rotation of a skeleton joint (by its glTF node name) in the pose of the last AnimationSystem phase; nil, nil without a pose or such a joint, and a position with a nil rotation when a scale of 0 on the way (a clip scaling a joint to nothing) leaves the joint no rotation, so check the rotation before you use it. It is the fixed-step read: to make an entity follow a joint, give it a `BoneAttachment` (below), which a script placing it from this read could only do a tick late or with judder.
- `Animator:GetRootMotion() -> (vector, Quat)` reads the last step's root motion in the entity's parent space.
- `SkinnedMesh:SetMorphWeight(target, weight)` sets the weight of the morph target named `target` (a blink, a smile) in [-1, 1], growing `MorphWeights` with the targets' defaults; a name the mesh lacks raises NotFound. A clip's morph channel overrides the weights it keys.

**When it runs:** every Animator steps in the AnimationSystem phase of the fixed step, after `OnFixedUpdate`, tasks and native systems and before the transform update, in Play and Simulate. A clip played in `OnFixedUpdate` is sampled at that tick; one played in a contact callback or `OnDestroy` advances from the next tick. While `Playing` is false the times stay but the pose follows them, so a script scrubs by writing `Time`. A looping layer passes at most eight ends of its clip in one step.

**Holding things: `BoneAttachment`** (M41). A sword, a staff, a torch, a shield or a muzzle flash that must follow a hand is an entity of its own with a `BoneAttachment` (`Bone`, `Offset`, `Rotation`). The engine places it each fixed step on that joint of the nearest Animator above it (on its parent or the parent's nearest ancestor that holds one, never its own), after every Animator was posed and before `OnAnimationEvent`, and draws it with the skin between ticks, so it stays in the fist at any frame rate. Its placement is hashed state, so replays verify. The rules:
- **Where it goes.** Make it a child of the model instance's Animator entity (a user child of the armature, which instance updates keep): `entity_create {parent: "/Warden/Armature", name: "Staff", components: {BoneAttachment: {Bone: "DEF-hand.R"}, MeshRenderer: {...}}}`. `Bone` is the glTF node name, written exactly; `Offset` (metres, along the joint's axes) and `Rotation` (relative to the joint) hold it in the hand. Its `Scale` stays as authored and the joint's scale is ignored.
- **What scripts write.** `Bone`, `Offset` and `Rotation`, never the attached entity's Transform, which the next phase overwrites. A write in `OnFixedUpdate`, a task or a test case is in force at that tick's AnimationSystem phase; in `OnUpdate`, `OnLateUpdate`, a physics callback or `OnAnimationEvent`, at the next tick's. Hide it with `staff:SetActive(false)` (a clip that scales the bone to 0 does not hide it); enabled again, it is placed at the next phase. To hand it to another character, reparent it with `staff:SetParent(otherArmature, false)` and set `Bone`.
- **Reading it.** Its `Transform.WorldPosition` is the bone's pose at the last tick, the same as `animator:GetJointTransform(bone)` plus the offset, and `Transform.RenderPosition` is where it is drawn. A child of the attached entity (a trail emitter at the tip, a light in a lantern) rides along.
- **Physics.** A solid collider on it needs a `RigidBody` of Type `Kinematic` of its own, which is swept with the bone and pushes what it meets; a trigger can stay without one. A collider gathered into an ancestor's body, a static one, a Dynamic body or a `CharacterController` fights the placement.
- **When it is not attached** (no Animator above it, an empty or misspelt `Bone`, an Animator without a pose), it keeps its Transform, and `ANIMATION_ATTACHMENT_INVALID` names the reason once (the validator's message names close joint names). Nothing is placed in the editor's edit view or its animation preview: there it sits at its authored Transform.

A dodge that folds the staff along the forearm, the switch made in the fixed step so it is in force that tick:

```luau path=Assets/Scripts/StaffFold.luau
--!strict
export type StaffFold = ScriptInstance & {
	Staff: Entity?,
	Fist: string,
	Forearm: string,
}

local StaffFold = {}

StaffFold.Fields = {
	Staff = Field.Entity(),
	Fist = Field.String("DEF-hand.R"),
	Forearm = Field.String("DEF-forearm.R"),
}

function StaffFold.OnFixedUpdate(self: StaffFold, dt: number)
	local staff = self.Staff
	local attachment = if staff then staff.BoneAttachment else nil
	if attachment == nil then
		return
	end
	if Input.IsActionDown("Dash") then
		attachment.Bone = self.Forearm
		attachment.Rotation = Quat.FromEuler(0, 0, 90)
	else
		attachment.Bone = self.Fist
		attachment.Rotation = Quat.Identity()
	end
end

return Script.Define("StaffFold", StaffFold)
```

**Root motion** (`RootMotion`): `None` keeps the hips' planar motion in the pose (the mesh walks away from its entity); `Apply` moves the entity's Transform by it each step (it reaches physics like a script's write); `Extract` holds it out of the pose and only reports it through `GetRootMotion`, for a script to move a `CharacterController` (`controller:Move(moved / dt)`) or a body.

**Events:** `OnAnimationEvent(self, event: AnimationEvent)` with `event.Name`, `event.Layer` (1-based), `event.Clip` and `event.Time` (seconds into the clip) is called when a layer's time passes an event of its clip, on every script slot of the Animator's entity in slot order, after every Animator of the tick was posed, in canonical entity, layer and firing order. A looping layer fires an event at the clip's end before one at its start. A behaviour that has not started holds its events until after its `OnStart`, and an Animator delivers at most 256 events a step (the rest are dropped with one warning). glTF has no events: list them in the model's import settings (`AnimationEvents`, each `{Animation, Name, Time}`, through `asset.setImportSettings`).

**Determinism:** poses are sampled in the fixed step with the engine's deterministic math, and the layers' times are component data the state hash covers, so a replay reproduces them. A suite compares a pose across the Editor, Release and Dist runs with `Test.GetPoseDigest(entity)` (16 hexadecimal digits, or nil without a pose). Read joints and root motion only in the fixed step; a frame between two ticks draws the last tick's pose.
