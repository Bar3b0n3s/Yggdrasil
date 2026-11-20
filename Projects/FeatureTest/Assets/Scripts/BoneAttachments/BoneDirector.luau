--!strict
-- Drives BoneAttachments.scene from input (BoneAttachments.replay; M41, Docs/Decisions/0058-m41-bone-attachments.md
-- decision 3): Jump starts the Walker's walk, whose root motion Apply carries the Pack on its Spine and the Rider on its
-- Hips; Fire folds the Waver's Sword from its fist (Joint1) onto Joint0, and back. Written in the fixed step, each switch is
-- in force at that tick's AnimationSystem phase. Without input nothing changes.
export type BoneDirector = ScriptInstance & {
	Folds: number,
}

local BoneDirector = {}

function BoneDirector.OnCreate(self: BoneDirector)
	self.Folds = 0
end

function BoneDirector.OnFixedUpdate(self: BoneDirector, dt: number)
	if Input.IsActionPressed("Jump") then
		local walker = Scene.FindByPath("/Walker/Character")
		local animator = if walker then walker.Animator else nil
		if animator then
			animator.Playing = true
		end
	end
	if Input.IsActionPressed("Fire") then
		local sword = Scene.FindByPath("/Waver/Armature/Sword")
		local attachment = if sword then sword.BoneAttachment else nil
		if attachment then
			self.Folds += 1
			attachment.Bone = if attachment.Bone == "Joint1" then "Joint0" else "Joint1"
		end
	end
end

return Script.Define("BoneDirector", BoneDirector)
