--!strict
-- The Script module and the Script component proxy (§11.2, §11.4): Script.Define authenticates a class once, and the
-- component's list of script slots, Scripts, each with its Script, Fields and ExecutionOrder, reads and writes; GetScript
-- reaches the instance, which a write that keeps the slot's script keeps.
local ProbeScript = require("../../Scripts/Api/Probe")
local Support = require("./Support")

return Test.Suite("Api.Script", function()
	Test.Case("Script.Define returns its class and authenticates it once", function()
		local class = { Fields = { Value = Field.Integer(1) } }
		Test.ExpectEqual(Script.Define("ApiDefined", class), class)
		Test.Expect(not pcall(Script.Define, "ApiDefinedAgain", class), "a class is defined once")
	end)

	Test.Case("the Script component's slots read and write through the proxy", function()
		local entity = Support.Find("Probe")
		local component = entity:GetComponent("Script") :: Script
		local script = Assets.Load("Assets/Scripts/Api/Probe.luau") :: AssetRef
		local slots = component.Scripts
		Test.ExpectEqual(#slots, 1)
		Test.ExpectEqual(slots[1].Script, script)
		Test.ExpectEqual(slots[1].ExecutionOrder, 3)
		local fields = slots[1].Fields
		Test.ExpectEqual(fields.Speed, 4)
		Test.ExpectEqual(fields.Mode, "Chase")
		Test.ExpectEqual(fields.Title, "Scripted")
		local instance = entity:GetScript() :: ProbeScript.Probe
		Test.Expect(instance ~= nil and instance.Created, "the instance exists")
		component.Scripts = { { Script = script, Fields = { Speed = 6, Mode = "Idle" }, ExecutionOrder = -2 } }
		local written = component.Scripts
		Test.ExpectEqual(#written, 1)
		Test.ExpectEqual(written[1].Script, script)
		Test.ExpectEqual(written[1].ExecutionOrder, -2)
		Test.ExpectEqual(written[1].Fields.Speed, 6)
		Test.ExpectEqual(written[1].Fields.Mode, "Idle")
		Test.ExpectEqual(written[1].Fields.Title, nil, "the written slot replaces its overrides")
		Test.ExpectEqual(entity:GetScript(), instance, "a slot that keeps its script keeps its instance")
	end)

	Test.Case("a script attached at run time creates its instance with the given overrides", function()
		local entity = Scene.CreateEntity("LateProbe")
		local component = entity:AddComponent("Script", {
			Scripts = {
				{ Script = Assets.Load("Assets/Scripts/Api/Probe.luau"), Fields = { Count = 9, Title = "Late" }, ExecutionOrder = 1 },
			},
		}) :: Script
		Test.ExpectEqual(component.Scripts[1].ExecutionOrder, 1)
		local instance = entity:GetScript() :: ProbeScript.Probe
		Test.Expect(instance ~= nil and instance.Created, "OnCreate ran inside AddComponent")
		Test.ExpectEqual(instance.Count, 9)
		Test.ExpectEqual(instance.Title, "Late")
		Test.ExpectEqual(instance.Speed, 1.5, "fields without an override keep their defaults")
		Test.ExpectEqual(instance.Starts, 0, "OnStart waits for the next start flush")
		Test.WaitTicks(1)
		Test.ExpectEqual(instance.Starts, 1)
	end)
end)
