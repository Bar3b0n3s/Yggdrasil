--!strict
-- Formats whole ticks (60 a second) as M:SS.t by integer arithmetic.
local Clock = {}

function Clock.Format(ticks: number): string
	local tenths = ticks * 10 // 60
	local minutes = tenths // 600
	local seconds = (tenths // 10) % 60
	return string.format("%d:%02d.%d", minutes, seconds, tenths % 10)
end

return Clock
