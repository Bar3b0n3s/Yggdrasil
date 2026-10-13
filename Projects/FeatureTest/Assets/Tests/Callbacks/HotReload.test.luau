--!strict
-- OnHotReload through Test.ReloadScript, the editor's hot-reload path (§11.8). Hot reload exists only in the editor, so
-- this suite runs only in the Editor mode (Modes ["Editor"]); both members are EditorOnly.
local Journal = require("../../Scripts/Callbacks/Journal")
local ReloadableScript = require("../../Scripts/Callbacks/Reloadable")

return Test.Suite("Callbacks.HotReload", function()
	Test.Case("a reload keeps instance state, runs OnHotReload once and keeps running the class", function()
		local entity = Scene.FindByName("Reloadable") :: Entity
		local instance = entity:GetScript() :: ReloadableScript.Reloadable
		Test.Expect(instance ~= nil, "the Reloadable behaviour runs")
		-- The scene overrides the Step field's default 1 with 2.
		Test.ExpectEqual(instance.Step, 2)
		Test.WaitTicks(3)
		local counter = instance.Counter
		Test.Expect(counter > 0, "the instance counted its fixed steps")
		local cursor = Journal.Count()
		Test.ReloadScript(Assets.Load("Assets/Scripts/Callbacks/Reloadable.luau") :: AssetRef)
		Test.ExpectEqual(instance.Reloads, 1)
		Test.ExpectEqual(instance.Counter, counter, "the reload keeps the instance's state")
		Test.ExpectEqual(entity:GetScript(), instance, "the instance table survives the reload")
		Test.ExpectEqual(Journal.Since(cursor)[1], "Reloadable:hot-reload")
		Test.WaitTicks(2)
		Test.ExpectEqual(instance.Counter, counter + 2 * instance.Step, "the reloaded class keeps updating the instance")
		Test.ExpectEqual(instance.Reloads, 1)
	end)

	Test.Case("each reload runs OnHotReload again", function()
		local instance = (Scene.FindByName("Reloadable") :: Entity):GetScript() :: ReloadableScript.Reloadable
		local before = instance.Reloads
		local script = Assets.Load("Assets/Scripts/Callbacks/Reloadable.luau") :: AssetRef
		-- A reload's reimport and type check are host time, outside the case's callback budget (ScriptEngine::RunHostOperation);
		-- only the reload itself, which runs the new code, counts. The second reload follows a tick later, after the instance
		-- kept updating.
		Test.ReloadScript(script)
		Test.ExpectEqual(instance.Reloads, before + 1)
		Test.WaitTicks(1)
		Test.ReloadScript(script)
		Test.ExpectEqual(instance.Reloads, before + 2)
	end)
end)
