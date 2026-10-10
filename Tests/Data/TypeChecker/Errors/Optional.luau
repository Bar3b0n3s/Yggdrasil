--!strict
local function bad(entity: Entity)
	local low, high = entity:GetWorldBounds()
	return high - low
end
return bad
