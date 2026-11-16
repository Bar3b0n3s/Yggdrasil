--!strict
-- M28's game services (Docs/Decisions/0043-m28-game-services.md decisions 3 to 6) in GameServices.scene (GameServices.jsonl,
-- IdSeed 2501), in every run mode: the display settings, the performance readout, presentation text and frame-time
-- signals. Each case runs on a fresh load of the scene.
local HudScript = require("../../Scripts/GameServices/Hud")

local function hud(): Entity
	local entity = Scene.FindByName("Hud")
	assert(entity, "the scene holds Hud")
	return entity
end

local function hudText(): UiText
	local text = hud().UiText
	assert(text, "Hud has a UiText")
	return text
end

local function hudScript(): HudScript.Hud
	return hud():GetScript("Hud") :: HudScript.Hud
end

return Test.Suite("GameServices", function()
	Test.Case("the performance readout reports the session's counters, and the fixed step cannot read it", function()
		Test.WaitTicks(2)
		local stats = Application.GetStats()
		Test.ExpectEqual(stats.Entities, Scene.GetEntityCount())
		Test.ExpectEqual(stats.Bodies, 1)
		Test.Expect(stats.ScriptHeapBytes > 0, "the script heap is counted")
		Test.ExpectEqual(type(stats.RenderAvailable), "boolean")
		Test.ExpectEqual(type(stats.Fps), "number")
		Test.ExpectEqual(hudScript().StatsRefused, true)
	end)

	Test.Case("presentation text shows a frame-paced value without reaching the state hash", function()
		local text = hudText()
		local hash = Test.GetStateHash()
		text:SetPresentationText("16.7 ms")
		Test.ExpectEqual(text:GetPresentationText(), "16.7 ms")
		Test.ExpectEqual(text.Text, "Wave 1")
		Test.ExpectEqual(Test.GetStateHash(), hash)
		text:SetPresentationText(nil)
		Test.ExpectEqual(text:GetPresentationText(), nil)
	end)

	Test.Case("a test run ignores live signals and takes injected ones, which end the run", function()
		Test.ExpectEqual(Input.PostSignal("Live"), false)
		Test.InjectSignal("FrameTimeExceeded")
		Test.WaitUntil(function()
			return hudText().Text == "Run ended"
		end, 10)
		Test.ExpectEqual(hudScript().Ended, true)
		Test.ExpectEqual(Input.WasSignalled("Live"), false)
	end)

	Test.Case("display settings do nothing in a test run: no modes, the project's window, and a refused request", function()
		Test.ExpectEqual(#Application.GetDisplayModes(), 0)
		local display = Application.GetDisplaySettings()
		Test.ExpectEqual(display.WindowMode, "Windowed")
		Test.ExpectEqual(display.Width, 1280)
		Test.ExpectEqual(display.Height, 720)
		Test.ExpectEqual(display.RefreshRate, 0)
		Test.ExpectEqual(display.VSync, true)
		local accepted, reason = Application.SetDisplaySettings({ WindowMode = "Borderless", VSync = false })
		Test.ExpectEqual(accepted, false)
		Test.Expect(type(reason) == "string" and #reason > 0, "the refusal names its reason")
		Test.ExpectEqual(Application.GetDisplaySettings().VSync, true)
		-- A malformed request raises in a test run too.
		Test.ExpectEqual((pcall(function()
			Application.SetDisplaySettings({ WindowMode = "Fullscreen" })
		end)), false)
		Test.ExpectEqual((pcall(function()
			Application.SetDisplaySettings({ Width = 0 })
		end)), false)
	end)
end)
