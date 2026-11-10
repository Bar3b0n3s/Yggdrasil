--!strict
-- Plays and blends the walk of SkinnedRigs.gltf from input, with root motion Apply (Animation.replay, M23;
-- Docs/Decisions/0034-m23-contract.md decision 33): Jump restarts the walk on layer 1 and starts the Animator's layers;
-- MoveX fades in a second walk layer, half a cycle ahead, by its magnitude. Without input the Walker stands still.
export type Director = ScriptInstance & {
	Walk: AssetRef?,
	Starts: number,
}

local Director = {}

Director.Fields = {
	Walk = Field.Asset("AnimationClip"),
}

function Director.OnCreate(self: Director)
	self.Starts = 0
end

function Director.OnFixedUpdate(self: Director, dt: number)
	local animator = self.Entity.Animator
	local walk = self.Walk
	if animator == nil or walk == nil then
		return
	end
	if Input.IsActionPressed("Jump") then
		self.Starts += 1
		animator:Play(1, walk, 0)
		animator.Playing = true
	end
	local blend = math.clamp(math.abs(Input.GetAxis("MoveX")), 0, 1)
	if blend > 0 and #animator.Layers < 2 then
		animator:Play(2, walk, 0.5)
	end
	if #animator.Layers >= 2 then
		animator:SetWeight(2, blend)
	end
end

return Script.Define("Director", Director)
