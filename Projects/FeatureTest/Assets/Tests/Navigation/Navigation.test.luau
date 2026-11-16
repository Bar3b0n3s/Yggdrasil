--!strict
-- Navigation.scene (M30; Docs/Decisions/0045-m30-navigation.md decisions 4, 5, 10 and 13): the arena navigation.bake baked
-- (Navigation.jsonl, IdSeed 2701), a floor with a pillar, a ramp up to a platform, a lava channel NavModifier that is not
-- walkable and a costly mud NavModifier, under a Navigation component of two agent types, Small (0.35 m) and Large
-- (0.9 m). The committed bake loads as a NavMesh in every run mode (gate 9's .navmesh), the scene holds every Navigation
-- and NavModifier field at a non-default value (gates 1 and 1b), and the cases below call every member of the Navigation
-- namespace on the session's mesh (gate 2). NavigationReplay.jsonl (IdSeed 2702) adds the Warden and the chasers that
-- Navigation.replay records.
local function insideLava(point: vector): boolean
	return math.abs(point.x + 3) < 7 and math.abs(point.z + 5) < 1
end

return Test.Suite("Navigation", function()
	Test.Case("the committed bake loads as a NavMesh", function()
		local mesh = Assets.Load("Assets/Navigation/Scenes/Navigation.navmesh")
		assert(mesh, "the scene's bake is committed")
		Test.ExpectEqual(Assets.GetType(mesh), "NavMesh")
		Test.ExpectEqual(Assets.GetPath(mesh), "Assets/Navigation/Scenes/Navigation.navmesh")
	end)

	Test.Case("FindPath goes around the lava channel's end and reaches its goal", function()
		for _, agent in { "Small", "Large" } do
			local path, complete = Navigation.FindPath(vector.create(-6, 0, -8), vector.create(-6, 0, -2), agent)
			assert(path, "both ends are on the mesh")
			Test.ExpectEqual(complete, true, agent)
			local around = false
			for index, corner in path do
				around = around or corner.x > 4
				if index > 1 then
					local previous = path[index - 1]
					for step = 0, 20 do
						Test.Expect(not insideLava(previous + (corner - previous) * (step / 20)), agent .. " never crosses the lava")
					end
				end
			end
			Test.Expect(around, agent .. " goes around the channel's +X end")
		end
	end)

	Test.Case("FindPath climbs the ramp to the platform, and an end off the mesh finds nothing", function()
		local path, complete = Navigation.FindPath(vector.create(-6, 0, 6), vector.create(6, 2, 6))
		assert(path, "both ends are on the mesh")
		Test.ExpectEqual(complete, true)
		Test.ExpectNear(path[#path].y, 2, 0.25)
		local none, reached = Navigation.FindPath(vector.create(0, 40, 0), vector.create(0, 0, 0))
		Test.ExpectEqual(none, nil)
		Test.ExpectEqual(reached, false)
	end)

	Test.Case("ClosestPoint keeps each agent type's clearance from the pillar", function()
		for _, case in { { Agent = "Small", Clearance = 0.35 }, { Agent = "Large", Clearance = 0.9 } } do
			local point = Navigation.ClosestPoint(vector.create(-4, 0, 0), 3, case.Agent)
			assert(point, case.Agent .. " has floor around the pillar")
			local outside = math.max(math.abs(point.x + 4) - 0.5, math.abs(point.z) - 0.5)
			Test.Expect(outside > case.Clearance - 0.25, case.Agent .. " keeps its clearance")
		end
		Test.ExpectEqual(Navigation.ClosestPoint(vector.create(0, 40, 0)), nil)
	end)

	Test.Case("Raycast stops at the pillar's clearance and crosses open floor", function()
		local blocked = Navigation.Raycast(vector.create(-8, 0, 0), vector.create(-1, 0, 0))
		assert(blocked, "the walk starts on the mesh")
		Test.ExpectEqual(blocked.Clear, false)
		Test.Expect(blocked.Point.x < -4.5 and blocked.Normal.x < -0.99, "it stops before the pillar, facing back")
		local open = Navigation.Raycast(vector.create(0, 0, -2), vector.create(0, 0, 2), "Large")
		assert(open, "the walk starts on the mesh")
		Test.ExpectEqual(open.Clear, true)
		Test.ExpectEqual(open.Fraction, 1)
	end)

	Test.Case("RandomPoint repeats with its generator's seed and stays within its radius", function()
		local first = Navigation.RandomPoint(vector.create(-6, 0, 2), 3, "Small", Random.New(11))
		local second = Navigation.RandomPoint(vector.create(-6, 0, 2), 3, "Small", Random.New(11))
		assert(first and second, "the floor around the center has area")
		Test.ExpectEqual(first, second)
		local dx, dz = first.x + 6, first.z - 2
		Test.Expect(dx * dx + dz * dz <= 9, "within the radius")
		local anywhere = Navigation.RandomPoint()
		assert(anywhere, "the session's stream draws a point of the mesh")
		Test.Expect(not insideLava(anywhere) or anywhere.y > 0.5, "never in the lava")
	end)
end)
