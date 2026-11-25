--!strict
-- MeshLods.scene (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3M; MeshLods.jsonl, IdSeed
-- 4301): LodTree.gltf's tree with its authored MSFT_lod levels, LodRock.gltf's rock grouped from its _LOD<n> nodes and
-- LodSphere.gltf's ball with generated levels, placed along the Primary camera's view, each with a RenderDistance, so that
-- across them every field is off its default. TreeBand sits in the middle of its first switch's fade band, RockFading
-- fades out before its CullDistance and RockCulled lies beyond it. Every run mode reads and writes the four fields through
-- the proxy; where the game view renders (the exported Release and Dist runs, headless), the view cross-fades one item
-- and culls one by distance, and a cull distance moved in or a band closed changes those counts.
local Support = require("../Api/Support")

local function Distance(name: string): RenderDistance
	return Support.Find(name).RenderDistance :: RenderDistance
end

-- Waits until the game view has rendered the latest state, and returns the readout.
local function RenderedStats(): PerformanceStats
	Test.WaitTicks(4)
	return Application.GetStats()
end

-- The view's counts: one cross-fade and one item culled by distance as authored, none without a renderer.
local function ExpectCounts(stats: PerformanceStats, crossFades: number, culled: number, message: string)
	if stats.RenderAvailable then
		Test.ExpectEqual(stats.LodCrossFades, crossFades, `{message}: items drawn cross-fading two levels`)
		Test.ExpectEqual(stats.DistanceCulledMeshes, culled, `{message}: items beyond their cull distance`)
	else
		Test.ExpectEqual(stats.LodCrossFades, 0, "nothing is drawn without a renderer")
		Test.ExpectEqual(stats.DistanceCulledMeshes, 0, "nothing is culled without a renderer")
	end
end

return Test.Suite("MeshLods", function()
	Test.Case("the authored distances read back, and the view cross-fades one item and culls one", function()
		local near = Distance("TreeNear")
		Test.ExpectNear(near.CullDistance, 400, 1e-4)
		Test.ExpectNear(near.LodBias, 1, 1e-6)
		Test.ExpectEqual(near.ShadowLodOffset, 0)
		Test.ExpectNear(near.FadeWidth, 0.1, 1e-6)
		local band = Distance("TreeBand")
		Test.ExpectNear(band.LodBias, 1.25, 1e-6)
		Test.ExpectNear(band.FadeWidth, 0.3, 1e-6)
		local far = Distance("TreeFar")
		Test.ExpectNear(far.LodBias, 0.5, 1e-6)
		Test.ExpectEqual(far.ShadowLodOffset, 2)
		local fading = Distance("RockFading")
		Test.ExpectNear(fading.CullDistance, 25, 1e-4)
		Test.ExpectNear(fading.FadeWidth, 0.2, 1e-6)
		Test.ExpectNear(Distance("RockCulled").CullDistance, 35, 1e-4)
		Test.ExpectNear(Distance("Ball").LodBias, 0.8, 1e-6)
		-- RockMid's RenderDistance keeps its defaults: no cull distance, the bias 1, shadows one level coarser.
		local plain = Distance("RockMid")
		Test.ExpectEqual(plain.CullDistance, 0)
		Test.ExpectNear(plain.LodBias, 1, 1e-6)
		Test.ExpectEqual(plain.ShadowLodOffset, 1)
		ExpectCounts(RenderedStats(), 1, 1, "as authored")
		Test.Screenshot("MeshLods")
	end)

	Test.Case("every field reads and writes through the proxy, and out-of-range values are refused", function()
		local distance = Distance("RockMid")
		Support.RoundTrip(distance, "CullDistance", 120, 1e-4)
		Support.RoundTrip(distance, "CullDistance", 0, 1e-6)
		Support.RoundTrip(distance, "LodBias", 3, 1e-6)
		Support.RoundTrip(distance, "LodBias", 1, 1e-6)
		Support.RoundTrip(distance, "ShadowLodOffset", 4)
		Support.RoundTrip(distance, "ShadowLodOffset", 1)
		Support.RoundTrip(distance, "FadeWidth", 0.45, 1e-6)
		Support.RoundTrip(distance, "FadeWidth", 0.1, 1e-6)
		Test.Expect(not pcall(function()
			distance.LodBias = 0
		end), "a LodBias below 0.01 is refused")
		Test.ExpectNear(distance.LodBias, 1, 1e-6, "a refused LodBias keeps the field")
		Test.Expect(not pcall(function()
			distance.FadeWidth = 0.75
		end), "a FadeWidth above 0.5 is refused")
		Test.Expect(not pcall(function()
			distance.ShadowLodOffset = 5
		end), "a ShadowLodOffset above 4 is refused")
		Test.Expect(not pcall(function()
			distance.CullDistance = -1
		end), "a negative CullDistance is refused")
		Test.ExpectEqual(distance.CullDistance, 0, "a refused CullDistance keeps the field")
	end)

	Test.Case("a cull distance moved in culls one more item, and a closed band cross-fades none", function()
		ExpectCounts(RenderedStats(), 1, 1, "before the change")
		local far = Distance("TreeFar")
		local band = Distance("TreeBand")
		-- TreeFar is about 30 m from the camera, so a 20 m cull distance culls it; a FadeWidth of 0 switches sharply.
		far.CullDistance = 20
		band.FadeWidth = 0
		ExpectCounts(RenderedStats(), 0, 2, "after the change")
		Test.Screenshot("MeshLodsCulled")
		far.CullDistance = 0
		band.FadeWidth = 0.3
		ExpectCounts(RenderedStats(), 1, 1, "after the restore")
	end)
end)
