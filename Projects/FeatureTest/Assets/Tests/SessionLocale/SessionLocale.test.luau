--!strict
-- The session's locale (M38, Docs/Decisions/0055-m38-runtime-locale.md decisions 1, 2, 6 and 9; World Tree's brief 17) in
-- SessionLocale.scene (SessionLocale.jsonl, IdSeed 3501), listed twice in Testing.Suites with Locale de and ar, so it runs
-- as "SessionLocale (de)" and "SessionLocale (ar)" in every run mode: the suite starts in its entry's locale; Loc.SetLocale
-- changes what Get and GetLocale read from the next call; OnLocaleChanged reaches every started caption in execution order
-- before SetLocale returns, with the new and the previous locale; the locale in force changes nothing; a malformed tag and a
-- change inside OnLocaleChanged raise; Scene.Load and Scene.Reload keep the locale, the new scene starting in it.
local Log = require("../../Scripts/SessionLocale/Log")

local TABLE = "Assets/Localization/SessionLocale.stringtable"
-- The entry's locale, which the suite's Parameters repeat (read in a case: load parameters are not available at load time).
local function startLocale(): string
	return Scene.GetLoadParameters().Locale :: string
end

local function caption(name: string): any
	return (Scene.FindByName(name) :: Entity):GetScript("Caption")
end

return Test.Suite("SessionLocale", function()
	Test.Case("the suite starts in its entry's locale", function()
		local START = startLocale()
		Test.ExpectEqual(Loc.GetLocale(), START)
		Test.ExpectEqual(caption("Title").StartedIn, START)
		Test.ExpectEqual(((Scene.FindByName("Title") :: Entity).UiText :: UiText).Text, Loc.Get(Assets.Load(TABLE) :: AssetRef, "Title"))
	end)

	Test.Case("SetLocale changes what Get and GetLocale read and redraws every caption in order", function()
		local START = startLocale()
		Log.Clear()
		local other = if START == "ar" then "de" else "ar"
		Loc.SetLocale(other)
		Test.ExpectEqual(Loc.GetLocale(), other)
		-- Every caption heard it before SetLocale returned, in execution order (Banner's ExecutionOrder is -10).
		Test.ExpectEqual(#Log.Entries, 3)
		Test.ExpectEqual(Log.Entries[1], "Banner:" .. other .. ":" .. START)
		Test.ExpectEqual(Log.Entries[2], "Title:" .. other .. ":" .. START)
		Test.ExpectEqual(Log.Entries[3], "Play:" .. other .. ":" .. START)
		Test.ExpectEqual(((Scene.FindByName("Title") :: Entity).UiText :: UiText).Text, Loc.Get(Assets.Load(TABLE) :: AssetRef, "Title"))
		-- The locale in force changes nothing.
		Loc.SetLocale(other)
		Test.ExpectEqual(#Log.Entries, 3)
		Loc.SetLocale(START)
		Test.ExpectEqual(#Log.Entries, 6)
		Test.ExpectEqual(caption("Play").Changes, 2)
		-- The Banner's own change inside each OnLocaleChanged raised, and the change it heard stood.
		Test.ExpectEqual((Scene.FindByName("Banner") :: Entity):GetScript("Banner").Refused, 2)
		Test.ExpectEqual(Loc.GetLocale(), START)
	end)

	Test.Case("SetLocale refuses a malformed tag", function()
		local START = startLocale()
		local ok, message = pcall(Loc.SetLocale, "de_DE")
		Test.Expect(not ok and string.find(tostring(message), "not a locale", 1, true) ~= nil, "a malformed tag raises")
		Test.ExpectEqual(Loc.GetLocale(), START)
	end)

	Test.Case("Scene.Load keeps the session's locale", function()
		local START = startLocale()
		Loc.SetLocale("en")
		-- A case returns in the tick that requests the load; the next case reads the new scene.
		Scene.Load(Assets.Load("Assets/Scenes/SessionLocaleB.scene") :: AssetRef, { Locale = START })
	end)

	Test.Case("the loaded scene starts in the session's locale without OnLocaleChanged", function()
		Test.ExpectEqual(Scene.GetName(), "SessionLocaleB")
		Test.ExpectEqual(Loc.GetLocale(), "en")
		local title = caption("Title")
		Test.ExpectEqual(title.StartedIn, "en")
		Test.ExpectEqual(title.Changes, 0)
		Test.ExpectEqual(((Scene.FindByName("Title") :: Entity).UiText :: UiText).Text, "World Tree")
		Scene.Reload()
	end)

	Test.Case("Scene.Reload keeps the session's locale", function()
		local START = startLocale()
		Test.ExpectEqual(Scene.GetName(), "SessionLocaleB")
		Test.ExpectEqual(Loc.GetLocale(), "en")
		Test.ExpectEqual(caption("Title").StartedIn, "en")
		Loc.SetLocale(START)
		Scene.Load(Assets.Load("Assets/Scenes/SessionLocale.scene") :: AssetRef, { Locale = START })
	end)

	Test.Case("the suite's scene starts again in its entry's locale", function()
		local START = startLocale()
		Test.ExpectEqual(Scene.GetName(), "SessionLocale")
		Test.ExpectEqual(Loc.GetLocale(), START)
		Test.ExpectEqual(caption("Title").StartedIn, START)
	end)
end)
