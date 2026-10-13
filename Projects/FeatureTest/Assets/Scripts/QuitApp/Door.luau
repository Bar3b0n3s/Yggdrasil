--!strict
-- Quits the application with ExitCode in the fixed step after it is opened: gameplay code ending a run, which in a
-- test run ends the running suite with a recorded quit(ExitCode) (§11.10).
export type Door = ScriptInstance & {
	ExitCode: number,
	Opened: boolean,
	Steps: number,
}

local Door = {}

Door.Fields = {
	ExitCode = Field.Integer(0, { Min = 0, Max = 255, Tooltip = "The exit code Application.Quit receives" }),
}

function Door.OnCreate(self: Door)
	self.Opened = false
	self.Steps = 0
end

function Door.OnFixedUpdate(self: Door, dt: number)
	self.Steps += 1
	if self.Opened then
		Application.Quit(self.ExitCode)
	end
end

return Script.Define("Door", Door)
