--!strict
-- Skeletal and morph animation in every run mode (M23 W23.3; Docs/Decisions/0034-m23-contract.md decisions 25 to 33;
-- Animation.jsonl, IdSeed 2107): Animation.scene holds three imported rigs. The Waver (Skinned.gltf) plays the Wave clip
-- on two layers with RootMotion Extract and an EventProbe behaviour; the Statue (Skinned.gltf) has no layer and a morph
-- weight; the Walker's Character (SkinnedRigs.gltf) stands with Playing false and RootMotion Apply until its Director
-- behaviour or a case starts it. The pose digest is committed from the Editor run, so the exported Release and Dist runs
-- prove that a sampled pose hashes equal in every configuration (Roadmap M23).
local Animation = require("./Animation")
local EventProbeScript = require("../../Scripts/Animation/EventProbe")

-- Test.GetPoseDigest after the case below waits its ticks, committed from the Editor run.
local WAVER_DIGEST = "3ce4af55e765a3aa"
local STATUE_DIGEST = "822af7e0482d6a11"

return Test.Suite("Animation", function()
	Test.Case("the scene holds its Animators and SkinnedMeshes as authored", function()
		local wave = Animation.Load(Animation.WAVE)
		Test.ExpectEqual(Assets.GetType(wave), "AnimationClip")
		local waver = Animation.Animator("/Waver/Armature")
		Test.ExpectEqual(Assets.GetType(waver.Skeleton :: AssetRef), "Skeleton")
		Test.ExpectEqual(waver.RootMotion, "Extract")
		Test.ExpectEqual(waver.Playing, true)
		local layers = waver.Layers
		Test.ExpectEqual(#layers, 2)
		Test.ExpectEqual(layers[1].Clip, wave)
		Test.ExpectEqual(layers[2].Clip, wave)
		Test.ExpectEqual(layers[2].Speed, 0.5)
		Test.ExpectEqual(layers[2].Weight, 0.5)
		Test.ExpectEqual(layers[2].Loop, false)
		local statue = Animation.Animator("/Statue/Armature")
		Test.ExpectEqual(#statue.Layers, 0)
		Test.ExpectEqual(statue.RootMotion, "None")
		local weights = (Animation.Find("/Statue/Armature/Body").SkinnedMesh :: SkinnedMesh).MorphWeights
		Test.ExpectEqual(#weights, 1)
		Test.ExpectEqual(weights[1], 0.75)
		local walker = Animation.Animator("/Walker/Character")
		Test.ExpectEqual(walker.Playing, false)
		Test.ExpectEqual(walker.RootMotion, "Apply")
		Test.ExpectEqual(walker.Layers[1].Clip, Animation.Load(Animation.WALK))
	end)

	Test.Case("a sampled pose hashes to its committed digest in every run mode", function()
		Test.WaitTicks(40)
		Test.ExpectEqual(Test.GetPoseDigest(Animation.Find("/Waver/Armature")), WAVER_DIGEST)
		Test.ExpectEqual(Test.GetPoseDigest(Animation.Find("/Statue/Armature")), STATUE_DIGEST)
		Test.ExpectEqual(Test.GetPoseDigest(Animation.Find("/Floor")), nil, "an entity without an Animator has no pose")
	end)

	Test.Case("events reach OnAnimationEvent in tick, layer and clip order", function()
		local probe = Animation.Find("/Waver/Armature"):GetScript("EventProbe") :: EventProbeScript.EventProbe
		Test.WaitUntil(function()
			return probe.Count >= 6
		end, 240)
		local wave = Animation.Load(Animation.WAVE)
		-- Layer 1 loops from 0 at speed 1; layer 2 holds its end, from 0.25 at speed 0.5: both pass Peak in tick 33, and
		-- layer 1's End fires before its next Start.
		local expected: { { Name: string, Layer: number, Time: number } } = {
			{ Name = "Start", Layer = 1, Time = 0 },
			{ Name = "Peak", Layer = 1, Time = 0.5 },
			{ Name = "Peak", Layer = 2, Time = 0.5 },
			{ Name = "End", Layer = 1, Time = 1 },
			{ Name = "Start", Layer = 1, Time = 0 },
			{ Name = "End", Layer = 2, Time = 1 },
		}
		for index, entry in expected do
			local event = probe.Events[index]
			Test.ExpectEqual(event.Name, entry.Name, `event {index}'s name`)
			Test.ExpectEqual(event.Layer, entry.Layer, `event {index}'s layer`)
			Test.ExpectNear(event.Time, entry.Time, 1e-6, `event {index}'s time`)
			Test.ExpectEqual(event.Clip, wave, `event {index}'s clip`)
		end
	end)

	Test.Case("root motion Apply moves the entity, Extract only reports it and None keeps it in the pose", function()
		local character = Animation.Find("/Walker/Character")
		local walker = Animation.Animator("/Walker/Character")
		local transform = character.Transform :: Transform
		local start = transform.Translation
		walker.Playing = true
		Test.WaitTicks(32)
		local moved = transform.Translation - start
		Test.Expect(vector.magnitude(vector.create(moved.x, 0, moved.z)) > 0.3, `Apply moved the Walker ({moved})`)
		Test.ExpectNear(moved.y, 0, 1e-4, "root motion is planar")
		local motion, turn = walker:GetRootMotion()
		Test.Expect(vector.magnitude(motion) > 0, "Apply reports the step's motion")
		Test.ExpectNear(turn.w, 1, 1e-4, "the walk does not turn")

		walker.RootMotion = "Extract"
		Test.WaitTicks(1)
		local held = transform.Translation
		Test.WaitTicks(8)
		Test.ExpectEqual(transform.Translation, held, "Extract leaves the Transform")
		motion = walker:GetRootMotion()
		Test.Expect(vector.magnitude(motion) > 0, "Extract reports the step's motion")

		walker.RootMotion = "None"
		Test.WaitTicks(1)
		motion, turn = walker:GetRootMotion()
		Test.ExpectEqual(motion, vector.zero, "None reports no motion")
		Test.ExpectEqual(turn.w, 1, "and no turn")
		Test.ExpectEqual(transform.Translation, held, "None leaves the Transform")
	end)

	Test.Case("the posed and morphed meshes render", function()
		Test.WaitTicks(2)
		Test.Screenshot("Animation")
		local minimum, maximum = Animation.Find("/Waver/Armature/Body"):GetWorldBounds()
		Test.Expect(minimum ~= nil and maximum ~= nil, "a skinned mesh has render bounds")
	end)
end)
