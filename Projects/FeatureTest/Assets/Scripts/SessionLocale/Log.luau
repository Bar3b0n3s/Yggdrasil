--!strict
-- The order in which SessionLocale.scene's captions heard OnLocaleChanged (M38, Docs/Decisions/0055-m38-runtime-locale.md
-- decision 2), shared by the captions and the SessionLocale suite through require, as one VM's modules are.
local Log = {}
Log.Entries = {} :: { string }

function Log.Clear()
	table.clear(Log.Entries)
end

return Log
