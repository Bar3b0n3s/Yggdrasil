--!strict
local function bounds(entity: Entity)
	local a, b = entity:GetWorldBounds()
	local c, d = Physics.GetBodyBounds(entity)
	local e, f = Physics.GetColliderBounds(entity)
	if a and b then local size: vector = b - a; print(size) end
	if c and d then local size: vector = d - c; print(size) end
	if e and f then local size: vector = f - e; print(size) end
end
return bounds
