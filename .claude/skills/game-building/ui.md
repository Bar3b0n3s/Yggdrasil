# UI: canvases, rectangles, images, text, widgets and string tables

A reference page of the `game-building` skill (`skills/game-building`). The UI components are in `reference/Components` (category Ui), their script proxies and the `Ui` module in `reference/ScriptAPI`; `luau-gameplay` ("UI") shows a script that sets UI text from a string table and one that handles a menu's events.

## How UI is laid out and drawn

- **Scene pixels.** UI is laid out, clipped and hit-tested in the pixels of the game's scene image: x right, y down, the origin at its top-left corner, the view's size (the project's `Window` size in every lockstep session, test run and replay). It draws over the game, after the game's screen `Text`, and is part of the game's image: the Game view, an exported game and `viewport_screenshot {view: "Game"}` show it, while the editor's Scene view draws no game UI unless its UI preview is on ("Authoring" below).
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
- **`UiText`** lays out `Text` (UTF-8, `\n` starts a line) in the rectangle: `Size` is the em size in canvas units, `Wrap` breaks lines at spaces to fit its width, `Alignment` (Left, Center, Right) aligns each line, `VerticalAlignment` (Top, Middle, Bottom) places the block, `LineSpacing` multiplies the line height. A null `Font` is its canvas's `Font`, or the default font, which holds ASCII and Latin-1, when the canvas names none; a font whose import settings list `Ranges` of codepoints bakes them too (`FontImportSettings`, `reference/FileFormats`), and characters a font lacks are skipped. Text is clipped only by a clipping ancestor, never by its own rectangle.

## Widgets

- **The four widgets**, one per element, each with `Interactable` and `Focusable`: `UiButton` is clicked; `UiToggle` flips `IsOn` and shows its `Graphic` element (and its subtree) only while on; `UiSlider` holds `Value` between `Min` and `Max` (read clamped, rounded with `WholeNumbers`), its `Fill` stretched from the Min edge (left, or bottom when `Direction` is `Vertical`) to the value and its `Handle` at it, and one direction press moves it by `Step` (a tenth of the range when 0); `UiScrollView` clips its descendants and moves its `Content` element by `ScrollPosition` (read clamped to the content's overflow) along `Horizontal` and `Vertical`, by `ScrollStep` per wheel step or direction press. `Graphic`, `Fill`, `Handle` and `Content` name elements below the widget in its canvas; any other entity is ignored (`UI_WIDGET_PART_INVALID`). Give a scroll view's content its full size: rectangles never follow their content.
- **`ui_create_widget {kind, parent?, index?, name?, text?}`** builds a `Canvas`, `Panel`, `Image`, `Text`, `Button`, `Toggle`, `Slider` or `ScrollView` from its template (plain colours, a button's or toggle's `Label` in the canvas's font, a slider's `Fill` and `Handle`, a scroll view's stacked `Content`) as one undo step, below `parent` (last, or at `index`) or, without one, below a new root canvas; a widget never lands outside a canvas. `entities` lists every entity it created; style them with `entity_update`.
- **The pointer** is the mouse. A visible widget, or a `UiImage` or `UiText` whose `RaycastTarget` is on, catches it and reaches the nearest widget at or above it: it hovers an interactable widget; a press and a release over the same widget click a button or flip a toggle; a press sets a slider from the pointer and drags it; a press on a scroll view, or on its content where no other widget catches the pointer, drags the content; the wheel scrolls the innermost interactable scroll view under it. Turn `RaycastTarget` off on decorations; a panel's image keeps the pointer from a lower canvas's buttons. Gameplay that takes clicks checks `Ui.IsPointerOverUi()` first.
- **Keyboard and gamepad.** The project's `Ui` settings bind Submit (Enter, Space, South), Cancel (Escape, East), the four directions (the arrows, the d-pad, and the left stick with `StickNavigation`) and Next (Tab; with Shift, Previous). A direction moves focus to the nearest focusable widget ahead (`Interactable` and `Focusable`, laid out with an area: a clipped list item counts), Next and Previous through them in draw order, wrapping; Submit clicks a focused button or flips a focused toggle; Cancel reaches the focused element, else the last canvas. A focused slider takes the two directions along it, a focused `Focusable` scroll view those of its axes, and focus moved into a scroll view's content scrolls it into view.
- **Where navigation starts.** A navigation command moves focus only from a focused widget. With nothing focused, the first one focuses the canvas's `DefaultFocus` (that of the last canvas in draw order whose `DefaultFocus` is focusable), whatever its direction, and does nothing when no canvas names one; Submit does nothing then. A pointer press focuses a focusable widget, and a script calls `Ui.SetFocus(element)`. So a menu's canvas names its first button as `DefaultFocus`, or its script calls `Ui.SetFocus` when it opens. A game whose play uses the arrows and Space (Tetris) leaves its HUD canvas's `DefaultFocus` null and turns `Focusable` off on its HUD buttons: the pointer clicks them without taking keyboard focus, and the arrows and Space stay the game's. `Ui.Navigation: false` leaves focus to the pointer and `Ui.SetFocus`.
- **The look.** A canvas's `Theme` tints each widget's own `UiImage` by its state, the first that holds of Disabled (`Interactable` off), Pressed, Focused, Hovered and Normal, so a focused button shows for gamepad play; texts and child images keep their colours.
- **Events.** Interaction runs in the fixed step from latched input, before `OnFixedUpdate`, and writes the widgets' components (`IsOn`, `Value`, `ScrollPosition`); its focus, hover, press and drag are in the state hash, so a replay of UI input reproduces them, and a scene without a canvas keeps its hash. Behaviours receive `OnUiClick`, `OnUiValueChanged`, `OnUiFocus`, `OnUiHover`, `OnUiCancel` and `OnUiScroll`, bubbling from the element to its ancestors (`luau-gameplay`, "UI"); a script's or automation's writes raise none. The `Ui` module reads `GetFocus`, `GetHovered`, `GetPressed`, `IsPointerOverUi` and `GetRect`, and `SetFocus` moves focus.
- **The `Ui` settings** (`project_set_settings {patch: {Ui: {...}}}`): `Navigation`, `StickNavigation` and the binding lists `Submit`, `Cancel`, `Up`, `Down`, `Left`, `Right` and `Next`, each of keys and gamepad buttons as an action's bindings (`Key.Enter`, `Gamepad.South`), at most 16; a play session refuses a mouse control or an axis.

## Authoring

- **The UI preview:** `engine_call {method: "viewport.setOptions", params: {uiPreview: true}}` (the Scene view's Overlays popup, "UI preview") shows the edit scene as the game frames it, through its primary camera with its UI at the game's frame size, with an overlay that outlines every element, selects the topmost one under a click and moves or resizes the selected one by its handles, one `entity.update` per drag. `viewport_screenshot {view: "Game", target: "Edit"}` captures what it shows.
- `UiRect`'s inspector has anchor presets above its fields (Shift also sets the pivot, Alt also moves the element to the anchors); the hierarchy's context menus have "Create UI" and "Create UI child", which run `ui_create_widget`.

## String tables and `Loc`

A string table (`.stringtable`, `reference/FileFormats`) holds keyed strings in several locales; the first locale is the fallback. Create it with `asset_create {type: "StringTable", path: "Assets/Text/Menu.stringtable", values: {Locales: ["en", "fr"], Entries: [{Key: "Menu.Start", Values: {en: "Start", fr: "Commencer"}}]}}` and change it with `asset_set_properties`.
- Scripts read it with `Loc.Get(table, key)` (`Loc.Has` tests a key), holding the table in a `Field.Asset("StringTable")` field or loading it by literal path (`Assets.Load("Assets/Text/Menu.stringtable")`, which a pruned export keeps).
- The locale is the project's `Localization.Locale` (`fr`, `pt-BR`; empty reads every table's first locale): a table's locale equal to it, else its language's (`pt` for `pt-BR`), else the fallback; a key without a value in that locale reads the fallback's. It is fixed for the session: a language menu takes effect at the next session or export.
- A key the table lacks raises a located error naming the closest keys: test every key the game uses in a suite.

## Checking the UI

- **`ui_tree {format: "Text" | "Json", canvas?, depth?, width?, height?, target?}`** lists every canvas and element in draw order with its rectangle in scene pixels, its clip, whether it is visible (covers a pixel once clipped) and its UI components and text. The edit scene is laid out at the project's window size unless `width` and `height` name another; `target: "Play"` reads the play session's current layout. A canvas's `scale` (scene pixels per canvas unit) turns a measured pixel offset into the `UiRect` units to write. The exported game's Runtime serves `ui.tree` too. Each element's `widget` and `interactable` come from its components, and `focused`, `hovered` and `pressed` from the play session (false in the edit scene); the text outline adds the widgets' short forms (`UiToggle(on)`, `UiSlider(<value as read>)`, `UiScrollView(<x>,<y>)`) and the markers `disabled`, `focused`, `hovered` and `pressed`.
- **`viewport_screenshot {view: "Game", annotate: {bounds: true, labels: "all"}}`** outlines every drawn element in yellow and labels it with its name and short id, over the UI.
- `project_validate {}` reports `UI_ELEMENT_WITHOUT_CANVAS`, `UI_IMAGE_FRAME_OUT_OF_RANGE` and `UI_WIDGET_PART_INVALID` (`skills/game-debugging/validation-codes`).

## Input in `OnUpdate`

In live play (the editor's Play without lockstep, an exported game), a frame that runs no fixed step already sees the device input queued for the next tick, so `OnUpdate` reacts to a press in the frame it happens: `Input.IsKeyPressed`, `IsMouseButtonPressed` and `IsActionPressed` are true once per press in `OnUpdate`, possibly a frame before the tick that applies it, and `OnFixedUpdate` sees the press at that tick, as before. Lockstep sessions, recordings, replays and test runs never preview: there `OnUpdate` sees input only from the tick that applies it, and injected input (`Test.Inject*`, `play_step {input}`, replays) is never previewed anywhere. Keep decisions that reach the state hash in `OnFixedUpdate`, and use `OnUpdate`'s earlier view for presentation only (a highlight, a cursor).
