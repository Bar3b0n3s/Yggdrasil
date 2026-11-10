--!strict
-- Sprites2D.scene's Strider (Docs/Decisions/0032-m21-contract.md decisions 5 and 17): walks along X with the MoveX
-- action and faces its way with FlipX, plays the sheet's Walk clip while it moves and Idle once it stops, and plays the
-- held Jump clip on each press of the Jump action, so the Sprites2D replay's position, frames and clips depend on its
-- input.
export type Strider = ScriptInstance & {
	Jumps: number,
}

local SPEED = 4 -- units per second

local Strider = {}

function Strider.OnCreate(self: Strider)
	self.Jumps = 0
end

function Strider.OnFixedUpdate(self: Strider, dt: number)
	local sprite = self.Entity.SpriteRenderer :: SpriteRenderer
	local animator = self.Entity.SpriteAnimator :: SpriteAnimator
	if Input.IsActionPressed("Jump") then
		self.Jumps += 1
		animator:Play("Jump")
	end
	local jumping = animator.Clip == "Jump" and animator.Playing
	local move = Input.GetAxis("MoveX")
	if move ~= 0 then
		local transform = self.Entity.Transform
		transform.Translation = transform.Translation + vector.create(move * SPEED * dt, 0, 0)
		sprite.FlipX = move < 0
		if animator.Clip ~= "Walk" and not jumping then
			animator:Play("Walk")
		end
	elseif animator.Clip == "Walk" then
		animator:Play("Idle")
	end
end

return Script.Define("Strider", Strider)
