--!strict
local Loader = {}

function Loader.Load(name: string)
	return Assets.Load("Assets/Textures/" .. name .. ".png")
end

return Loader
