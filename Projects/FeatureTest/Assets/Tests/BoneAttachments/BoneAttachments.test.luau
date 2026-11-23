--!strict
-- Bone attachments in every run mode (M41; Docs/Decisions/0058-m41-bone-attachments.md decision 3; BoneAttachments.jsonl,
-- IdSeed 3801). BoneAttachments.scene holds two imported rigs: the Waver (Skinned.gltf) waves its Joint1 45 degrees about
-- Z, and holds a Sword on Joint1 (a Kinematic body with a trigger, a Gem light riding it, and a Charm below it attached to
-- Joint0); the Walker's Character (SkinnedRigs.gltf, RootMotion Apply) stands until a case starts its walk, with a Pack on
-- its Spine and a Rider (another Skinned.gltf) on its Hips. Each case starts the scene afresh (Isolation "Case"). Every check reads the
-- attachments between ticks, where Animator:GetJointTransform reads the pose of the last AnimationSystem phase.

-- The tolerance of Brief 22's check: an attached entity stays in the fist within a millimetre.
local MILLIMETRE = 1e-3
-- The Sword's trigger BoxCollider, centred on the Sword, in its axes.
local SWORD_HALF_EXTENTS = vector.create(0.05, 0.4, 0.05)

local function Find(path: string): Entity
	local entity = Scene.FindByPath(path)
	assert(entity, `{path} exists`)
	return entity :: Entity
end

local function Attachment(path: string): BoneAttachment
	local attachment = Find(path).BoneAttachment
	assert(attachment, `{path} holds a BoneAttachment`)
	return attachment :: BoneAttachment
end

-- Where `bone` of the Animator on `animatorPath` holds an attachment of `offset`: the joint's world position plus its
-- world rotation times the offset (the joints of these models have the scale 1), and that rotation.
local function OnBone(animatorPath: string, bone: string, offset: vector): (vector, Quat)
	local animator = Find(animatorPath).Animator
	assert(animator, `{animatorPath} holds an Animator`)
	local position, rotation = (animator :: Animator):GetJointTransform(bone)
	assert(position and rotation, `{animatorPath} poses {bone}`)
	local turn = rotation :: Quat
	return (position :: vector) + turn * offset, turn
end

local function ExpectOnBone(path: string, animatorPath: string, bone: string)
	local attachment = Attachment(path)
	local expected, rotation = OnBone(animatorPath, bone, attachment.Offset)
	local actual = Find(path).Transform.WorldPosition
	Test.Expect(vector.magnitude(actual - expected) <= MILLIMETRE, `{path} is within 1 mm of {bone} ({actual} against {expected})`)
	-- The world rotation is the joint's times the attachment's: both turn an axis alike.
	local world = Find(path).Transform.WorldRotation
	local turned = (rotation * attachment.Rotation) * vector.create(0, 0, 1)
	Test.Expect(vector.magnitude(world * vector.create(0, 0, 1) - turned) <= MILLIMETRE, `{path} turns with {bone}`)
end

local function StartWalk()
	local animator = Find("/Walker/Character").Animator
	assert(animator, "the Walker's Character holds an Animator");
	(animator :: Animator).Playing = true
end

