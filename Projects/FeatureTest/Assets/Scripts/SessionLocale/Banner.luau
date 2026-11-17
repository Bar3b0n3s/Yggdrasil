--!strict
-- SessionLocale.scene's Banner (M38, Docs/Decisions/0055-m38-runtime-locale.md decision 2): the first caption in execution
-- order, which also tries a change of its own inside OnLocaleChanged; it raises there, the first change standing.
local Log = require("./Log")

local Banner = {}

function Banner.OnStart(self: any)
	self.Refused = 0
end

function Banner.OnLocaleChanged(self: any, locale: string, previous: string)
	table.insert(Log.Entries, self.Entity.Name .. ":" .. locale .. ":" .. previous)
	local ok, message = pcall(Loc.SetLocale, "en")
	if not ok and string.find(tostring(message), "OnLocaleChanged", 1, true) ~= nil then
		self.Refused += 1
	end
end

return Script.Define("Banner", Banner)
