--!strict
local Behaviour = require("./Templates/Behaviour")
local function speed(entity: Entity): number
	local instance = entity:GetScript() :: Behaviour.FixtureBehaviour?
	return if instance then instance.Speed else 0
end
return speed
