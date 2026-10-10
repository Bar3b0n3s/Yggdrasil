--!strict
local function make(): { Value: number }
	return { Value = 1 }
end
local value = make()
value.Added = true
return value
