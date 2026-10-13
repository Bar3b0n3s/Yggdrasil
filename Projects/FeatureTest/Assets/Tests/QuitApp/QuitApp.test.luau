--!strict
-- Application.Quit(3) from gameplay code ends the suite at once with quit(3), which the suite's ExpectQuit 3 accepts
-- (§11.10, §15.5). The second case must never run.
local DoorScript = require("../../Scripts/QuitApp/Door")

return Test.Suite("QuitApp", function()
	Test.Case("the door quits with exit code 3 and the quit ends the suite", function()
		Test.ExpectEqual(Scene.GetName(), "QuitApp")
		local door = (Scene.FindByName("Door") :: Entity):GetScript() :: DoorScript.Door
		Test.ExpectEqual(door.ExitCode, 3, "the scene overrides the exit code")
		Test.WaitTicks(2)
		Test.ExpectEqual(door.Steps, 3)
		door.Opened = true
		-- The door quits in the next fixed step, which ends the suite before this wait returns.
		Test.WaitTicks(30)
		Test.Fail("Application.Quit(3) did not end the suite")
	end)

	Test.Case("no case runs after the quit", function()
		Test.Fail("the suite went on after Application.Quit(3)")
	end)
end)
