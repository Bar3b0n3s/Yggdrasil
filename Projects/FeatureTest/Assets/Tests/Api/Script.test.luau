--!strict
-- The Script module and the Script component proxy (§11.2, §11.4): Script.Define authenticates a class once, and the
-- component's Script, Fields and ExecutionOrder read and write; GetScript reaches the instance.
local ProbeScript = require("../../Scripts/Api/Probe")
local Support = require("./Support")

return Test.Suite("Api.Script", function()
	Test.Case("Script.Define returns its class and authenticates it once", function()
		local class = { Fields = { Value = Field.Integer(1) } }
		Test.ExpectEqual(Script.Define("ApiDefined", class), class)
		Test.Expect(not pcall(Script.Define, "ApiDefinedAgain", class), "a class is defined once")
	end)

	Test.Case("the Script component's fields read and write through the proxy", function()
		local entity = Support.Find("Probe")
		local component = entity:GetComponent("Script") :: Script
		local script = Assets.Load("Assets/Scripts/Api/Probe.luau") :: AssetRef
		Test.ExpectEqual(component.Script, script)
		Test.ExpectEqual(component.ExecutionOrder, 3)
		local fields = component.Fields
		Test.ExpectEqual(fields.Speed, 4)
		Test.ExpectEqual(fields.Mode, "Chase")
		Test.ExpectEqual(fields.Title, "Scripted")
		Support.RoundTrip(component, "ExecutionOrder", -2)
		component.Fields = { Speed = 6, Mode = "Idle" }
		Test.ExpectEqual(component.Fields.Speed, 6)
		Test.ExpectEqual(component.Fields.Mode, "Idle")
		Test.ExpectEqual(component.Fields.Title, nil, "the written map replaces the overrides")
		Support.RoundTrip(component, "Script", script)
		local instance = entity:GetScript() :: ProbeScript.Probe
		Test.Expect(instance ~= nil and instance.Created, "the instance exists")
	end)

	Test.Case("a script attached at run time creates its instance with the given overrides", function()
		local entity = Scene.CreateEntity("LateProbe")
		local component = entity:AddComponent("Script", {
			Script = Assets.Load("Assets/Scripts/Api/Probe.luau"),
			Fields = { Count = 9, Title = "Late" },
			ExecutionOrder = 1,
		}) :: Script
		Test.ExpectEqual(component.ExecutionOrder, 1)
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
