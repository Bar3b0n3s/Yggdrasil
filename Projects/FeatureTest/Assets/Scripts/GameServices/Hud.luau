--!strict
-- The horde HUD of M28's FeatureTest coverage (Docs/Decisions/0043-m28-game-services.md decisions 4 to 6, GameServices.jsonl,
-- IdSeed 2501): every frame it shows the performance readout through presentation text, which never reaches the state hash,
-- and a frame-time signal ends its run in the fixed step. FeatureTest posts no live signal (a recording would take one at a
-- machine's pace): its replay and its suite inject the signal, as a game's recording records the one it posted.
export type Hud = ScriptInstance & {
	Ended: boolean,
	Ticks: number,
	StatsRefused: boolean,
}

local Hud = {}

function Hud.OnCreate(self: Hud)
	self.Ended = false
	self.Ticks = 0
	self.StatsRefused = false
end

function Hud.OnFixedUpdate(self: Hud)
	self.Ticks += 1
	if self.Ticks == 1 then
		-- The readout is frame-paced: the fixed step cannot read it.
		self.StatsRefused = not pcall(Application.GetStats)
	end
	local text = self.Entity.UiText
	if text and not self.Ended and Input.WasSignalled("FrameTimeExceeded") then
		self.Ended = true
		text.Text = "Run ended"
	end
end

function Hud.OnUpdate(self: Hud)
	local text = self.Entity.UiText
	if text == nil then
		return
	end
	if self.Ended then
		text:SetPresentationText(nil)
		return
	end
	local stats = Application.GetStats()
	text:SetPresentationText(string.format("%.1f ms  %d entities  %d bodies", stats.CpuMilliseconds, stats.Entities, stats.Bodies))
end

return Script.Define("Hud", Hud)
