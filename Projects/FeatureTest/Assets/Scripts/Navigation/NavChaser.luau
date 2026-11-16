--!strict
-- Navigation.scene's chasers (M30; Docs/Decisions/0045-m30-navigation.md decision 13): each finds a path to the Warden
-- with Navigation.FindPath in the fixed step every RepathTicks steps (three times a second at 20), staggered by Offset,
-- and walks its corners at Speed. The husks are the Small agent type, the brute the Large one; Navigation.replay records
-- them chasing the Warden around the lava channel.
export type NavChaser = ScriptInstance & {
	Agent: string,
	Speed: number,
	RepathTicks: number,
	Offset: number,
	Ticks: number,
	Plans: number,
	Complete: number,
	Corners: { vector }?,
	Next: number,
}

local NavChaser = {}

NavChaser.Fields = {
	Agent = Field.String("Small", { Tooltip = "The agent type of the scene's Navigation component" }),
	Speed = Field.Number(3, { Min = 0, Max = 10, Tooltip = "Metres per second" }),
	RepathTicks = Field.Integer(20, { Min = 1, Max = 600, Tooltip = "Fixed steps between two paths" }),
	Offset = Field.Integer(0, { Min = 0, Max = 600, Tooltip = "Fixed steps the first path is staggered by" }),
}

function NavChaser.OnCreate(self: NavChaser)
	self.Ticks = self.Offset
	self.Plans = 0
	self.Complete = 0
	self.Corners = nil
	self.Next = 2
end

local function plan(self: NavChaser)
	local warden = Scene.FindByName("Warden")
	if warden == nil then
		return
	end
	local path, complete = Navigation.FindPath(self.Entity.Transform.Translation, warden.Transform.Translation, self.Agent)
	self.Plans += 1
	if complete then
		self.Complete += 1
	end
	self.Corners = path
	self.Next = 2
end

function NavChaser.OnFixedUpdate(self: NavChaser, dt: number)
	if self.Corners == nil or self.Ticks % self.RepathTicks == 0 then
		plan(self)
	end
	self.Ticks += 1
	local corners = self.Corners
	if corners == nil then
		return
	end
	local transform = self.Entity.Transform
	local position = transform.Translation
	local step = self.Speed * dt
	while step > 0 and self.Next <= #corners do
		local target = corners[self.Next]
		local distance = vector.magnitude(target - position)
		if distance <= step then
			position = target
			step -= distance
			self.Next += 1
		else
			position = position + (target - position) * (step / distance)
			step = 0
		end
	end
	transform.Translation = position
end

return Script.Define("NavChaser", NavChaser)
