--!strict
-- Recording suite of Assets/Tests/Replays/Level1.replay: the autopilot drives Level 1 to its goal under par.
local Autopilot = require("../Lib/Autopilot")

local ParTicks = 1800

return Test.Suite("Autopilot.Level1", function()
	Test.Case("reaches the goal under par", function()
		Autopilot.Case(ParTicks)
	end, { TimeoutTicks = ParTicks + 60 })
end)
