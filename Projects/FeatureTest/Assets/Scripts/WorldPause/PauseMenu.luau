--!strict
-- The pause menu of WorldPause.scene (M35, Docs/Decisions/0050-m35-pause-entity-state.md decisions 1, 2 and 6): its class
-- runs while the world is paused. P pauses the world (the WorldPause replay presses it), its Resume button resumes it,
-- and it counts its updates, its task's runs, its clicks and focus changes, so the WorldPause suite and the replay see it
-- run while the world holds.
export type PauseMenu = ScriptInstance & {
	Fixed: number,
	Updates: number,
	LateUpdates: number,
	TaskRuns: number,
	DelayEndedAt: number,
	Clicks: { [string]: number },
	Focused: { [string]: number },
	PausedAt: number,
	ResumedAt: number,
}

local PauseMenu = {}

PauseMenu.RunsWhileWorldPaused = true

function PauseMenu.OnCreate(self: PauseMenu)
	self.Fixed = 0
	self.Updates = 0
	self.LateUpdates = 0
	self.TaskRuns = 0
	self.DelayEndedAt = -1
	self.Clicks = {}
	self.Focused = {}
	self.PausedAt = -1
	self.ResumedAt = -1
end

function PauseMenu.OnStart(self: PauseMenu)
	Task.Spawn(function()
		while true do
			Task.WaitTicks(1)
			self.TaskRuns += 1
		end
	end)
	-- Half a second at FeatureTest's 64 Hz: 32 ticks of this behaviour's clock.
	Task.Delay(0.5, function()
		self.DelayEndedAt = Time.GetSessionTick()
	end)
end

function PauseMenu.OnFixedUpdate(self: PauseMenu)
	self.Fixed += 1
	if Input.IsKeyPressed("P") and not Time.IsWorldPaused() then
		Time.PauseWorld("Pause")
		self.PausedAt = Time.GetSessionTick()
	end
end

function PauseMenu.OnUpdate(self: PauseMenu)
	self.Updates += 1
end

function PauseMenu.OnLateUpdate(self: PauseMenu)
	self.LateUpdates += 1
end

function PauseMenu.OnUiFocus(self: PauseMenu, element: Entity, focused: boolean): boolean?
	if focused then
		self.Focused[element.Name] = (self.Focused[element.Name] or 0) + 1
	end
	return nil
end

function PauseMenu.OnUiClick(self: PauseMenu, element: Entity): boolean?
	self.Clicks[element.Name] = (self.Clicks[element.Name] or 0) + 1
	if element.Name == "Resume" and Time.IsWorldPaused() then
		Time.ResumeWorld()
		self.ResumedAt = Time.GetSessionTick()
	end
	return true
end

return Script.Define("PauseMenu", PauseMenu)
