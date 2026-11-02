# UI: canvases, rectangles, images, text and string tables

A reference page of the `game-building` skill (`skills/game-building`). The UI components are in `reference/Components` (category Ui), their script proxies in `reference/ScriptAPI`; `luau-gameplay` ("UI") shows a script that sets UI text from a string table.

## How UI is laid out and drawn

- **Scene pixels.** UI is laid out, clipped and hit-tested in the pixels of the game's scene image: x right, y down, the origin at its top-left corner, the view's size (the project's `Window` size in every lockstep session, test run and replay). It draws over the game, after the game's screen `Text`, and is part of the game's image: the Game view, an exported game and `viewport_screenshot {view: "Game"}` show it, while the editor's Scene view draws no game UI.
- **A canvas** (`UiCanvas`) is the root of a UI tree and covers the whole scene image; it has no rectangle of its own and no `Transform` effect. `ScaleMode` maps its units to scene pixels: `ConstantPixelSize` gives `ScaleFactor` pixels per unit whatever the resolution; `MatchHeight` makes the canvas `ReferenceHeight` units high (1080 by default), so the UI scales with the window's height. `PixelSnap` (on by default) rounds every corner to whole pixels. Canvases draw in ascending `SortOrder`, ties in entity order.
- **An element** is an entity with a `UiRect` below a canvas. Its parent element is its nearest ancestor with a `UiRect` (an entity without one in between is transparent, a plain grouping), or the canvas. A `UiRect` with no `UiCanvas` above it is never laid out or drawn (`project_validate` reports `UI_ELEMENT_WITHOUT_CANVAS`). A disabled element (`SetActive(false)`, or a disabled ancestor) and its subtree are left out and take no space in a container.
- **Draw order** is painter's order inside a canvas: a parent before its children, siblings in order, and on one entity its `UiImage` before its `UiText` (a button's background, then its label).
- **When.** The layout runs at the end of every fixed step and again after `OnLateUpdate` each frame, so UI that `OnUpdate` or `OnLateUpdate` writes shows in that frame. Layouts are derived data: they never enter the state hash.
- **Rectangles never follow their content.** A label is not sized to its text, an image not to its frame, a container not to its children. Give text room; `ui_tree` shows every rectangle, so an overflow is visible.

## Rectangles and anchors (`UiRect`)

In canvas units, in the parent element's rectangle `P`:
- the anchor rectangle runs from `P.Min + AnchorMin * P.Size` to `P.Min + AnchorMax * P.Size` (`AnchorMin` at most `AnchorMax` on each axis; equal anchors are a point);
- the element's size is the anchor rectangle's size plus `Size` (each at least 0, so a negative `Size` insets stretched anchors);
- `Pivot` is the point of the element (0 to 1 from its top-left corner) placed at the anchor rectangle's own pivot point plus `Position`.

| Wanted | AnchorMin | AnchorMax | Pivot | Position | Size |
|---|---|---|---|---|---|
| 200 x 60 in the centre | `[0.5, 0.5]` | `[0.5, 0.5]` | `[0.5, 0.5]` | `[0, 0]` | `[200, 60]` |
| a bar along the top, 8 units in, 40 high | `[0, 0]` | `[1, 0]` | `[0.5, 0]` | `[0, 8]` | `[-16, 40]` |
| 120 x 40 in the bottom-right corner, 10 units in | `[1, 1]` | `[1, 1]` | `[1, 1]` | `[-10, -10]` | `[120, 40]` |
| the whole parent (a full-screen background) | `[0, 0]` | `[1, 1]` | `[0.5, 0.5]` | `[0, 0]` | `[0, 0]` |

`ClipChildren` clips the element's descendants (never the element itself) to its rectangle; nested clips intersect.

```json
{"label": "Hud", "ops": [
	{"name": "hud", "method": "entity.create", "params": {"name": "Hud", "components": {"UiCanvas": {"ScaleMode": "MatchHeight"}}}},
	{"name": "bar", "method": "entity.create", "params": {"name": "Bar", "parent": {"$ref": "hud.entity.id"}, "components": {
		"UiRect": {"AnchorMin": [0, 0], "AnchorMax": [1, 0], "Pivot": [0.5, 0], "Position": [0, 8], "Size": [-16, 64]},
		"UiImage": {"Sheet": "Assets/Ui/Panels.spritesheet", "Frame": 0, "Type": "Sliced"},
		"UiLayout": {"Kind": "Stack", "Direction": "Horizontal", "Padding": {"Left": 12, "Top": 8, "Right": 12, "Bottom": 8},
			"Spacing": [16, 0], "CrossAlignment": "Stretch"}}}},
	{"method": "entity.create", "params": {"name": "Score", "parent": {"$ref": "bar.entity.id"}, "components": {
		"UiRect": {"Size": [240, 0]}, "UiText": {"Text": "Score 0", "Size": 36, "VerticalAlignment": "Middle", "Wrap": false}}}}]}
```

## Containers (`UiLayout`, `UiLayoutElement`)

