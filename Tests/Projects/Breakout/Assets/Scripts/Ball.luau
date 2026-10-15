--!strict
-- The ball: reports every collision it starts to the Game behaviour, which
-- steers it off the paddle, breaks bricks and plays the sounds.
local GameScript = require("./Game")

export type Ball = ScriptInstance & {
	Game: Entity?,
}

local Ball = {}

Ball.Fields = {
	Game = Field.Entity(),
}

function Ball.OnCollisionEnter(self: Ball, other: Entity, contact: CollisionContact)
	local gameEntity = self.Game
	if gameEntity then
		local game = gameEntity:GetScript() :: GameScript.Game?
		if game then
			game:OnBallHit(other, contact)
		end
	end
end

return Script.Define("Ball", Ball)
