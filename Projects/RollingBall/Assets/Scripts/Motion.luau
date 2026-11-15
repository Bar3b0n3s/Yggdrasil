--!strict
-- Pure motion helpers: the platforms' easing and the ball's heading (degrees, 0 facing -Z, positive turning left).
local Motion = {}

-- Wraps an angle in degrees into (-180, 180].
function Motion.Wrap(degrees: number): number
	local wrapped = degrees % 360
	if wrapped > 180 then
		wrapped -= 360
	end
	return wrapped
end

-- A platform's progress from 0 to 1 at a tick of its period of 2 * (dwellTicks + moveTicks) ticks: it dwells at 0,
-- rises along (1 - cos(pi t / moveTicks)) / 2, dwells at 1 and returns the same way.
function Motion.Ease(tick: number, moveTicks: number, dwellTicks: number): number
	local t = tick % (2 * (dwellTicks + moveTicks))
	if t < dwellTicks then
		return 0
	end
	t -= dwellTicks
	if t < moveTicks then
		return (1 - math.cos(math.pi * t / moveTicks)) / 2
	end
	t -= moveTicks
	if t < dwellTicks then
		return 1
	end
	t -= dwellTicks
	return (1 + math.cos(math.pi * t / moveTicks)) / 2
end

-- The ticks a platform has left in its current dwell (0 while it moves), and whether it dwells at its start or end.
function Motion.Dwell(tick: number, moveTicks: number, dwellTicks: number): (boolean, boolean, number)
	local t = tick % (2 * (dwellTicks + moveTicks))
	if t < dwellTicks then
		return true, false, dwellTicks - t
	end
	local endStart = dwellTicks + moveTicks
	if t >= endStart and t < endStart + dwellTicks then
		return false, true, endStart + dwellTicks - t
	end
	return false, false, 0
end

-- The horizontal unit vector a heading faces.
function Motion.Forward(heading: number): vector
	local radians = math.rad(heading)
	return vector.create(-math.sin(radians), 0, -math.cos(radians))
end

-- The horizontal unit vector to the right of a heading.
function Motion.Right(heading: number): vector
	local radians = math.rad(heading)
	return vector.create(math.cos(radians), 0, -math.sin(radians))
end

-- The heading of a velocity's horizontal part.
function Motion.HeadingOf(velocity: vector): number
	return math.deg(math.atan2(-velocity.x, -velocity.z))
end

-- Turns a heading towards a target by at most maxStep degrees, wrapped into (-180, 180].
function Motion.Turn(heading: number, target: number, maxStep: number): number
	local delta = Math.DeltaAngle(heading, target)
	return Motion.Wrap(heading + math.clamp(delta, -maxStep, maxStep))
end

return Motion
