--!strict
local TypeReload = {}

function TypeReload.Current(self: ScriptInstance): number
	return "two"
end

return Script.Define("TypeReload", TypeReload)
