--!strict
-- Records the first trigger its entity's sensor enters (BoneAttachments.scene's Sword; M41,
-- Docs/Decisions/0058-m41-bone-attachments.md decision 3) and where the bone held the entity in the two ticks before the
-- contact. Each OnFixedUpdate runs before its tick's AnimationSystem phase, so it reads the pose the last phase placed the
-- entity at, taken from the bone (the Animator's GetJointTransform with the BoneAttachment's Offset and Rotation), never
-- from the entity's own Transform; it stops at the first contact.
type Pose = { Position: vector, Rotation: Quat }

export type TriggerProbe = ScriptInstance & {
	Count: number,
	Other: string,
	-- The bone-held pose of the tick before the contact's tick, and of the tick before that.
	Last: Pose?,
	Earlier: Pose?,
}

local TriggerProbe = {}

function TriggerProbe.OnCreate(self: TriggerProbe)
	self.Count = 0
	self.Other = ""
	self.Last = nil
	self.Earlier = nil
end

function TriggerProbe.OnFixedUpdate(self: TriggerProbe, dt: number)
	if self.Count > 0 then
		return
	end
	local attachment = self.Entity.BoneAttachment
	local parent = self.Entity:GetParent()
	local animator = if parent then parent.Animator else nil
	if attachment == nil or animator == nil then
		return
	end
	local position, rotation = animator:GetJointTransform(attachment.Bone)
	if position == nil or rotation == nil then
		return
	end
	local turn = rotation :: Quat
	self.Earlier = self.Last
	self.Last = { Position = (position :: vector) + turn * attachment.Offset, Rotation = turn * attachment.Rotation }
end

function TriggerProbe.OnTriggerEnter(self: TriggerProbe, other: Entity)
	self.Count += 1
	if self.Count == 1 then
		self.Other = other.Name
	end
end

return Script.Define("TriggerProbe", TriggerProbe)
