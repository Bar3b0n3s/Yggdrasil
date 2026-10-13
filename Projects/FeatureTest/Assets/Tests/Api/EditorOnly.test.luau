--!strict
-- The EditorOnly script API (§11.4): Test.ReloadScript, which exists only in the editor, so this suite runs only in the
-- Editor mode (Modes ["Editor"]). Callbacks.HotReload covers OnHotReload through it.
local ProbeScript = require("../../Scripts/Api/Probe")
local Support = require("./Support")

return Test.Suite("Api.EditorOnly", function()
	Test.Case("Test.ReloadScript reloads a behaviour in place and keeps its instances' state", function()
		Test.ExpectEqual(Application.IsEditor(), true, "EditorOnly suites run in the editor")
		local entity = Support.Find("Probe")
		local probe = entity:GetScript() :: ProbeScript.Probe
		Test.WaitTicks(2)
		local steps = probe.Steps
		probe.Title = "Before reload"
		Test.ReloadScript(Assets.Load("Assets/Scripts/Api/Probe.luau") :: AssetRef)
		Test.ExpectEqual(entity:GetScript(), probe, "the same instance table")
		Test.ExpectEqual(probe.Title, "Before reload", "instance state survives")
		Test.ExpectEqual(probe.Steps, steps)
		Test.ExpectEqual(probe.Starts, 1, "a reload does not restart the instance")
		Test.WaitTicks(1)
		Test.ExpectEqual(probe.Steps, steps + 1, "the reloaded class keeps running")
	end)
end)
