--!strict
-- PlanarReflections.scene (M42; Docs/Decisions/0059-m42-planar-reflections.md decision 3; PlanarReflections.jsonl, IdSeed
-- 3901): five mirrors, each a flat glass with a PlanarReflection on its own entity: a Quad facing +Z, a thin box whose
-- Normal names its local X face (Offset 0.5), a Plane whose Normal is its local Y, a small Quad, and a thin box reflecting
-- from its back face (Normal -Z, Offset 0.5) turned to face the camera, so across them every field is off its default
-- (ResolutionScale 0.25 on the first and 0.75 on the third). Every run mode reads and writes the three fields through the
-- proxy and turns a mirror from a task; where the game view renders (the exported Release and Dist runs, headless), the
-- view draws four of the five, the largest on screen, drops the smallest, and stops counting a mirror turned away.
local Support = require("../Api/Support")

local MirrorNames = { "MirrorQuad", "MirrorSlabX", "MirrorPlaneY", "MirrorSmall", "MirrorSlabBack" }

local function Reflection(name: string): PlanarReflection
	return Support.Find(name).PlanarReflection :: PlanarReflection
end

local function ExpectVectorNear(actual: vector, expected: vector, message: string)
	Test.Expect(vector.magnitude(actual - expected) < 1e-5, `{message}: {actual} is {expected}`)
end

-- Waits until the game view has rendered the latest state, and returns the readout.
local function RenderedStats(): PerformanceStats
	Test.WaitTicks(4)
	return Application.GetStats()
end

return Test.Suite("PlanarReflections", function()
	Test.Case("the authored mirrors read back, each with its plane on its glass", function()
		ExpectVectorNear(Reflection("MirrorQuad").Normal, vector.create(0, 0, 1), "the Quad's face")
		Test.ExpectNear(Reflection("MirrorQuad").ResolutionScale, 0.25, 1e-6)
		ExpectVectorNear(Reflection("MirrorSlabX").Normal, vector.create(1, 0, 0), "the slab's X face")
		Test.ExpectNear(Reflection("MirrorSlabX").Offset, 0.5, 1e-6)
		ExpectVectorNear(Reflection("MirrorPlaneY").Normal, vector.create(0, 1, 0), "the Plane's face")
		Test.ExpectNear(Reflection("MirrorPlaneY").ResolutionScale, 0.75, 1e-6)
		ExpectVectorNear(Reflection("MirrorSlabBack").Normal, vector.create(0, 0, -1), "the slab's back face")
		Test.ExpectNear(Reflection("MirrorSlabBack").Offset, 0.5, 1e-6)
		Test.Screenshot("PlanarReflections")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local reflection = Reflection("MirrorSmall")
		reflection.Normal = vector.create(0, 0, -1)
		ExpectVectorNear(reflection.Normal, vector.create(0, 0, -1), "Normal reads back what was written")
		reflection.Normal = vector.create(0, 0, 1)
		Support.RoundTrip(reflection, "Offset", 0.25, 1e-6)
		Support.RoundTrip(reflection, "Offset", 0, 1e-6)
		Support.RoundTrip(reflection, "ResolutionScale", 1, 1e-6)
		Support.RoundTrip(reflection, "ResolutionScale", 0.5, 1e-6)
		Test.Expect(not pcall(function()
			reflection.Normal = vector.create(0, 0, 0)
		end), "a zero Normal is refused")
		ExpectVectorNear(reflection.Normal, vector.create(0, 0, 1), "a refused Normal keeps the field")
		Test.Expect(not pcall(function()
			reflection.ResolutionScale = 2
		end), "a ResolutionScale above 1 is refused")
	end)

	Test.Case("a task turns a mirror, and the view draws at most four with the smallest dropped", function()
		local before = RenderedStats()
		if before.RenderAvailable then
			Test.ExpectEqual(before.PlanarReflections, 4, "four of the five mirrors in view are drawn")
			Test.ExpectEqual(before.DroppedPlanarReflections, 1, "the smallest falls back")
		else
			Test.ExpectEqual(before.PlanarReflections, 0, "nothing is drawn without a renderer")
		end
		-- The aurora mirror's way: a task turns the Quad a little each tick until it faces away.
		local mirror = Support.Find("MirrorQuad")
		local turned = 0
		Task.Spawn(function()
			for _ = 1, 12 do
				mirror.Transform:Rotate(vector.create(0, 15, 0))
				turned += 15
				Task.WaitTicks(1)
			end
		end)
		Test.WaitTicks(12)
		Test.ExpectEqual(turned, 180, "the task turned the mirror half a turn")
		local after = RenderedStats()
		if after.RenderAvailable then
			Test.ExpectEqual(after.PlanarReflections, 4, "the four other mirrors are drawn")
			Test.ExpectEqual(after.DroppedPlanarReflections, 0, "the turned mirror is culled, not dropped")
		end
		Test.Screenshot("PlanarReflectionsTurned")
	end)
end)
