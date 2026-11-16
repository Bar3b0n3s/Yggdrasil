--!strict
-- Navigation.scene's Warden (M30; Docs/Decisions/0045-m30-navigation.md decision 13): the autopilot that the chasers
-- (NavChaser.luau) chase. Navigation.replay's planned input moves it across the arena's floor, MoveX along +X and MoveY
-- along -Z, so the chasers' paths depend on that input; without input it stands still.
export type NavWarden = ScriptInstance

local SPEED = 4 -- metres per second at full input

local NavWarden = {}

function NavWarden.OnFixedUpdate(self: NavWarden, dt: number)
	local move = vector.create(Input.GetAxis("MoveX"), 0, -Input.GetAxis("MoveY"))
	if move ~= vector.zero then
		local transform = self.Entity.Transform
		transform.Translation = transform.Translation + move * (SPEED * dt)
	end
end

return Script.Define("NavWarden", NavWarden)
