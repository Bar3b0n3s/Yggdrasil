--!strict
-- Observes the suite settings of Tests/Data/Project/AllSettings.eproj through the runner (gate 8). Every case passes only
-- when its setting reached the session: the Scene, the Parameters, the scripted Clock (fewer frames than ticks), case
-- Isolation, CaseTimeoutTicks and SuiteTickLimit (the long cases outlast both defaults) and ExpectQuit (the last case).
local LongCases = 31
local LongTicks = 1180

return Test.Suite("Main", function()
	Test.Case("the suite runs in its scene", function()
		Test.Expect(Scene.FindByName("Ball") ~= nil, "Main.scene has a Ball")
	end)

	Test.Case("parameters reach the scene", function()
		local parameters = Scene.GetLoadParameters()
		Test.ExpectEqual(parameters.Level, 2)
		Test.ExpectNear(parameters.Time :: number, 61.25)
	end)

	Test.Case("a scripted clock does not step once per frame", function()
		local frame, tick = Time.GetFrameCount(), Time.GetTick()
		Test.WaitTicks(10)
		Test.Expect(Time.GetFrameCount() - frame < Time.GetTick() - tick, "fewer frames than ticks")
	end)

	Test.Case("case isolation restarts the scene", function()
		Test.Expect(Scene.FindByName("Marker") == nil, "no marker survives an earlier case")
		Scene.CreateEntity("Marker")
	end)

	Test.Case("case isolation restarts the scene again", function()
		Test.Expect(Scene.FindByName("Marker") == nil, "no marker survives an earlier case")
		Scene.CreateEntity("Marker")
	end)

	for index = 1, LongCases do
		Test.Case("a long case " .. tostring(index), function()
			Test.WaitTicks(LongTicks)
		end)
	end

	Test.Case("the suite quits with its expected code", function()
		Application.Quit(3)
	end)
end)
