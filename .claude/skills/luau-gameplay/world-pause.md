# The world pause

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): pausing a game's world while its menus keep working. The signatures are in `reference/ScriptAPI` (section `Time`).

**Pause the world, not the session.** `Time.PauseWorld()` freezes the world from the next fixed step on, and `Time.ResumeWorld()` lets it run again; `Time.IsWorldPaused()` reads the state the next step runs with. While the world is paused:
- physics and 2D physics do not step (bodies keep their velocities and fall on from where they hung), animators, sprite animators, particle emitters and the native module's systems hold, and so does a `MaterialParameters` scroll (`UVScroll`), which moves with the world's clock;
- `OnFixedUpdate`, `OnUpdate`, `OnLateUpdate` and tasks (`Task.Wait`, `Task.Delay`) run only for behaviours whose class sets `RunsWhileWorldPaused = true` before its `Script.Define` call, which reads it; every other behaviour holds, its waits and delays included. A visual-scripting graph cannot opt in, so write a pause menu as a Luau behaviour;
- the world's clock holds: `Time.GetTick()` and `Time.GetTime()` (the world's) stop, so gameplay timers built on them stop too, while `Time.GetSessionTick()` and `Time.GetSessionTime()` (the session's) count every fixed step for the behaviours that run; outside a pause the two clocks agree;
- input, the UI (pointer, focus, navigation, typing), every `OnUi*` callback, `OnStart`, `OnDestroy` and entity creation run as always, so a pause menu can show, take input and spawn its own entities;
- no new contact or animation event arises, but an entity destroyed or switched off while the world is paused still ends its contacts (`OnCollisionExit`, `OnTriggerExit`), and a behaviour that starts then still gets the events held for it. These reach every behaviour, also one that holds, since an exit always follows its enter, so a trigger's behaviour may run gameplay on an exit behind the menu.

It is not the time scale: `Time.SetTimeScale(0)` stops every tick and the UI with it. Nor is it the editor's or automation's pause (`play.pause`), which stops the whole session; automation reads the world pause in `play.state`'s `worldPaused` and sets it with `play.setWorldPaused`.

**Sounds.** `Time.PauseWorld("Pause")` (the default) holds every playing voice outside the `Ui` group until the world resumes; `Time.PauseWorld("Finish")` lets them play on to their end. Voices of the `Ui` group always play, so play a menu's sounds in it (`Audio.PlayOneShot(clip, nil, 1, "Ui")`, or an AudioSource whose `Group` is `Ui`).

**Pause from the fixed step.** The pause takes effect at the next fixed step, so a tick runs wholly paused or wholly not. Request it from `OnFixedUpdate`, a UI callback or a task, as every simulated write, so a replay reproduces it in every frame grouping. It is session state, like the time scale: it survives `Scene.Load` and `Scene.Reload`. So a pause menu or a defeat panel that loads or restarts a scene calls `Time.ResumeWorld()` first; otherwise the new scene starts with its world paused and only its `RunsWhileWorldPaused` behaviours running, which soft-locks a scene without a menu of its own. The world pause and the world's clock are in the state hash once a session first pauses.

A pause menu that opens on Escape and closes with its Resume button:

```luau path=Assets/Scripts/PauseMenu.luau
--!strict
export type PauseMenu = ScriptInstance & {
	Panel: Entity?,
	Opened: number,
}

local PauseMenu = {}

-- The menu runs while the world is paused; the game's other behaviours hold.
PauseMenu.RunsWhileWorldPaused = true

PauseMenu.Fields = {
	Panel = Field.Entity(),
}

function PauseMenu.OnCreate(self: PauseMenu)
	self.Opened = -1
end

function PauseMenu.OnFixedUpdate(self: PauseMenu)
	if Input.IsKeyPressed("Escape") and not Time.IsWorldPaused() then
		Time.PauseWorld()
		self.Opened = Time.GetSessionTick()
		if self.Panel then
			self.Panel:SetActive(true)
		end
	end
end

function PauseMenu.OnUiClick(self: PauseMenu, element: Entity): boolean?
	if element.Name == "Resume" then
		Time.ResumeWorld()
		if self.Panel then
			self.Panel:SetActive(false)
		end
		return true
	end
	return nil
end

return Script.Define("PauseMenu", PauseMenu)
```

A suite checks a pause by stepping through it: `Test.WaitTicks` counts the session's ticks, so a case can wait 60 ticks with the world paused and find a body where it was, then resume and see it move. In a suite whose cases share one session (no `Isolation: "Case"`), a case that pauses the world resumes it before it ends, as one that changes the time scale restores it, or the next case starts with the world paused.
