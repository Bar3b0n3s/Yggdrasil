--!strict
-- The Terrain and TerrainCollider proxies (M26; Docs/Decisions/0037-m26-contract.md decisions 4, 21 and 26) in
-- Terrain.scene: every field read and written, and SampleHeight following the heightfield, the HeightScale and the
-- entity's transform. Each case starts from the scene as saved (Isolation "Case").
local Support = require("./Support")

return Test.Suite("Api.Terrain", function()
	Test.Case("every Terrain field reads and writes through the proxy", function()
		local terrain = Support.Find("Ground").Terrain :: Terrain
		local heightfield = terrain.Heightfield
		Test.Expect(heightfield ~= nil, "the imported heightfield")
		Test.ExpectEqual(Support.RoundTrip(terrain, "Heightfield", nil), heightfield)
		Test.ExpectEqual(terrain:SampleHeight(vector.zero), nil, "no heightfield, no surface")
		terrain.Heightfield = heightfield
		Test.ExpectEqual(Support.RoundTrip(terrain, "Size", vector.create(24, 40, 0)), vector.create(32, 32, 0))
		Test.ExpectEqual(Support.RoundTrip(terrain, "HeightScale", 3), 2)
		local splat = terrain.SplatMap
		Test.Expect(splat ~= nil, "the painted splat map")
		Test.ExpectEqual(Support.RoundTrip(terrain, "SplatMap", nil), splat)
		Test.ExpectEqual(Support.RoundTrip(terrain, "ReceiveShadows", true), false)
		Test.ExpectEqual(Support.RoundTrip(terrain, "LodBias", 0.5), 1.5)
		Test.Expect(not pcall(function()
			terrain.LodBias = 8
		end), "a LodBias above 4 is refused")
		local checker = terrain.Layers[1].BaseColorMap
		terrain.Layers = {
			{
				BaseColorMap = nil,
				NormalMap = checker,
				Tint = Color.New(0.25, 0.5, 0.75),
				Tiling = 8,
				Roughness = 0.5,
				Metallic = 0.25,
			},
		}
		local layers = terrain.Layers
		Test.ExpectEqual(#layers, 1)
		Test.ExpectEqual(layers[1].BaseColorMap, nil)
		Test.ExpectEqual(layers[1].NormalMap, checker)
		Support.ExpectColorNear(layers[1].Tint, Color.New(0.25, 0.5, 0.75), 1e-6, "the written tint")
		Test.ExpectEqual(layers[1].Tiling, 8)
		Test.ExpectEqual(layers[1].Roughness, 0.5)
		Test.ExpectEqual(layers[1].Metallic, 0.25)
		terrain.Layers = {}
		Test.ExpectEqual(#terrain.Layers, 0)
	end)

	Test.Case("SampleHeight follows the HeightScale and the entity's transform", function()
		local ground = Support.Find("Ground")
		local terrain = ground.Terrain :: Terrain
		-- An untouched point of the imported ramp (Terrain.test.luau): twice the HeightScale doubles its height, and
		-- moving the entity up a metre lifts it by that metre.
		local point = vector.create(-12, 0, 12)
		local height = terrain:SampleHeight(point) :: number
		terrain.HeightScale = 4
		Test.ExpectNear(terrain:SampleHeight(point) :: number, 2 * height, 1e-5, "twice the HeightScale")
		ground.Transform.Translation = vector.create(-16, 1, -16)
		Test.WaitTicks(1)
		Test.ExpectNear(terrain:SampleHeight(point) :: number, 2 * height + 1, 1e-5, "a metre higher")
		Test.ExpectEqual(terrain:SampleNormal(point), terrain:SampleNormal(point + vector.create(0, 7, 0)))
	end)

	Test.Case("every TerrainCollider field reads and writes through the proxy", function()
		local collider = Support.Find("Ground").TerrainCollider :: TerrainCollider
		Test.ExpectEqual(Support.RoundTrip(collider, "Layer", "Default"), "Track")
		Test.ExpectEqual(Support.RoundTrip(collider, "Friction", 0.25), Support.F32(0.8))
		Test.ExpectEqual(Support.RoundTrip(collider, "Restitution", 0.5), Support.F32(0.1))
		Test.Expect(not pcall(function()
			collider.Restitution = 2
		end), "a restitution above 1 is refused")
	end)
end)
