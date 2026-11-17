--!strict
-- SessionLocale.scene's language button (M38, Docs/Decisions/0055-m38-runtime-locale.md decisions 2 and 9): a click switches
-- the session to Arabic, and from Arabic back to its Home locale (the FeatureTest project's fr-CA in the scene), as World
-- Tree's options row steps through its languages. Its Next button loads SessionLocaleB.scene, which starts in the session's
-- locale without OnLocaleChanged.
local Language = {}

Language.Fields = {
	Home = Field.String("fr-CA", { Tooltip = "The locale a click in Arabic returns to" }),
	Next = Field.Bool(false, { Tooltip = "Whether a click loads SessionLocaleB.scene instead" }),
}

function Language.OnUiClick(self: any, element: Entity)
	if self.Next then
		Scene.Load(Assets.Load("Assets/Scenes/SessionLocaleB.scene") :: AssetRef)
	elseif Loc.GetLocale() == "ar" then
		Loc.SetLocale(self.Home)
	else
		Loc.SetLocale("ar")
	end
end

return Script.Define("Language", Language)
