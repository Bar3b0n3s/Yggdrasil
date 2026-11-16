# Game services in scripts

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): display settings, the performance readout, presentation text and frame-time signals, the services a game's HUD, options menu and stress modes use. The signatures are in `reference/ScriptAPI` (sections `Application`, `UiText` and `Input`).

**Display settings.** An options menu lists `Application.GetDisplayModes()` (the monitor's video modes, `{Width, Height, RefreshRate}`, sorted) and shows `Application.GetDisplaySettings()` (`WindowMode` `"Windowed"`, `"Borderless"` or `"Exclusive"`, `Width`, `Height`, `RefreshRate`, `VSync`). `Application.SetDisplaySettings(settings)` requests a change for the next frame boundary; a member left out keeps its current value. `Windowed` takes `Width` x `Height`, `Borderless` covers the monitor at its current mode, and `Exclusive` switches the monitor to one of the listed modes. The Runtime keeps the choice in `user://Display.json` and restores it at the next start, before its window shows, so the game needs no save of its own for it. The calls take effect only in a live, windowed Runtime on Windows. In the editor, headless runs, test runs, replays, recordings and lockstep sessions, and on Linux and macOS, the mode list is empty, the settings are the session's view as a `Windowed` window, and `SetDisplaySettings` returns `false` and the reason, so an options menu still populates and never reaches the state hash. A request outside the ranges (a `Width` or `Height` outside 1 to 16384, a `RefreshRate` over 1000, an unknown mode) raises everywhere, and in the Runtime so does an `Exclusive` mode the monitor does not list.

```luau path=Assets/Scripts/DisplayMenu.luau
--!strict
local DisplayMenu = {}

-- The options menu's resolution list: one label per mode the monitor lists (none where display settings do nothing).
function DisplayMenu.GetLabels(): {string}
	local labels = {}
	for _, mode in Application.GetDisplayModes() do
		table.insert(labels, string.format("%d x %d at %d Hz", mode.Width, mode.Height, mode.RefreshRate))
	end
	return labels
end

-- Applies a chosen mode full screen; returns the reason a request was refused, nil when it was accepted.
function DisplayMenu.Apply(mode: DisplayMode, vsync: boolean): string?
	local accepted, reason = Application.SetDisplaySettings({
		WindowMode = "Exclusive",
		Width = mode.Width,
		Height = mode.Height,
		RefreshRate = mode.RefreshRate,
		VSync = vsync,
	})
	return if accepted then nil else reason
end

return DisplayMenu
```

**The performance readout.** `Application.GetStats()` returns a `PerformanceStats` table in every configuration, Dist included: `Fps` and `CpuMilliseconds` (the frame loop's), the game view's last rendered frame (`RenderAvailable`; `GpuMilliseconds` with `GpuAvailable`, `VisibleLights` up to the per-view cap and `DroppedLights` beyond it, `ShadowedLights`, `DrawCalls`, `Triangles`, `SkinnedMeshes`; all 0 without a renderer, as in a headless run) and the session's `Particles`, `Bodies`, `ActiveBodies`, `Entities` and `ScriptHeapBytes`. These values depend on frame pacing, so they follow `ParticleEmitter:GetParticleCount`'s rule: a fixed-step callback (`OnFixedUpdate`, a task, a physics, animation or UI event) raises an error; read them in `OnUpdate`, `OnLateUpdate` or a test case, and never store them in fields that the fixed step reads or in component data, which the state hash covers.

**Presentation text.** UI components are hashed, so writing a frame-paced number into `UiText.Text` would split every replay. `self.Entity.UiText:SetPresentationText(text)` shows `text` in place of the element's `Text` until it is cleared with `nil` (or the element, its `UiText` or the scene goes away); it is never serialized, hashed or recorded, and layout and hit testing keep the authored element, so size the element for the longest value. `GetPresentationText()` reads it back. Both follow the readout's rule: set it in `OnUpdate` or `OnLateUpdate`. `ui.tree` reports it as `presentationText` beside the authored `text`.

**Frame-time signals.** A decision taken on a frame-paced value (ending a horde run whose frame time stays over 33 ms) changes the simulation, so it is recorded as input: `Input.PostSignal(name)` posts a named signal from the frame phase (a fixed-step callback raises: there a decision is deterministic and acts directly), the next fixed step latches it, and gameplay reads `Input.WasSignalled(name)` there (in `OnUpdate` it reports the ticks since the previous frame). `input.record` records the signals a recording posts and a replay plays them back, so a run that ended on frame time verifies at its hash in every mode. Replays, test runs and lockstep sessions ignore live signals (`PostSignal` returns false); a test injects one with `Test.InjectSignal(name)` and automation with `input.inject {events: [{type: "signal", name}]}`. Names are 1 to 64 bytes; a name posted twice for one tick counts once.

```luau path=Assets/Scripts/HordeHud.luau
--!strict
export type HordeHud = ScriptInstance & {
	BudgetMs: number,
	SlowFrames: number,
	Ended: boolean,
}

local HordeHud = {}

HordeHud.Fields = {
	BudgetMs = Field.Number(33, { Min = 1 }),
}

function HordeHud.OnCreate(self: HordeHud)
	self.SlowFrames = 0
	self.Ended = false
end

-- The fixed step acts on the latched signal: deterministic, and a replay plays the signal back.
function HordeHud.OnFixedUpdate(self: HordeHud)
	if not self.Ended and Input.WasSignalled("FrameTimeExceeded") then
		self.Ended = true
		local text = self.Entity.UiText
		if text then
			text.Text = "Run over"
		end
	end
end

-- The frame phase reads the readout, shows it outside the state hash and posts the decision as input.
function HordeHud.OnUpdate(self: HordeHud)
	local text = self.Entity.UiText
	if text == nil or self.Ended then
		return
	end
	local stats = Application.GetStats()
	text:SetPresentationText(string.format("%.1f ms  %d lights (%d dropped)", stats.CpuMilliseconds, stats.VisibleLights, stats.DroppedLights))
	self.SlowFrames = if stats.CpuMilliseconds > self.BudgetMs then self.SlowFrames + 1 else 0
	if self.SlowFrames >= 120 then
		Input.PostSignal("FrameTimeExceeded")
	end
end

return Script.Define("HordeHud", HordeHud)
```
