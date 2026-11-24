# Game services in scripts

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): display settings, save data, the session's audio mix and music, the performance readout, presentation text and frame-time signals, the services a game's HUD, options menu and stress modes use. The signatures are in `reference/ScriptAPI` (sections `Application`, `SaveData`, `Audio`, `UiText` and `Input`).

**Display settings.** An options menu lists `Application.GetDisplayModes()` (the monitor's video modes, `{Width, Height, RefreshRate}`, sorted) and shows `Application.GetDisplaySettings()` (`WindowMode` `"Windowed"`, `"Borderless"` or `"Exclusive"`, `Width`, `Height`, `RefreshRate`, `VSync`). `Application.SetDisplaySettings(settings)` requests a change for the next frame boundary; a member left out keeps its current value, the one in force, so a later request in the same frame replaces an earlier one: give every member you change in one request. `Windowed` takes `Width` x `Height`, `Borderless` covers the monitor at its current mode, and `Exclusive` switches the monitor to one of the listed modes. The Runtime keeps the choice in `user://Display.json` and restores it at the next start, before its window shows, so the game needs no save of its own for it. The calls take effect only in a live, windowed Runtime on Windows. In the editor, headless runs, test runs, replays, recordings and lockstep sessions, and on Linux and macOS, the mode list is empty, the settings are the session's view as a `Windowed` window, and `SetDisplaySettings` returns `false` and the reason, so an options menu still populates and never reaches the state hash. A request outside the ranges (a `Width` or `Height` outside 1 to 16384, a `RefreshRate` over 1000, an unknown mode) raises everywhere, and in the Runtime so does an `Exclusive` mode the monitor does not list.

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

**Save data.** `SaveData.Get(key)` reads what the game saved (nil when absent), `SaveData.Set(key, value)` saves a value (nil deletes it), `SaveData.Delete(key)` returns whether there was one and `SaveData.GetKeys()` lists the keys, sorted. Values are what converts to JSON: booleans, numbers, strings, arrays and tables of string keys, nesting at most 64 arrays and tables; keys are 1 to 256 bytes and the whole store at most 1 MiB. The store is part of the session's state (its hash covers it) and lasts across `Scene.Load` and `Scene.Reload`, so options and progress read in one scene are there in the next; it outlives the run only once `SaveData.Commit()` writes it, which returns `true`, or `false` and the reason (a headless run never writes its store). Commit at moments the player expects (closing the options menu, finishing a level), not every frame. Test runs, replays, recordings and lockstep sessions keep their store in memory, empty at the start: a suite seeds one with `Test.SetSaveData({...})`, and a replay starts with the store its recording started with, so a replay verifies whatever the machine's store holds. When the store on disk could not be read, the game starts with an empty one and `SaveData.GetLoadError()` says why: a live windowed run keeps the file as `Store.corrupt-<n>.json`, while a headless run leaves it in place, so the next windowed run reports it to the player.

**The session's audio mix.** `Audio.SetGroupVolume(group, volume)` holds for the rest of the session, across `Scene.Load` and `Scene.Reload`; the session starts every group at 1, and the volumes the editor or an earlier session had come back when it ends. A volume the player chose in the options menu outlives the run only through the store: save it, and set it again from `SaveData.Get` when the game starts.

```luau path=Assets/Scripts/Options.luau
--!strict
local Options = {}

-- At the start: the volume the player saved last time, or full volume.
function Options.Restore()
	local volume = SaveData.Get("MusicVolume")
	Audio.SetGroupVolume("Music", if type(volume) == "number" then volume else 1)
end

-- When the player closes the options menu: keep the choice for the next run.
function Options.Save(volume: number): string?
	Audio.SetGroupVolume("Music", volume)
	SaveData.Set("MusicVolume", volume)
	local committed, reason = SaveData.Commit()
	return if committed then nil else reason
end

return Options
```

**Music across scenes.** The play session owns one track at a time, so music goes on through `Scene.Load` and `Scene.Reload` where an `AudioSource` (the scene's) is cut at every doorway. `Audio.PlayMusic(clip, {Volume, FadeSeconds, Loop})` plays `clip` in the `Music` group at music priority (effects never steal it): with no music playing it starts, its level rising from 0 to `Volume` (default 1) over `FadeSeconds` (0 to 60, default 0, at once); with `clip` already playing its voice is kept, never restarted, and only its level fades to `Volume` (a duck: `{Volume = 0.3, FadeSeconds = 0.5}` under a dialogue) and `Loop` (default true) is applied; another clip crossfades over `FadeSeconds`, and a clip still fading out comes back from where it was. So give every scene of a chain one call in an `OnStart`, the same track in each, and the track plays on; the next realm's scene asks for its own track and crossfades. `Audio.StopMusic(fadeSeconds)` fades it out and stops it, and `Audio.GetMusic()` returns the request (`{Clip, Volume, Loop}`, nil after `StopMusic`), never the voice's progress, so gameplay can read it and replays still verify. A clip played once (`Loop = false`) stays the music until `StopMusic` or another call, and starts again if a scene asks for it after it ended. A track shipped as an opening and a loop plays as one: `Audio.PlayMusic(loop, {Intro = opening})` plays the opening once and the loop from the frame after its last (both of one sample rate, channel count and sample format, so two sound effects or two imported files, or the call raises); the intro matters only when the track starts, so later scenes asking for `loop` keep it wherever it is. A stinger that must not be cut by effects is a `Music` one-shot (`Audio.PlayOneShot(clip, nil, 1, "Music")`, at music priority), and the `Music` group plays on under `Time.PauseWorld`. Fades run on simulation time in lockstep, test runs and replays. Test suites read the voice with `Test.GetMusic()` (`Voice`, the same handle while the track is kept; `Intro` and `InIntro`; `CursorFrames`; `FadeVolume`; `FadingOut`), as of the last frame's audio update: compare handles across a `Scene.Load` by passing them in the load parameters, since the next case runs in the new scene's VM. The calls raise "audio is unavailable" only outside a play session (edit-time evaluations); a session without audio (a test run discovering its suites) records the request and plays nothing, so asking for the track in `OnStart` is safe everywhere.

```luau path=Assets/Scripts/RealmMusic.luau
--!strict
-- On one entity of every scene of a realm, each with the realm's track: the track plays on through every doorway, and a
-- scene of another realm, asking for its own track, crossfades to it.
export type RealmMusic = ScriptInstance & {
	Track: AssetRef?,
	FadeSeconds: number,
}

local RealmMusic = {}

RealmMusic.Fields = {
	Track = Field.Asset("AudioClip"),
	FadeSeconds = Field.Number(1, { Min = 0, Max = 60 }),
}

function RealmMusic.OnStart(self: RealmMusic)
	local track = self.Track
	if track then
		Audio.PlayMusic(track, { FadeSeconds = self.FadeSeconds })
	end
end

return Script.Define("RealmMusic", RealmMusic)
```

**The performance readout.** `Application.GetStats()` returns a `PerformanceStats` table in every configuration, Dist included: `Fps` and `CpuMilliseconds` (the frame loop's), the game view's last rendered frame (`RenderAvailable`; `GpuMilliseconds` with `GpuAvailable`, `VisibleLights` up to `MaxShadedLights`, the shaded-light cap it rendered with (the project's `Rendering.MaxShadedLights`), and `DroppedLights` beyond it, `ShadowedLights`, with `CachedPointShadows` and `CachedSpotShadows` the shadowed lights whose maps the static shadow cache reused, `DrawCalls`, `Triangles`, `SkinnedMeshes`, `PlanarReflections` and `DroppedPlanarReflections` (M42: the planar reflections the view drew, and the candidates it fell back for, past its cap or without their GPU objects), `LodCrossFades` and `DistanceCulledMeshes` (M46: the mesh items the view drew cross-fading two levels of detail, and those beyond their `RenderDistance`'s `CullDistance`), and the area the SSR trace covered in the last completed frame that traced, `SsrTiles`, `SsrTracedTiles` and `SsrTracedPixels`; all 0 without a renderer, as in a headless run) and the session's `Particles`, `Bodies`, `ActiveBodies`, `Entities` and `ScriptHeapBytes`. These values depend on frame pacing, so they follow `ParticleEmitter:GetParticleCount`'s rule: a fixed-step callback (`OnFixedUpdate`, a task, a physics, animation or UI event) raises an error; read them in `OnUpdate`, `OnLateUpdate` or a test case, and never store them in fields that the fixed step reads or in component data, which the state hash covers.

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
