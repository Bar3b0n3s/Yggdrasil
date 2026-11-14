--!strict
-- The ReflectionProbe proxy (M26, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decisions 3, 8, 16 and 25) in
-- Probes.scene (Probes.jsonl, IdSeed 2403): a reflection probe's bake is an Environment, the .hdr lighting.bake committed
-- beside the scene, which loads in every run mode; every field reads and writes through the proxy.
local Support = require("./Support")

return Test.Suite("Api.ReflectionProbe", function()
	Test.Case("the committed bake loads as an Environment", function()
		local probe = Support.Find("Mirror").ReflectionProbe :: ReflectionProbe
		local data = probe.Data
		assert(data, "the probe holds its bake")
		local path = Assets.GetPath(data)
		Test.Expect(string.match(path, "^Assets/Lighting/Scenes/Probes/[^/]+%.hdr$") ~= nil, `{path} is the bake's .hdr`)
		Test.ExpectEqual(Assets.GetType(data), "Environment")
		Test.ExpectEqual(Assets.Load(path), data)
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local probe = Support.Find("Mirror").ReflectionProbe :: ReflectionProbe
		local data = probe.Data
		Test.ExpectEqual(Support.RoundTrip(probe, "Data", nil), data)
		probe.Data = data
		Test.ExpectNear(Support.RoundTrip(probe, "Size", vector.create(8, 4, 8), 1e-6), vector.create(4, 2, 4), 1e-6)
		Test.ExpectEqual(Support.RoundTrip(probe, "BoxProjection", true), false)
		Test.ExpectEqual(Support.RoundTrip(probe, "Resolution", 256), 64)
		Test.ExpectNear(Support.RoundTrip(probe, "Intensity", 2, 1e-6), 0.9, 1e-6)
		Test.ExpectNear(Support.RoundTrip(probe, "BlendDistance", 0.75, 1e-6), 0.5, 1e-6)
		Test.Expect(not pcall(function()
			probe.Resolution = 48
		end), "a capture resolution that is not a power of two is refused")
		Test.Expect(not pcall(function()
			probe.Resolution = 1024
		end), "a capture resolution above 512 is refused")
		Test.ExpectEqual(probe.Resolution, 256, "a refused write leaves the field")
	end)
end)