return Test.Suite("BoneAttachments", function()
	Test.Case("the scene holds its attachments as authored", function()
		local sword = Attachment("/Waver/Armature/Sword")
		Test.ExpectEqual(sword.Bone, "Joint1")
		Test.ExpectNear(sword.Offset, vector.create(0, 0.6, 0.05), 1e-6)
		Test.ExpectNear(sword.Rotation.y, 0.7071068, 1e-6)
		Test.ExpectEqual(Attachment("/Waver/Armature/Sword/Charm").Bone, "Joint0")
		Test.ExpectEqual(Attachment("/Walker/Character/Pack").Bone, "Spine")
		Test.ExpectEqual(Attachment("/Walker/Character/Rider").Bone, "Hips")
		Test.Expect(Find("/Waver/Armature/Sword").RigidBody ~= nil, "the Sword holds its Kinematic body")
	end)

	Test.Case("attached entities stay within 1 mm of their bones at several clip times", function()
		StartWalk()
		for _ = 1, 6 do
			Test.WaitTicks(7)
			ExpectOnBone("/Waver/Armature/Sword", "/Waver/Armature", "Joint1")
			ExpectOnBone("/Waver/Armature/Sword/Charm", "/Waver/Armature", "Joint0")
			ExpectOnBone("/Walker/Character/Pack", "/Walker/Character", "Spine")
			ExpectOnBone("/Walker/Character/Rider", "/Walker/Character", "Hips")
		end
		-- The walk moved the Character by root motion, and its attachments with it.
		Test.Expect(vector.magnitude(Find("/Walker/Character").Transform.Translation) > 0.1, "the Walker walked")
		-- The Gem rides the Sword, 0.4 m along its axis.
		local sword = Find("/Waver/Armature/Sword").Transform
		local expected = sword.WorldPosition + sword.WorldRotation * vector.create(0, 0.4, 0)
		Test.ExpectNear(Find("/Waver/Armature/Sword/Gem").Transform.WorldPosition, expected, MILLIMETRE)
	end)

	Test.Case("a bone switch written by a test case takes effect in the same tick", function()
		Test.WaitTicks(10)
		Attachment("/Waver/Armature/Sword").Bone = "Joint0"
		-- The case writes in the Tasks phase, before the tick's AnimationSystem phase; it resumes after that tick, so the
		-- Sword is already on Joint0.
		Test.WaitTicks(1)
		ExpectOnBone("/Waver/Armature/Sword", "/Waver/Armature", "Joint0")
	end)

	Test.Case("Offset and Rotation writes move the attachment", function()
		local attachment = Attachment("/Waver/Armature/Sword")
		attachment.Offset = vector.create(0.1, 0.3, -0.2)
		attachment.Rotation = Quat.FromEuler(30, 0, 15)
		Test.ExpectNear(attachment.Offset, vector.create(0.1, 0.3, -0.2), 1e-6)
		Test.WaitTicks(1)
		ExpectOnBone("/Waver/Armature/Sword", "/Waver/Armature", "Joint1")
		Test.WaitTicks(9)
		ExpectOnBone("/Waver/Armature/Sword", "/Waver/Armature", "Joint1")
	end)

	Test.Case("a disabled attachment holds and is placed again once enabled", function()
		local sword = Find("/Waver/Armature/Sword")
		Test.WaitTicks(2)
		sword:SetActive(false)
		Test.WaitTicks(1)
		local held = sword.Transform.Translation
		Test.WaitTicks(10)
		Test.ExpectEqual(sword.Transform.Translation, held, "a disabled attachment is not placed")
		sword:SetActive(true)
		Test.WaitTicks(1)
		ExpectOnBone("/Waver/Armature/Sword", "/Waver/Armature", "Joint1")
		Test.Expect(sword.Transform.Translation ~= held, "enabled again, it follows the waving joint")
	end)

	Test.Case("removing the BoneAttachment keeps the last Transform, and reparenting away detaches", function()
		StartWalk()
		Test.WaitTicks(5)
		-- The Pack, on the walking Spine, keeps the Transform it was placed at when its BoneAttachment goes.
		ExpectOnBone("/Walker/Character/Pack", "/Walker/Character", "Spine")
		local pack = Find("/Walker/Character/Pack")
		local placed = pack.Transform.Translation
		pack:RemoveComponent("BoneAttachment")
		Test.WaitTicks(10)
		Test.ExpectEqual(pack.Transform.Translation, placed, "a removed attachment keeps the Transform it was placed at")
		-- The Rider, reparented away from the Walker while the Hips it was on walk on, stays where it was. Its attachment
		-- then has no Animator above it: the run logs one ANIMATION_ATTACHMENT_INVALID warning naming /Rider, as expected.
		local rider = Find("/Walker/Character/Rider")
		local world = rider.Transform.WorldPosition
		local hips = OnBone("/Walker/Character", "Hips", vector.zero)
		rider:SetParent(nil, true)
		Test.WaitTicks(10)
		Test.ExpectNear(rider.Transform.WorldPosition, world, 1e-5, "a detached Rider stays where it was")
		local walked = OnBone("/Walker/Character", "Hips", vector.zero)
		Test.Expect(vector.magnitude(walked - hips) > 0.1, "while the Hips it was on walked on")
	end)

	Test.Case("the world pause holds an attachment with its pose", function()
		local sword = Find("/Waver/Armature/Sword")
		Test.WaitTicks(5)
		Time.PauseWorld()
		Test.WaitTicks(1)
		local held = sword.Transform.Translation
		Test.WaitTicks(10)
		Test.ExpectEqual(sword.Transform.Translation, held, "the pause holds the attachment")
		ExpectOnBone("/Waver/Armature/Sword", "/Waver/Armature", "Joint1")
		Time.ResumeWorld()
		Test.WaitTicks(3)
		Test.Expect(sword.Transform.Translation ~= held, "resumed, it follows the waving joint again")
		ExpectOnBone("/Waver/Armature/Sword", "/Waver/Armature", "Joint1")
	end)

	Test.Case("the Sword's trigger fires where the bone holds it", function()
		local probe = Find("/Waver/Armature/Sword"):GetScript("TriggerProbe")
		Test.WaitUntil(function()
			return probe.Count >= 1
		end, 120)
		Test.ExpectEqual(probe.Other, "Target")
		-- A sensor's contact is found from the bodies' poses at the start of the physics step, and the AnimationSystem phase
		-- places the Sword before PhysicsPreStep sweeps its Kinematic body there. So the contact begins exactly one tick
		-- after the first pose of the bone that overlaps the Target: the bone-held pose of the tick before the contact
		-- overlaps it, and the one before that does not. A body that lagged its bone meets the Target a tick later, one
		-- teleported rather than swept a tick earlier, and either fails one of the two checks.
		local function OverlapsTarget(pose: { Position: vector, Rotation: Quat }?): boolean
			assert(pose, "the probe kept the bone's poses before the contact")
			for _, hit in Physics.OverlapBox(pose.Position, SWORD_HALF_EXTENTS, pose.Rotation) do
				if hit.Name == "Target" then
					return true
				end
			end
			return false
		end
		Test.Expect(OverlapsTarget(probe.Last), "the bone held the Sword on the Target in the tick before the contact")
		Test.Expect(not OverlapsTarget(probe.Earlier), "and clear of it in the tick before that")
	end)
end)
