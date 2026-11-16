--!strict
-- The MaterialParameters proxy (M31; Docs/Decisions/0046-m31-material-parameters.md decisions 1, 8 and 9) in
-- MaterialParameters.scene (MaterialParameters.jsonl, IdSeed 2801), whose entities hold every field off its default: the
-- authored values in every run mode, every field read and written, the ranges refusing what the renderer cannot draw, a
-- scroll that is presentation only (the field stays as authored while the surface moves) and the Enemy's dissolve driven
-- by input. Each case starts from the scene as saved (Isolation "Case").
local Support = require("./Support")

local function parameters(name: string): MaterialParameters
	local component = Support.Find(name).MaterialParameters
	assert(component, `{name} has MaterialParameters`)
	return component
end

return Test.Suite("Api.MaterialParameters", function()
	Test.Case("the authored parameters keep their values in every run mode", function()
		local tinted = parameters("Tinted")
		Support.ExpectColorNear(tinted.Tint, Color.New(0.9, 0.35, 0.2), 1e-6, "the tint")
		Support.ExpectColorNear(tinted.Emissive, Color.New(1, 0.6, 0.2), 1e-6, "the glow's colour")
		Test.ExpectEqual(tinted.EmissiveStrength, 1.5)
		local lava = parameters("Lava")
		Test.ExpectNear(lava.UVOffset, vector.create(0.25, 0, 0), 1e-6, "the lava's offset")
		Test.ExpectNear(lava.UVScroll, vector.create(0.1, 0.05, 0), 1e-6, "the lava's scroll")
		local ghost = parameters("Ghost")
		Test.ExpectEqual(ghost.DissolveAmount, Support.F32(0.4))
		Support.ExpectColorNear(ghost.DissolveEdgeColor, Color.New(0.2, 0.6, 1), 1e-6, "the edge's colour")
		Test.ExpectEqual(ghost.DissolveEdgeStrength, 3)
		Test.ExpectEqual(ghost.DissolveEdgeWidth, Support.F32(0.1))
		Test.ExpectEqual(ghost.DissolveNoiseScale, 2)
		Test.ExpectEqual(parameters("Wall").Opacity, 0.5)
		Test.ExpectEqual(parameters("Enemy").DissolveAmount, 0)
		Test.Screenshot("MaterialParameters")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local tinted = parameters("Tinted")
		Support.ExpectColorNear(Support.RoundTrip(tinted, "Tint", Color.New(0.5, 0.25, 1)), Color.New(0.9, 0.35, 0.2), 1e-6, "the tint")
		Support.ExpectColorNear(Support.RoundTrip(tinted, "Emissive", Color.New(0, 1, 0)), Color.New(1, 0.6, 0.2), 1e-6, "the glow")
		Test.ExpectEqual(Support.RoundTrip(tinted, "EmissiveStrength", 6), 1.5)
		local lava = parameters("Lava")
		Test.ExpectNear(Support.RoundTrip(lava, "UVOffset", vector.create(-0.5, 2, 0), 1e-6), vector.create(0.25, 0, 0), 1e-6)
		Test.ExpectNear(Support.RoundTrip(lava, "UVScroll", vector.create(0, -1, 0), 1e-6), vector.create(0.1, 0.05, 0), 1e-6)
		local ghost = parameters("Ghost")
		Test.ExpectEqual(Support.RoundTrip(ghost, "DissolveAmount", 0.75), Support.F32(0.4))
		Support.ExpectColorNear(Support.RoundTrip(ghost, "DissolveEdgeColor", Color.New(1, 0, 0)), Color.New(0.2, 0.6, 1), 1e-6, "the edge")
		Test.ExpectEqual(Support.RoundTrip(ghost, "DissolveEdgeStrength", 8), 3)
		Test.ExpectEqual(Support.RoundTrip(ghost, "DissolveEdgeWidth", 0.25), Support.F32(0.1))
		Test.ExpectEqual(Support.RoundTrip(ghost, "DissolveNoiseScale", 0.5), 2)
		Test.ExpectEqual(Support.RoundTrip(parameters("Wall"), "Opacity", 0.25), 0.5)
		Test.WaitTicks(1)
		Test.Screenshot("MaterialParametersWritten")
	end)

	Test.Case("each field refuses a value outside its range", function()
		local ghost = parameters("Ghost")
		local refusals: { [string]: any } = {
			DissolveAmount = 1.5,
			Opacity = -0.5,
			DissolveNoiseScale = 0,
			EmissiveStrength = -1,
			DissolveEdgeWidth = 2,
		}
		for field, value in refusals do
			Test.Expect(not pcall(function()
				(ghost :: any)[field] = value
			end), `{field} = {value} is refused`)
		end
		Test.ExpectEqual(ghost.DissolveAmount, Support.F32(0.4), "a refused write changes nothing")
	end)

	Test.Case("the scroll leaves the field as authored, and a press dissolves the Enemy", function()
		local lava = parameters("Lava")
		Test.WaitTicks(32)
		Test.ExpectNear(lava.UVOffset, vector.create(0.25, 0, 0), 1e-6, "the scroll is presentation: the field stays as authored")
		local enemy = parameters("Enemy")
		Test.ExpectEqual(enemy.DissolveAmount, 0)
		Test.InjectAction("Jump", "Tap")
		Test.WaitTicks(1)
		Test.ExpectEqual(enemy.EmissiveStrength, 4, "the hit flashes white-hot")
		Test.WaitTicks(40)
		Test.ExpectEqual(enemy.DissolveAmount, 1, "half a second later it has dissolved")
		Test.ExpectEqual(enemy.EmissiveStrength, 0, "and the flash is over")
	end)
end)
