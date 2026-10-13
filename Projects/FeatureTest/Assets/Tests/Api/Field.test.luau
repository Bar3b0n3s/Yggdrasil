--!strict
-- The Field module (§11.2): every field kind's constructor, and the Probe behaviour's fields as Scripting.scene
-- overrides them.
local ProbeScript = require("../../Scripts/Api/Probe")
local Support = require("./Support")

return Test.Suite("Api.Field", function()
	Test.Case("every field constructor returns a descriptor", function()
		local descriptors = {
			Field.Number(),
			Field.Number(2, { Min = 0, Max = 5, Step = 0.5, Tooltip = "number" }),
			Field.Integer(4),
			Field.Bool(true),
			Field.String("text"),
			Field.Vector(vector.create(1, 2, 3)),
			Field.Color(Color.New(0, 1, 0)),
			Field.Quat(Quat.FromEuler(0, 90, 0)),
			Field.Entity(),
			Field.Asset("Prefab"),
			Field.Enum({ "A", "B" }, "B"),
			Field.Array(Field.Integer()),
		}
		Test.ExpectEqual(#descriptors, 12)
		for index, descriptor in descriptors do
			Test.ExpectEqual(typeof(descriptor), "FieldDescriptor", "descriptor " .. index)
		end
		Test.Expect(not pcall(function()
			Field.Enum({})
		end), "an enum needs choices")
		Test.Expect(not pcall(Field.Asset, "NotAType"), "an asset field needs an asset type")
	end)

	Test.Case("each field kind takes the scene's override", function()
		local probe = Support.Find("Probe"):GetScript() :: ProbeScript.Probe
		Test.ExpectEqual(probe.Speed, 4)
		Test.ExpectEqual(probe.Count, 7)
		Test.ExpectEqual(probe.Enabled, true)
		Test.ExpectEqual(probe.Title, "Scripted")
		Test.ExpectEqual(probe.Offset, vector.create(1, 2, 3))
		Support.ExpectColorNear(probe.Tint, Color.New(0.1, 0.2, 0.3), 1e-6, "Tint")
		Support.ExpectQuatNear(probe.Facing, Quat.FromEuler(0, 90, 0), 1e-6, "Facing")
		Test.ExpectEqual(probe.Target, Support.Find("Crate"))
		Test.ExpectEqual(probe.Clip, Assets.Load("Assets/Audio/Beep.sfx"))
		Test.ExpectEqual(probe.Mode, "Chase")
		Test.ExpectEqual(table.concat(probe.Waypoints, ","), "1,2.5,4")
	end)
end)
