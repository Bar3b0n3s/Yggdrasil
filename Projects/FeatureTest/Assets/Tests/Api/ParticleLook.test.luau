--!strict
-- The ParticleLook component (M33; Docs/Decisions/0048-m33-particle-look.md decisions 1, 2, 7 and 8) in ParticleLook.scene
-- (ParticleLook.jsonl, IdSeed 3001), whose three emitters' looks hold every field off its default and every value of
-- ParticleFrameMode and ParticleAlignment: Test.GetParticleDigest of emitters with a look against digests committed from
-- the Editor run, so the Release and Dist runs prove the look's second random stream (angles, spins and frames) equal in
-- every configuration; and every field read and written through the proxy, the look's rules refusing what breaks them. The
-- suite runs each case on a fresh load of the scene (Isolation "Case"), so a digest depends only on the ticks its case waits.
local Support = require("./Support")

-- Test.GetParticleDigest after DIGEST_TICKS ticks of a fresh scene, committed from the Editor run.
local DIGEST_TICKS = 64
local FLAME_DIGEST = "5b53e4e1a02713bd"
local RING_DIGEST = "0d77ad9e9a49e112"

local function look(name: string): ParticleLook
	local component = Support.Find(name).ParticleLook
	assert(component, `{name} has a ParticleLook`)
	return component
end

return Test.Suite("Api.ParticleLook", function()
	Test.Case("seeded particles with a look hash to their committed digests in every run mode", function()
		Test.WaitTicks(DIGEST_TICKS)
		Test.ExpectEqual(Test.GetParticleDigest(Support.Find("Flame")), FLAME_DIGEST, "the turning, blended flipbook flame")
		Test.ExpectEqual(Test.GetParticleDigest(Support.Find("Ring")), RING_DIGEST, "the flat ring's random frames")
		local sparks = Support.Find("Sparks").ParticleEmitter
		assert(sparks, "Sparks has a ParticleEmitter")
		Test.Expect(sparks:GetParticleCount() > 0, "the stretched sparks fly")
		Test.Screenshot("ParticleLook")
	end)

	Test.Case("every field reads and writes through the proxy, and the look's rules refuse what breaks them", function()
		local flame = look("Flame")
		local sheet = Support.RoundTrip(flame, "SpriteSheet", nil)
		Test.Expect(sheet ~= nil, "the flame's flipbook sheet")
		flame.SpriteSheet = sheet
		Test.ExpectEqual(flame.SpriteSheet, sheet, "the sheet written back")
		Test.ExpectEqual(Support.RoundTrip(flame, "FirstFrame", 3), 0)
		Test.ExpectEqual(Support.RoundTrip(flame, "FrameCount", 5), 0)
		Test.ExpectEqual(Support.RoundTrip(flame, "FrameMode", "Random"), "OverLife")
		Test.ExpectNear(Support.RoundTrip(flame, "FrameCycles", 0.5, 1e-6), 2, 1e-6)
		Test.ExpectEqual(Support.RoundTrip(flame, "BlendFrames", false), true)
		Test.ExpectNear(Support.RoundTrip(flame, "StartRotation", vector.create(-90, 45, 0), 1e-6), vector.create(-20, 20, 0), 1e-6)
		Test.ExpectNear(Support.RoundTrip(flame, "Spin", vector.create(90, 180, 0), 1e-6), vector.create(-30, 30, 0), 1e-6)
		Test.ExpectEqual(Support.RoundTrip(flame, "Alignment", "Flat"), "Camera")
		Test.ExpectNear(Support.RoundTrip(flame, "VelocityStretch", 0.25, 1e-6), 0.1, 1e-6)
		Test.ExpectNear(Support.RoundTrip(flame, "SoftDistance", 1.25, 1e-6), 0.4, 1e-6)
		Test.ExpectNear(Support.RoundTrip(flame, "Intensity", 12, 1e-6), 1.5, 1e-6)
		Test.ExpectEqual(look("Sparks").Alignment, "Velocity", "the sparks stretch along their velocity")
		Test.ExpectEqual(look("Ring").FrameMode, "Random", "the ring picks a frame per particle")

		Test.Expect(not pcall(function()
			flame.StartRotation = vector.create(30, -30, 0)
		end), "an angle range whose minimum exceeds its maximum is refused")
		Test.Expect(not pcall(function()
			flame.Spin = vector.create(5, -5, 0)
		end), "a spin range whose minimum exceeds its maximum is refused")
		Test.Expect(not pcall(function()
			flame.FrameCycles = 0
		end), "a frame cycle count below 0.01 is refused")
		Test.Expect(not pcall(function()
			flame.Intensity = -1
		end), "a negative intensity is refused")
		Test.Expect(not pcall(function()
			flame.FirstFrame = 70000
		end), "a first frame past the largest sheet is refused")
		Test.Expect(not pcall(function()
			(flame :: any).Alignment = "Diagonal"
		end), "an alignment ParticleAlignment lacks is refused")
		Test.ExpectNear(flame.Intensity, 12, 1e-6, "a refused write leaves the field")
	end)
end)
