--!strict
-- Recording suite of Assets/Tests/Replays/Level2.replay: the autopilot drives Level 2 to its goal under par, with the
-- carried timer in the suite's Parameters.
local Autopilot = require("../Lib/Autopilot")

local ParTicks = 2700

return Test.Suite("Autopilot.Level2", function()
	Test.Case("reaches the goal under par", function()
		Autopilot.Case(ParTicks)
	end, { TimeoutTicks = ParTicks + 60 })
end)