A `UiLayout` places the elements whose parent element it is, in entity order, inside its rectangle less `Padding`; each child's preferred size is its `UiRect` `Size`, and its own anchors and position are ignored.
- **Stack:** one after another along `Direction` with `Spacing`; `MainAlignment` places the run (Start, Center, End).
- **Flex:** a Stack whose children share the line's free space by `UiLayoutElement.Grow` or give back an overflow by `Shrink` times their size (never below `MinSize`); `Wrap` starts a new line when a child does not fit.
- **Grid:** cells of `CellSize`, `Columns` per row (`Horizontal`) or column (`Vertical`), or as many as fit with `Columns` 0.
- `CrossAlignment` places each child across its line: Start, Center, End, or Stretch to fill it. `UiLayoutElement.IgnoreLayout` keeps a child's own placement and takes it out of the run.

## Images and text

- **`UiImage`** draws frame `Frame` of the sprite sheet `Sheet` (`asset_create {type: "SpriteSheet"}`, `skills/game-building` "3. Scenes, assets and prefabs") over the element's rectangle, tinted by `Color`; a null `Sheet` draws a plain rectangle of `Color`. `Type: "Sliced"` keeps the frame's 9-slice `Borders` at `BorderScale` canvas units per texel and stretches the edges and the centre: make panels and buttons from one small bordered frame. `Filter: "Point"` (the default) keeps pixel art sharp, and an image whose rectangle is its frame's size in scene pixels reproduces its texels exactly; `Linear` smooths a scaled image. A `Frame` the sheet lacks draws nothing (`UI_IMAGE_FRAME_OUT_OF_RANGE`), and so does a missing sheet.
- **`UiText`** lays out `Text` (UTF-8, `\n` starts a line) in the rectangle: `Size` is the em size in canvas units, `Wrap` breaks lines at spaces to fit its width, `Alignment` (Left, Center, Right) aligns each line, `VerticalAlignment` (Top, Middle, Bottom) places the block, `LineSpacing` multiplies the line height. A null `Font` is the default font, which holds ASCII and Latin-1; a font whose import settings list `Ranges` of codepoints bakes them too (`FontImportSettings`, `reference/FileFormats`), and characters a font lacks are skipped. Text is clipped only by a clipping ancestor, never by its own rectangle.

## String tables and `Loc`

A string table (`.stringtable`, `reference/FileFormats`) holds keyed strings in several locales; the first locale is the fallback. Create it with `asset_create {type: "StringTable", path: "Assets/Text/Menu.stringtable", values: {Locales: ["en", "fr"], Entries: [{Key: "Menu.Start", Values: {en: "Start", fr: "Commencer"}}]}}` and change it with `asset_set_properties`.
- Scripts read it with `Loc.Get(table, key)` (`Loc.Has` tests a key), holding the table in a `Field.Asset("StringTable")` field or loading it by literal path (`Assets.Load("Assets/Text/Menu.stringtable")`, which a pruned export keeps).
- The locale is the project's `Localization.Locale` (`fr`, `pt-BR`; empty reads every table's first locale): a table's locale equal to it, else its language's (`pt` for `pt-BR`), else the fallback; a key without a value in that locale reads the fallback's. It is fixed for the session: a language menu takes effect at the next session or export.
- A key the table lacks raises a located error naming the closest keys: test every key the game uses in a suite.

## Checking the UI

- **`ui_tree {format: "Text" | "Json", canvas?, depth?, width?, height?, target?}`** lists every canvas and element in draw order with its rectangle in scene pixels, its clip, whether it is visible (covers a pixel once clipped) and its UI components and text. The edit scene is laid out at the project's window size unless `width` and `height` name another; `target: "Play"` reads the play session's current layout. A canvas's `scale` (scene pixels per canvas unit) turns a measured pixel offset into the `UiRect` units to write. The exported game's Runtime serves `ui.tree` too. Its `interactable`, `focused`, `hovered`, `pressed` and `widget` members are reserved for interactive widgets and report their defaults.
- **`viewport_screenshot {view: "Game", annotate: {bounds: true, labels: "all"}}`** outlines every drawn element in yellow and labels it with its name and short id, over the UI.
- `project_validate {}` reports `UI_ELEMENT_WITHOUT_CANVAS` and `UI_IMAGE_FRAME_OUT_OF_RANGE` (`skills/game-debugging/validation-codes`).

## Input in `OnUpdate`

In live play (the editor's Play without lockstep, an exported game), a frame that runs no fixed step already sees the device input queued for the next tick, so `OnUpdate` reacts to a press in the frame it happens: `Input.IsKeyPressed`, `IsMouseButtonPressed` and `IsActionPressed` are true once per press in `OnUpdate`, possibly a frame before the tick that applies it, and `OnFixedUpdate` sees the press at that tick, as before. Lockstep sessions, recordings, replays and test runs never preview: there `OnUpdate` sees input only from the tick that applies it, and injected input (`Test.Inject*`, `play_step {input}`, replays) is never previewed anywhere. Keep decisions that reach the state hash in `OnFixedUpdate`, and use `OnUpdate`'s earlier view for presentation only (a highlight, a cursor).
