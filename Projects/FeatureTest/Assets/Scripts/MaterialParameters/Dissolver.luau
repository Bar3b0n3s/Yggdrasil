--!strict
-- MaterialParameters.scene's Enemy (M31; Docs/Decisions/0046-m31-material-parameters.md decision 9): the Jump action flashes
-- it white-hot for a few ticks, then it dissolves over half a second, as World Tree's enemies die, so
-- MaterialParameters.replay's state depends on its input. Without input it stands untouched. Every write goes through the
-- MaterialParameters proxy, so the replay's checkpoints hash the parameters each tick.
export type Dissolver = ScriptInstance & {
	Dying: boolean,
	Flash: number,
}

local DISSOLVE_SECONDS = 0.5
local FLASH_TICKS = 6
local FLASH_STRENGTH = 4

local Dissolver = {}

function Dissolver.OnCreate(self: Dissolver)
	self.Dying = false
	self.Flash = 0
end

function Dissolver.OnFixedUpdate(self: Dissolver, dt: number)
	local parameters = self.Entity.MaterialParameters :: MaterialParameters
	if not self.Dying and Input.IsActionPressed("Jump") then
		self.Dying = true
		self.Flash = FLASH_TICKS
	end
	if self.Flash > 0 then
		self.Flash -= 1
		parameters.EmissiveStrength = if self.Flash > 0 then FLASH_STRENGTH else 0
	end
	if self.Dying then
		parameters.DissolveAmount = math.min(1, parameters.DissolveAmount + dt / DISSOLVE_SECONDS)
	end
end

return Script.Define("Dissolver", Dissolver)
