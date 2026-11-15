--!strict
-- Pure path helpers over values the caller reads: the support check's and the autopilot's shared rules.
local Track = {}

-- The polyline's samples every step metres, starting offset metres in, with each sample's distance along it.
function Track.Sample(points: { vector }, step: number, offset: number): ({ vector }, { number })
	local samples: { vector } = {}
	local distances: { number } = {}
	local travelled = 0
	local nextAt = offset
	for index = 1, #points - 1 do
		local a = points[index]
		local b = points[index + 1]
		local length = vector.magnitude(b - a)
		while nextAt <= travelled + length do
			local t = if length > 0 then (nextAt - travelled) / length else 0
			table.insert(samples, a + (b - a) * t)
			table.insert(distances, nextAt)
			nextAt += step
		end
		travelled += length
	end
	return samples, distances
end

-- Whether a point in a marker's local space lies inside its unit box (every coordinate within +-0.5).
function Track.InsideBox(localPoint: vector): boolean
	return math.abs(localPoint.x) <= 0.5 and math.abs(localPoint.y) <= 0.5 and math.abs(localPoint.z) <= 0.5
end

-- The distance along the polyline at each point.
function Track.Lengths(points: { vector }): { number }
	local lengths: { number } = { 0 }
	for index = 2, #points do
		lengths[index] = lengths[index - 1] + vector.magnitude(points[index] - points[index - 1])
	end
	return lengths
end

-- The point of segment index (from points[index] to points[index + 1]) closest to position, and its distance along
-- the polyline.
local function closestOnSegment(points: { vector }, lengths: { number }, index: number, position: vector): (vector, number)
	local a = points[index]
	local b = points[index + 1]
	local ab = b - a
	local lengthSquared = vector.dot(ab, ab)
	local t = if lengthSquared > 0 then math.clamp(vector.dot(position - a, ab) / lengthSquared, 0, 1) else 0
	return a + ab * t, lengths[index] + (lengths[index + 1] - lengths[index]) * t
end

-- Projects a position on the polyline, searching only the segments from first to last (clamped to the polyline), so
-- a projection never jumps to a distant stretch of track. Returns the distance along it and the segment's index.
function Track.Project(points: { vector }, lengths: { number }, position: vector, first: number, last: number): (number, number)
	local from = math.max(1, first)
	local to = math.min(#points - 1, last)
	local bestDistance = math.huge
	local bestAlong = lengths[from]
	local bestIndex = from
	for index = from, to do
		local point, along = closestOnSegment(points, lengths, index, position)
		local distance = vector.magnitude(point - position)
		if distance < bestDistance then
			bestDistance = distance
			bestAlong = along
			bestIndex = index
		end
	end
	return bestAlong, bestIndex
end

-- The point at a distance along the polyline (clamped to its ends) and its segment's index.
function Track.PointAt(points: { vector }, lengths: { number }, along: number): (vector, number)
	if along <= 0 then
		return points[1], 1
	end
	for index = 1, #points - 1 do
		if along <= lengths[index + 1] then
			local span = lengths[index + 1] - lengths[index]
			local t = if span > 0 then (along - lengths[index]) / span else 0
			return points[index] + (points[index + 1] - points[index]) * t, index
		end
	end
	return points[#points], math.max(1, #points - 1)
end

return Track
