--!strict
-- Terrain.scene (M26; Docs/Decisions/0037-m26-contract.md decisions 20, 21, 23, 25 and 26): the Ground's Terrain and
-- TerrainCollider keep their authored values in every run mode. Its heightfield is Ramp16.png converted by
-- terrain.importHeightmap, 17 x 17 samples 2 m apart over 32 m whose sample (x, z) is x * 4000 + z of 65535 of the 2 m
-- HeightScale, so a plane rising along +X, (4000 X + Z) / 65535 metres at the local point (X, Z); terrain.edit then
-- raised and smoothed a mound at the local point (4, 16), lowered a bowl at (10, 16), flattened a ledge to 1.2 m at
-- (24, 6) and painted layer 1 along X in the scaffold. Terrain:SampleHeight and SampleNormal read that surface, the
-- TerrainCollider's body is that surface, and the Ball rests in the bowl.
local Support = require("../Api/Support")

-- The Ground's origin: its local point (X, Z) is the world point (X - 16, Z - 16).
local ORIGIN = vector.create(-16, 0, -16)
local DOWN = vector.create(0, -1, 0)

-- The imported ramp's height at the world point `position`, where no stroke reached.
local function rampHeight(position: vector): number
	return (4000 * (position.x - ORIGIN.x) + (position.z - ORIGIN.z)) / 65535
end

local function ground(): Terrain
	return Support.Find("Ground").Terrain :: Terrain
end

return Test.Suite("Terrain", function()
	Test.Case("the authored terrain and its collider keep their values", function()
		local entity = Support.Find("Ground")
		local terrain = entity.Terrain :: Terrain
		Test.ExpectEqual(terrain.Heightfield, Assets.Load("Assets/Heightfields/Terrain/Ramp16.heightfield"))
		Test.ExpectEqual(terrain.Size, vector.create(32, 32, 0))
		Test.ExpectEqual(terrain.HeightScale, 2)
		local layers = terrain.Layers
		Test.ExpectEqual(#layers, 2)
		Test.Expect(layers[1].BaseColorMap ~= nil and layers[1].NormalMap ~= nil, "the first layer's maps")
		Support.ExpectColorNear(layers[1].Tint, Color.New(0.45, 0.62, 0.32), 1e-6, "the first layer's tint")
		Test.ExpectEqual(layers[1].Tiling, 3)
		Test.ExpectEqual(layers[1].Roughness, Support.F32(0.85))
		Test.ExpectEqual(layers[1].Metallic, Support.F32(0.05))
		Test.Expect(layers[2].BaseColorMap == nil and layers[2].NormalMap == nil, "the second layer is its tint")
		Test.ExpectEqual(layers[2].Tiling, 6)
		Test.Expect(terrain.SplatMap ~= nil, "the first paint made the splat map and assigned it")
		Test.ExpectEqual(terrain.ReceiveShadows, false)
		Test.ExpectEqual(terrain.LodBias, 1.5)
		local collider = entity.TerrainCollider :: TerrainCollider
		Test.ExpectEqual(collider.Layer, "Track")
		Test.ExpectEqual(collider.Friction, Support.F32(0.8))
		Test.ExpectEqual(collider.Restitution, Support.F32(0.1))
	end)

	Test.Case("SampleHeight reads the imported ramp and the strokes' samples", function()
		local terrain = ground()
		-- Where no stroke reached, the surface is the ramp, between samples too: a plane, so both triangles of a cell lie
		-- in it. A point's height (its Y) does not matter.
		for _, position in { vector.create(-12, 0, 12), vector.create(-11, 5, 11), vector.create(-14.5, -3, 13.25) } do
			Test.ExpectNear(terrain:SampleHeight(position) :: number, rampHeight(position), 1e-5, "the ramp")
		end
		-- The Flatten dab's centre took the stroke's height, and the Lower dab's centre sank by its half metre; the Raise
		-- stroke's two dabs lifted the mound's top to the HeightScale, which the Smooth dab then pulled towards its
		-- neighbours.
		Test.ExpectNear(terrain:SampleHeight(vector.create(8, 0, -10)) :: number, 1.2, 1e-5, "the ledge")
		local bowl = vector.create(-6, 0, 0)
		Test.ExpectNear(terrain:SampleHeight(bowl) :: number, rampHeight(bowl) - 0.5, 1e-4, "the bowl's centre")
		local mound = vector.create(-12, 0, 0)
		local top = terrain:SampleHeight(mound) :: number
		Test.Expect(top > rampHeight(mound) + 0.5 and top < 2, "the smoothed mound")
		-- Outside the terrain's extent there is no surface.
		Test.ExpectEqual(terrain:SampleHeight(vector.create(16.5, 0, 0)), nil)
		Test.ExpectEqual(terrain:SampleNormal(vector.create(0, 0, -16.5)), nil)
	end)

	Test.Case("SampleNormal is the surface's unit normal", function()
		-- The ramp rises 4000 / 65535 m per metre along X and 1 / 65535 m along Z.
		local expected = vector.normalize(vector.create(-4000 / 65535, 1, -1 / 65535))
		Test.ExpectNear(ground():SampleNormal(vector.create(-12, 2, 12)) :: vector, expected, 1e-5, "the ramp's normal")
	end)

	Test.Case("the ball rests in the bowl on the surface, and rays hit the terrain's body there", function()
		local terrain = ground()
		local ball = Support.Find("Ball")
		Test.WaitTicks(120)
		local centre = ball.Transform.WorldPosition
		Test.Expect(centre.x > -10 and centre.x < -4, "the ball stays in the bowl")
		-- The sphere's centre lies its radius from the plane of the surface under it, within the penetration slop.
		local height = terrain:SampleHeight(centre) :: number
		local normal = terrain:SampleNormal(centre) :: vector
		Test.ExpectNear(vector.dot(centre - vector.create(centre.x, height, centre.z), normal), 0.5, 0.02, "the ball touches the surface")
		local probe = vector.create(10, 5, 10)
		local hit = Physics.Raycast(probe, DOWN, 20)
		Test.Expect(hit ~= nil, "the ray down hits the terrain")
		Test.ExpectEqual((hit :: RaycastHit).Entity, Support.Find("Ground"))
		Test.ExpectNear((hit :: RaycastHit).Point.y, rampHeight(probe), 0.005, "on the drawn surface")
	end)
end)
