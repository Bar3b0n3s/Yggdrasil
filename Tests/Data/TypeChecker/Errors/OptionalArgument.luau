--!strict
local function length(value: vector): number
	return vector.magnitude(value)
end
local function bad(entity: Entity): number
	local low = entity:GetWorldBounds()
	return length(low)
end
return bad
