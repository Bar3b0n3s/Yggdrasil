--!strict
-- The ParticleEmitter proxy and the particle script surface (M26, C-PART; Docs/Decisions/0037-m26-contract.md decisions 5,
-- 6, 25 and 26) in Particles.scene (Particles.jsonl, IdSeed 2401), whose four emitters hold every field off its default and
-- every shape, space and blend: Test.GetParticleDigest against digests committed from the Editor run, so the Release and
-- Dist runs prove the seeded simulation equal in every configuration; Emit, Clear and GetParticleCount; and every field
-- read and written. The suite runs each case on a fresh load of the scene (Isolation "Case"), so a digest depends only on
-- the ticks its case waits.
local Support = require("./Support")

-- Test.GetParticleDigest after DIGEST_TICKS ticks of a fresh scene, committed from the Editor run.
local DIGEST_TICKS = 64
local FOUNTAIN_DIGEST = "3b5d89a880fa484b"
local SPARKS_DIGEST = "f05b301294ea48f1"

local function emitter(name: string): ParticleEmitter
	local component = Support.Find(name).ParticleEmitter
	assert(component, `{name} has a ParticleEmitter`)
	return component
end

return Test.Suite("Api.ParticleEmitter", function()
	Test.Case("seeded particles hash to their committed digests in every run mode", function()
		Test.WaitTicks(DIGEST_TICKS)
		Test.ExpectEqual(Test.GetParticleDigest(Support.Find("Fountain")), FOUNTAIN_DIGEST, "the cone in world space")
		Test.ExpectEqual(Test.GetParticleDigest(Support.Find("Sparks")), SPARKS_DIGEST, "the sphere's burst in local space")
		Test.ExpectEqual(Test.GetParticleDigest(Support.Find("Camera")), nil, "an entity without a ParticleEmitter has no digest")
		Test.ExpectEqual(emitter("Sparks"):GetParticleCount(), 48, "the one-shot burst lives on")
		Test.Expect(emitter("Fountain"):GetParticleCount() > 0, "the fountain emits at its rate")
		Test.Screenshot("Particles")
	end)

	Test.Case("Emit queues particles for the next phase whatever Playing says, and Clear kills them", function()
		local puff = emitter("Puff")
		Test.ExpectEqual(puff.Playing, false, "the puff emits only when asked")
		Test.WaitTicks(2)
		Test.ExpectEqual(puff:GetParticleCount(), 0, "a stopped emitter emits nothing by itself")
		puff:Emit(12)
		puff:Emit(4)
		Test.ExpectEqual(puff:GetParticleCount(), 0, "the emits wait for the next Particles phase")
		Test.WaitTicks(1)
		Test.ExpectEqual(puff:GetParticleCount(), 16)
		puff:Emit(3)
		puff:Clear()
		Test.ExpectEqual(puff:GetParticleCount(), 0, "Clear kills every live particle")
		Test.WaitTicks(1)
		Test.ExpectEqual(puff:GetParticleCount(), 0, "and drops the queued emits")
		for _, count in { 0, 4097, 1.5 } do
			Test.Expect(not pcall(function()
				puff:Emit(count)
			end), `a count of {count} raises`)
		end
		puff.MaxParticles = 10
		puff:Emit(25)
		Test.WaitTicks(1)
		Test.ExpectEqual(puff:GetParticleCount(), 10, "emits beyond MaxParticles are dropped")
	end)

	Test.Case("every field reads and writes through the proxy, and the emitter's rules refuse what breaks them", function()
		local fountain = emitter("Fountain")
		Test.ExpectEqual(Support.RoundTrip(fountain, "Playing", false), true)
		Test.ExpectEqual(Support.RoundTrip(fountain, "Loop", false), true)
		Test.ExpectNear(Support.RoundTrip(fountain, "Duration", 2.5, 1e-6), 5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(fountain, "Rate", 12.5, 1e-6), 40, 1e-6)
		Test.ExpectEqual(Support.RoundTrip(fountain, "Burst", 8), 0)
		Test.ExpectEqual(Support.RoundTrip(fountain, "MaxParticles", 100), 200)
		Test.ExpectEqual(Support.RoundTrip(fountain, "Seed", 99), 11)
		Test.ExpectNear(Support.RoundTrip(fountain, "Lifetime", vector.create(0.5, 1, 0), 1e-6), vector.create(1.5, 2.5, 0), 1e-6)
		Test.ExpectNear(Support.RoundTrip(fountain, "Speed", vector.create(1, 1, 0), 1e-6), vector.create(3, 4, 0), 1e-6)
		Test.ExpectNear(Support.RoundTrip(fountain, "Size", vector.create(0.05, 0.5, 0), 1e-6), vector.create(0.15, 0.3, 0), 1e-6)
		Test.ExpectEqual(Support.RoundTrip(fountain, "Shape", "Box"), "Cone")
		Test.ExpectNear(Support.RoundTrip(fountain, "ShapeRadius", 0.75, 1e-6), 0.2, 1e-6)
		Test.ExpectNear(Support.RoundTrip(fountain, "ConeAngle", 60, 1e-6), 15, 1e-6)
		Test.ExpectNear(Support.RoundTrip(fountain, "BoxHalfExtents", vector.create(2, 1, 0.25), 1e-6), vector.create(0.5, 0.5, 0.5), 1e-6)
		Test.ExpectNear(Support.RoundTrip(fountain, "Acceleration", vector.create(0, -9.81, 0), 1e-6), vector.create(0, -4, 0), 1e-6)
		Test.ExpectNear(Support.RoundTrip(fountain, "Drag", 1, 1e-6), 0.2, 1e-6)
		Test.ExpectEqual(Support.RoundTrip(fountain, "SimulationSpace", "Local"), "World")
		Test.ExpectEqual(Support.RoundTrip(fountain, "Blend", "Additive"), "Alpha")
		local circle = Assets.Load("engine://Textures/SpriteCircle")
		Test.ExpectEqual(Support.RoundTrip(fountain, "Texture", circle), nil, "the built-in particle texture")

		local colors = fountain.ColorOverLife
		Test.ExpectEqual(#colors, 2)
		Test.ExpectEqual(colors[2].Time, 1)
		Support.ExpectColorNear(colors[2].Color, Color.New(0.2, 0.5, 1, 0), 1e-6, "the authored last colour key")
		fountain.ColorOverLife = { { Time = 0.5, Color = Color.New(1, 0.25, 0, 1) } }
		Test.ExpectEqual(#fountain.ColorOverLife, 1)
		Test.ExpectEqual(fountain.ColorOverLife[1].Time, 0.5)
		Support.ExpectColorNear(fountain.ColorOverLife[1].Color, Color.New(1, 0.25, 0, 1), 1e-6, "ColorOverLife reads back what was written")
		local sizes = fountain.SizeOverLife
		Test.ExpectEqual(#sizes, 2)
		Test.ExpectEqual(sizes[1].Size, 0.5)
		fountain.SizeOverLife = { { Time = 0.25, Size = 2 }, { Time = 0.75, Size = 0.5 } }
		Test.ExpectEqual(#fountain.SizeOverLife, 2)
		Test.ExpectEqual(fountain.SizeOverLife[2].Size, 0.5)

		Test.Expect(not pcall(function()
			fountain.Lifetime = vector.create(3, 1, 0)
		end), "a range whose minimum exceeds its maximum is refused")
		Test.Expect(not pcall(function()
			fountain.Burst = 101
		end), "a burst above MaxParticles is refused")
		Test.Expect(not pcall(function()
			fountain.SizeOverLife = { { Time = 0.5, Size = 1 }, { Time = 0.25, Size = 1 } }
		end), "curve keys out of order are refused")
		Test.Expect(not pcall(function()
			fountain.ConeAngle = 200
		end), "a cone angle above 180 degrees is refused")
		Test.Expect(not pcall(function()
			(fountain :: any).Shape = "Ring"
		end), "a shape ParticleEmitterShape lacks is refused")
		Test.ExpectNear(fountain.ConeAngle, 60, 1e-6, "a refused write leaves the field")
	end)
end)
