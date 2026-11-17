--!strict
-- Strikes GraphMembers.scene's Crystal from input (Assets/Tests/Replays/GraphMembers.replay; M39,
-- Docs/Decisions/0056-m39-graphs-behaviours.md decision 12): each Jump deals a Bolt of 2 through the Crystal's Health,
-- which calls the Rune graph's Damaged method, and the Rune moves the Crystal to x = Blows, so the replay's state hash
-- depends on its input. Without input nothing strikes.
local Striker = {}

function Striker.OnFixedUpdate(self: ScriptInstance)
	if Input.IsActionPressed("Jump") then
		local health = self.Entity:GetScript("Health")
		if health ~= nil then
			health:Damage({ Kind = "Bolt", Amount = 2 })
		end
	end
end

return Script.Define("Striker", Striker)
