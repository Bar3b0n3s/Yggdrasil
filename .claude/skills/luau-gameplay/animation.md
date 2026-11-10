# Skeletal and morph animation in scripts

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): the script surface of the `Animator` and `SkinnedMesh` components, blending, root motion and animation events. The signatures are in `reference/ScriptAPI` (sections `Animator`, `SkinnedMesh`, `Behaviour` and `Test`).

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
- `Animator:GetJointTransform(joint) -> (vector?, Quat?)` reads the world position and rotation of a skeleton joint (by its glTF node name) in the pose of the last AnimationSystem phase; nil, nil without a pose or such a joint, and a position with a nil rotation when a scale of 0 on the way (a clip scaling a joint to nothing) leaves the joint no rotation, so check the rotation before you use it. Attach a sword or a muzzle flash with it.
- `Animator:GetRootMotion() -> (vector, Quat)` reads the last step's root motion in the entity's parent space.
- `SkinnedMesh:SetMorphWeight(target, weight)` sets the weight of the morph target named `target` (a blink, a smile) in [-1, 1], growing `MorphWeights` with the targets' defaults; a name the mesh lacks raises NotFound. A clip's morph channel overrides the weights it keys.

**When it runs:** every Animator steps in the AnimationSystem phase of the fixed step, after `OnFixedUpdate`, tasks and native systems and before the transform update, in Play and Simulate. A clip played in `OnFixedUpdate` is sampled at that tick; one played in a contact callback or `OnDestroy` advances from the next tick. While `Playing` is false the times stay but the pose follows them, so a script scrubs by writing `Time`. A looping layer passes at most eight ends of its clip in one step.

**Root motion** (`RootMotion`): `None` keeps the hips' planar motion in the pose (the mesh walks away from its entity); `Apply` moves the entity's Transform by it each step (it reaches physics like a script's write); `Extract` holds it out of the pose and only reports it through `GetRootMotion`, for a script to move a `CharacterController` (`controller:Move(moved / dt)`) or a body.

**Events:** `OnAnimationEvent(self, event: AnimationEvent)` with `event.Name`, `event.Layer` (1-based), `event.Clip` and `event.Time` (seconds into the clip) is called when a layer's time passes an event of its clip, on every script slot of the Animator's entity in slot order, after every Animator of the tick was posed, in canonical entity, layer and firing order. A looping layer fires an event at the clip's end before one at its start. A behaviour that has not started holds its events until after its `OnStart`, and an Animator delivers at most 256 events a step (the rest are dropped with one warning). glTF has no events: list them in the model's import settings (`AnimationEvents`, each `{Animation, Name, Time}`, through `asset.setImportSettings`).

**Determinism:** poses are sampled in the fixed step with the engine's deterministic math, and the layers' times are component data the state hash covers, so a replay reproduces them. A suite compares a pose across the Editor, Release and Dist runs with `Test.GetPoseDigest(entity)` (16 hexadecimal digits, or nil without a pose). Read joints and root motion only in the fixed step; a frame between two ticks draws the last tick's pose.
