--!strict
-- A caption of SessionLocale.scene (M38, Docs/Decisions/0055-m38-runtime-locale.md decisions 1 and 2): it draws its Key from
-- SessionLocale.stringtable into its UiText in OnStart, and again in OnLocaleChanged, which Loc.SetLocale delivers to every
-- started behaviour before it returns; it logs each delivery as "<entity>:<locale>:<previous>" (Log.luau) and counts them.
local Log = require("./Log")

local TABLE = "Assets/Localization/SessionLocale.stringtable"

local Caption = {}

Caption.Fields = {
	Key = Field.String("Title", { Tooltip = "The string table key the caption draws" }),
}

function Caption.Draw(self: any)
	local text = self.Entity.UiText
	text.Text = Loc.Get(Assets.Load(TABLE) :: AssetRef, self.Key)
end

function Caption.OnStart(self: any)
	self.Changes = 0
	self.StartedIn = Loc.GetLocale()
	Caption.Draw(self)
end

function Caption.OnLocaleChanged(self: any, locale: string, previous: string)
	self.Changes += 1
	table.insert(Log.Entries, self.Entity.Name .. ":" .. locale .. ":" .. previous)
	Caption.Draw(self)
end

return Script.Define("Caption", Caption)
