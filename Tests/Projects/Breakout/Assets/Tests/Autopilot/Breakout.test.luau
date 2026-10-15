--!strict
-- Test-only autopilot that plays Breakout like a player, for recording the
-- Opening replay: it only reads the scene and injects the MoveX and Launch
-- actions. It keeps the ball in play off the paddle until the rally has sped
-- the ball up and bricks have broken, then lets the ball fall to lose a life,
-- serves again and returns the ball once more. It ends with the ball in play
-- and the played state still in the scene (broken bricks, the score and the
-- lost life on the HUD), so the recording depends on its input; restarts are
-- covered by the Breakout suite.
local GameScript = require("../../Scripts/Game")

-- Proportional steering gain of the paddle and where the paddle aims under
-- the ball, so the bounces leave at an angle.
local Gain = 1.5
local AimOffset = 0.4

local function getGame(): GameScript.Game
	local entity = Scene.FindByName("Game")
	if entity == nil then
		Test.Fail("the scene has no Game entity")
	end
	return (entity :: Entity):GetScript() :: GameScript.Game
end

local function ballX(game: GameScript.Game): number
	return (game.Ball :: Entity).Transform.WorldPosition.x
end

local function ballSpeed(game: GameScript.Game): number
	local body = (game.Ball :: Entity).RigidBody :: RigidBody
	return vector.magnitude(body:GetLinearVelocity())
end

local function textOf(entity: Entity?): string
	if entity then
		local text = entity.Text
		if text then
			return text.Text
		end
	end
	return ""
end

local function activeBricks(game: GameScript.Game): number
	local count = 0
	for _, brick in ipairs(game.Bricks) do
		if brick:IsActive() then
			count += 1
		end
	end
	return count
end

-- Pushes MoveX towards the paddle position x for one tick.
local function steer(game: GameScript.Game, x: number)
	local paddle = game.Paddle :: Entity
	local dx = x - paddle.Transform.WorldPosition.x
	Test.InjectAction("MoveX", nil, math.clamp(dx * Gain, -1, 1))
	Test.WaitTicks(1)
end

local function serve()
	Test.InjectAction("Launch", "Tap")
	Test.WaitTicks(2)
end

-- Follows the ball with the paddle until done() holds, serving again
-- whenever a ball is lost.
local function rally(game: GameScript.Game, done: () -> boolean)
	while not done() do
		if game.State.Phase == "Serve" then
			serve()
		else
			steer(game, ballX(game) - AimOffset)
		end
	end
end

return Test.Suite("Autopilot.Breakout", function()
	Test.Case("hits, speeds up, breaks bricks and loses a life", function()
		local game = getGame()
		Test.WaitTicks(30)
		serve()
		rally(game, function(): boolean
			return game.State.RallyHits >= game.SpeedUpHits and game.BricksBroken >= 3
		end)
		Test.WaitTicks(2)
		Test.Expect(ballSpeed(game) > game.BallSpeed + 0.5 * game.SpeedStep, "the rally sped the ball up")

		-- Let the ball go: keep the paddle far from it until a life is lost.
		local lost = game.LivesLost
		while game.LivesLost == lost do
			local x = ballX(game)
			local paddleX = (game.Paddle :: Entity).Transform.WorldPosition.x
			steer(game, if x > paddleX then x - 6 else x + 6)
		end
		Test.InjectAction("MoveX", nil, 0)
		Test.WaitTicks(30)
		Test.ExpectEqual(game.State.RallyHits, 0, "the lost ball ended the rally")

		-- Serve again and return the ball once more, then end with the ball in play.
		local hits = game.PaddleHits
		rally(game, function(): boolean
			return game.PaddleHits > hits
		end)
		Test.InjectAction("MoveX", nil, 0)
		Test.WaitTicks(20)

		Test.Expect(game.PaddleHits > game.SpeedUpHits, "the ball came off the paddle")
		Test.Expect(game.BricksBroken >= 3, "bricks broke")
		Test.ExpectEqual(game.LivesLost, 1, "one life was lost")
		Test.ExpectEqual(game.State.Phase, "Playing", "the recording ends with the ball in play")
		Test.ExpectEqual(game.State.Lives, game.StartLives - 1)
		Test.Expect(game.State.Score > 0, "the broken bricks scored")
		Test.ExpectEqual(game.State.BricksLeft, #game.Bricks - game.BricksBroken)
		Test.ExpectEqual(activeBricks(game), game.State.BricksLeft, "the broken bricks are gone from the scene")
		Test.ExpectEqual(textOf(game.ScoreText), "SCORE " .. tostring(game.State.Score))
		Test.ExpectEqual(textOf(game.LivesText), "LIVES " .. tostring(game.State.Lives))
	end, { TimeoutTicks = 7200 })
end)
