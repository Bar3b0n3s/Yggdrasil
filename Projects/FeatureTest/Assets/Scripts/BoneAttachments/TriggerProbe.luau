--!strict
-- Records the triggers its entity's sensor enters (BoneAttachments.scene's Sword; M41,
-- Docs/Decisions/0058-m41-bone-attachments.md decision 3): the first other entity's name and where its own entity stood
-- then, the pose the AnimationSystem phase's attaching pass gave it in that tick.
export type TriggerProbe = ScriptInstance & {
	Count: number,
	Other: string,
	Position: vector,
}

local TriggerProbe = {}

function TriggerProbe.OnCreate(self: TriggerProbe)
	self.Count = 0
	self.Other = ""
	self.Position = vector.zero
end

function TriggerProbe.OnTriggerEnter(self: TriggerProbe, other: Entity)
	self.Count += 1
	if self.Count == 1 then
		self.Other = other.Name
		self.Position = self.Entity.Transform.WorldPosition
	end
end

return Script.Define("TriggerProbe", TriggerProbe)
