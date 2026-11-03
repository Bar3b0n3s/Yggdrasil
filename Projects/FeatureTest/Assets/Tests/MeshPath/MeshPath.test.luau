--!strict
-- The 3D mesh path of M23 in every run mode (Docs/Decisions/0034-m23-contract.md decisions 2 to 5 and 17; IdSeed
-- 2101): MeshPath.scene draws a grid of 64 crates and 16 balls, each kind one mesh with one material, so every pass
-- batches each kind into one instanced draw; four shadow-casting point lights, the per-view budget; and 24 more point
-- lights and four spot lights that the light clusters hold per 64-pixel tile and depth slice. The suite checks the
-- authored scene, then renders it through Test.Screenshot, again after a shadow turned off and lights moved between
-- clusters through the proxies.

local CRATE_MATERIAL = "Assets/Materials/MeshPath/Crate.material"
local BALL_MATERIAL = "Assets/Materials/MeshPath/Ball.material"

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `MeshPath.scene has an entity named {name}`)
	return entity
end

-- Every child of `parent` draws `mesh` with `material` in its only slot; returns how many children there are.
local function expectInstancedKind(parent: string, mesh: string, material: string): number
	local children = find(parent):GetChildren()
	for _, child in children do
		local renderer = child.MeshRenderer
		assert(renderer, `{child.Name} has a MeshRenderer`)
		local drawn = renderer.Mesh
		assert(drawn, `{child.Name} draws a mesh`)
		Test.ExpectEqual(Assets.GetPath(drawn), mesh, `{child.Name}'s mesh`)
		local slot = renderer:GetMaterial(1)
		assert(slot, `{child.Name} has a material`)
		Test.ExpectEqual(Assets.GetPath(slot), material, `{child.Name}'s material`)
		Test.Expect(renderer.Visible and renderer.CastShadows and renderer.ReceiveShadows, `{child.Name} draws, casts and receives`)
	end
	return #children
end

return Test.Suite("MeshPath", function()
	Test.Case("each kind of the grid shares one mesh and one material", function()
		Test.ExpectEqual(expectInstancedKind("Crates", "engine://Meshes/Cube", CRATE_MATERIAL), 64, "crates")
		Test.ExpectEqual(expectInstancedKind("Balls", "engine://Meshes/Sphere", BALL_MATERIAL), 16, "balls")
	end)

	Test.Case("four point lights cast shadows, the per-view budget, and the other lights none", function()
		local shadowed, unshadowed, spots = 0, 0, 0
		for _, lamp in find("Lamps"):GetChildren() do
			local point = lamp.PointLight
			local spot = lamp.SpotLight
			if point then
				if point.CastShadows then
					shadowed += 1
					Test.Expect(string.sub(lamp.Name, 1, 10) == "ShadowLamp", `{lamp.Name} casts shadows`)
				else
					unshadowed += 1
				end
			elseif spot then
				spots += 1
				Test.ExpectEqual(spot.CastShadows, false, `{lamp.Name} casts no shadow`)
			end
		end
		Test.ExpectEqual(shadowed, 4, "shadow-casting point lights")
		Test.ExpectEqual(unshadowed, 24, "point lights without shadows")
		Test.ExpectEqual(spots, 4, "spot lights")
	end)

	Test.Case("the scene renders instanced, with point shadows and clustered lights", function()
		Test.WaitTicks(2)
		Test.Screenshot("MeshPath")
	end)

	Test.Case("a shadow turned off and lights moved between clusters render again", function()
		local lamp = find("ShadowLamp1").PointLight
		assert(lamp, "ShadowLamp1 has a PointLight")
		lamp.CastShadows = false
		Test.ExpectEqual(lamp.CastShadows, false, "ShadowLamp1's shadow is off")
		local moved = 0
		for _, child in find("Lamps"):GetChildren() do
			local point = child.PointLight
			if point and not point.CastShadows then
				local transform = child.Transform
				transform.Translation += vector.create(1.5, 0, 0)
				moved += 1
			end
		end
		Test.ExpectEqual(moved, 25, "the unshadowed point lights moved")
		Test.WaitTicks(1)
		Test.Screenshot("MeshPathMoved")
		Test.ExpectNear(find("Lamp01").Transform.Translation, vector.create(-6, 0.9, -6))
	end)
end)
