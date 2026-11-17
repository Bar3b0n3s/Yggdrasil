# Languages

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): a language the player chooses while the game runs. The signatures are in `reference/ScriptAPI` (section `Loc`).

**The session's locale.** A play session starts in the project's `Localization.Locale`, unless it was started in another (`play_start {locale}`, the Runtime's `--locale`, a suite's `Locale`, a replay's header). `Loc.Get(table, key)` reads every string table in it (the table's locale equal to it, else its language's, else the table's first), and `Loc.GetLocale()` returns it. `Loc.SetLocale(locale)` changes it for the rest of the session: `Loc.Get` and `Loc.GetLocale` read the new one from the next call, and every scene a later `Scene.Load` or `Scene.Reload` makes starts in it. The value is the empty locale (every table's first) or a tag such as `de`, `ar` or `pt-BR`; anything else raises a located error. Setting the locale in force changes nothing.

**Redraw in `OnLocaleChanged`.** `Loc.SetLocale` calls `OnLocaleChanged(self, locale, previous)` on every started behaviour that defines it, the caller's own included, in execution order, **before it returns**, whether or not the world is paused, so the options menu's click redraws the title, the menus and the HUD in the same tick:

```luau
--!strict
local Caption = {}

Caption.Fields = {
	Table = Field.Asset("StringTable"),
	Key = Field.String("Title"),
}

function Caption.Draw(self: any)
	(self.Entity.UiText :: UiText).Text = Loc.Get(self.Table, self.Key)
end

function Caption.OnStart(self: any) Caption.Draw(self) end
function Caption.OnLocaleChanged(self: any, locale: string, previous: string) Caption.Draw(self) end

return Script.Define("Caption", Caption)
```

- A scene's behaviours draw in `OnStart`: a scene loaded after the change starts in the new locale and gets no `OnLocaleChanged`.
- `Loc.SetLocale` raises inside `OnLocaleChanged` (no nested change) and while the scene's scripts are torn down (an `OnDestroy` at the session's end).
- A fault in one behaviour's `OnLocaleChanged` disables only that behaviour; the others still hear it.
- The engine stores no language choice: keep the player's in save data (`SaveData.Set("Language", Loc.GetLocale())`, `SaveData.Commit()`) and restore it at startup with `Loc.SetLocale` (a title that does so overrides the Runtime's `--locale`, so film or check an export with a store that holds no other choice).
- The engine knows no language's direction: keep a direction key in your string table (`meta.direction = "rtl"`) and mirror your rows from it in `OnLocaleChanged` (below).

**Right-to-left layouts.** Labels need no code: a `UiText` whose `Alignment` is `"Start"` or `"End"` aligns each paragraph by its own direction (an Arabic paragraph's start is the right edge, a Latin one's the left; a paragraph of digits alone reads left to right, so give a number `"Right"` if it must hug the right edge). Rows are yours to mirror: a `UiLayout` whose `Direction` is `"HorizontalReverse"` places its first child at the right, and a `UiSlider` with that `Direction` puts its Min at the right edge, so Left steps it toward Max. Rows are component data, so the change is hashed and replayed like any other. Set them in `OnStart` as well as in `OnLocaleChanged`, as `Caption` draws: a session that starts in Arabic (`--locale ar`, a suite's `Locale`, a replay recorded in `ar`) and every scene loaded after the switch get no `OnLocaleChanged`.

```luau
--!strict
local Options = {}

Options.Fields = {
	Table = Field.Asset("StringTable"),
	Rows = Field.Array(Field.Entity()),
}

-- Mirrors every row, and the sliders in it, for the locale in force.
function Options.Apply(self: any)
	local direction: UiLayoutDirection = if Loc.Get(self.Table, "meta.direction") == "rtl" then "HorizontalReverse" else "Horizontal"
	for _, row in self.Rows do
		(row.UiLayout :: UiLayout).Direction = direction
		for _, child in row:GetChildren() do
			local slider = child.UiSlider
			if slider then
				slider.Direction = direction
			end
		end
	end
end

function Options.OnStart(self: any) Options.Apply(self) end
function Options.OnLocaleChanged(self: any, locale: string, previous: string) Options.Apply(self) end

return Script.Define("Options", Options)
```

Canvas-wide mirroring (anchors, pivots, scroll views and navigation) is not the engine's: place each panel for its direction yourself.

**Determinism and replays.** The session's locale is in the state hash while it differs from the locale the session started in, and the start locale is in a replay's header (version 5), so a replay plays in the language it was recorded in and a change during it must come from its input (a click on the language row): a `script_eval` that calls `Loc.SetLocale` during a recording invalidates it, as any evaluation write does.

**Tests in each language.** List a strings suite once per language in `Testing.Suites`, each with its `Locale` (`{"Script": "Assets/Tests/Strings.test.luau", "Locale": "de"}`, then `"ar"`): its rows read `Strings (de)` and `Strings (ar)`, and `test_run {filter: "Strings (ar)/"}` runs one. A suite without a `Locale` starts in `test_run {locale}`'s, else the project's.
