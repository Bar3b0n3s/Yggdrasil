--!strict
-- A world behaviour of WorldPause.scene (M35, Docs/Decisions/0050-m35-pause-entity-state.md decisions 2 and 6): it holds
-- while the world is paused. It counts its updates and its task's runs, and records the session tick at which the
-- half-second delay its OnStart begins ended, which the WorldPause suite compares with the menu's.
export type WorldCounter = ScriptInstance & {
	Fixed: number,
	Updates: number,
	LateUpdates: number,
	TaskRuns: number,
	DelayEndedAt: number,
}

local WorldCounter = {}

function WorldCounter.OnCreate(self: WorldCounter)
	self.Fixed = 0
	self.Updates = 0
	self.LateUpdates = 0
	self.TaskRuns = 0
	self.DelayEndedAt = -1
end

function WorldCounter.OnStart(self: WorldCounter)
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

function WorldCounter.OnFixedUpdate(self: WorldCounter)
	self.Fixed += 1
end

function WorldCounter.OnUpdate(self: WorldCounter)
	self.Updates += 1
end

function WorldCounter.OnLateUpdate(self: WorldCounter)
	self.LateUpdates += 1
end

return Script.Define("WorldCounter", WorldCounter)
