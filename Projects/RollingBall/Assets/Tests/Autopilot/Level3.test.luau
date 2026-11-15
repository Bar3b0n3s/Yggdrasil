--!strict
-- Recording suite of Assets/Tests/Replays/Level3.replay: the autopilot drives Level 3 to its goal under par, with the
-- carried timer in the suite's Parameters.
local Autopilot = require("../Lib/Autopilot")

local ParTicks = 3600

return Test.Suite("Autopilot.Level3", function()
	Test.Case("reaches the goal under par", function()
		Autopilot.Case(ParTicks)
	end, { TimeoutTicks = ParTicks + 60 })
end)
