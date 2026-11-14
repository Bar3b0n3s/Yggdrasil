--!strict
-- The IrradianceVolume proxy (M26, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decisions 8, 16 and 25) in Probes.scene
-- (Probes.jsonl, IdSeed 2403), whose volume holds the bake lighting.bake committed beside the scene: the baked data loads
-- as an IrradianceVolume in every run mode (gate 9's .irradiance), the scene renders with it, and every field reads and
-- writes through the proxy.
local Support = require("./Support")

return Test.Suite("Api.IrradianceVolume", function()
	Test.Case("the committed bake loads as an IrradianceVolume and lights the scene", function()
		local volume = Support.Find("Volume").IrradianceVolume :: IrradianceVolume
		local data = volume.Data
		assert(data, "the volume holds its bake")
		local path = Assets.GetPath(data)
		Test.Expect(string.match(path, "^Assets/Lighting/Scenes/Probes/[^/]+%.irradiance$") ~= nil, `{path} is the bake's .irradiance`)
		Test.ExpectEqual(Assets.GetType(data), "IrradianceVolume")
		Test.ExpectEqual(Assets.Load(path), data)
		Test.WaitTicks(1)
		Test.Screenshot("Probes")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local volume = Support.Find("Volume").IrradianceVolume :: IrradianceVolume
		local data = volume.Data
		Test.ExpectEqual(Support.RoundTrip(volume, "Data", nil), data)
		volume.Data = data
		Test.ExpectNear(Support.RoundTrip(volume, "Size", vector.create(6, 3, 6), 1e-6), vector.create(4, 2, 4), 1e-6)
		Test.ExpectEqual(Support.RoundTrip(volume, "ProbeCountX", 5), 2)
		Test.ExpectEqual(Support.RoundTrip(volume, "ProbeCountY", 4), 2)
		Test.ExpectEqual(Support.RoundTrip(volume, "ProbeCountZ", 16), 3)
		Test.ExpectNear(Support.RoundTrip(volume, "Intensity", 1.5, 1e-6), 0.8, 1e-6)
		Test.ExpectNear(Support.RoundTrip(volume, "BlendDistance", 0.25, 1e-6), 0.5, 1e-6)
		Test.Expect(not pcall(function()
			volume.ProbeCountX = 1
		end), "a single probe along an axis is refused")
		Test.Expect(not pcall(function()
			volume.ProbeCountZ = 17
		end), "more than 16 probes along an axis are refused")
		Test.ExpectEqual(volume.ProbeCountX, 5, "a refused write leaves the field")
	end)
end)
