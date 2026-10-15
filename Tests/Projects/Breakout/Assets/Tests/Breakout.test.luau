--!strict
-- The rules of Breakout played in the Main scene: serving, launching, the
-- paddle, scoring, losing lives, game over, winning, restarting and the
-- ball's speed-up.
local GameScript = require("../Scripts/Game")
local BrickScript = require("../Scripts/Brick")

-- Where the paddle keeps its centre relative to the ball while returning it,
-- so the bounces leave at an angle.
local AimOffset = 0.4

local function getGame(): GameScript.Game
	local entity = Scene.FindByName("Game")
	if entity == nil then
		Test.Fail("the scene has no Game entity")
	end
	return (entity :: Entity):GetScript() :: GameScript.Game
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

local function launch(game: GameScript.Game)
	Test.InjectAction("Launch", "Tap")
	Test.WaitUntil(function(): boolean
		return game.State.Phase == "Playing"
	end, 10)
end

-- Test setup: puts the ball below the paddle and waits until the game has
-- noticed that it fell out.
local function dropBall(game: GameScript.Game)
	local ball = game.Ball :: Entity
	local body = ball.RigidBody :: RigidBody
	body:Teleport(vector.create(0, game.DeathY - 1, 0))
	Test.WaitUntil(function(): boolean
		return game.State.Phase ~= "Playing"
	end, 10)
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

local function ballSpeed(game: GameScript.Game): number
	local ball = game.Ball :: Entity
	local body = ball.RigidBody :: RigidBody
	return vector.magnitude(body:GetLinearVelocity())
end

-- Returns the ball with the paddle, like a player pressing MoveX, until the
-- rally has the given number of paddle hits; fails if the ball is lost.
-- Calls check(game) every tick.
local function returnBall(game: GameScript.Game, hits: number, check: (GameScript.Game) -> ())
	local paddle = game.Paddle :: Entity
	local ball = game.Ball :: Entity
	while game.State.RallyHits < hits do
		if game.State.Phase ~= "Playing" then
			Test.Fail("the ball was lost before the rally reached " .. tostring(hits) .. " paddle hits")
		end
		local dx = ball.Transform.WorldPosition.x - AimOffset - paddle.Transform.WorldPosition.x
		Test.InjectAction("MoveX", nil, math.clamp(dx * 1.5, -1, 1))
		Test.WaitTicks(1)
		check(game)
	end
	Test.InjectAction("MoveX", nil, 0)
end

return Test.Suite("Breakout", function()
	Test.Case("starts serving with full lives and every brick", function()
		local game = getGame()
		Test.WaitTicks(1)
		Test.ExpectEqual(game.State.Phase, "Serve")
		Test.ExpectEqual(game.State.Lives, 3)
		Test.ExpectEqual(game.State.Score, 0)
		Test.ExpectEqual(#game.Bricks, 50, "the grid has 5 rows of 10 bricks")
		Test.ExpectEqual(game.State.BricksLeft, 50)
		Test.ExpectEqual(textOf(game.ScoreText), "SCORE 0")
		Test.ExpectEqual(textOf(game.LivesText), "LIVES 3")
		Test.ExpectEqual(textOf(game.MessageText), "PRESS SPACE TO LAUNCH")
	end)

	Test.Case("launch sends the ball up at a constant speed", function()
		local game = getGame()
		Test.WaitTicks(1)
		Test.ExpectNear(ballSpeed(game), 0, 1e-4, "the served ball rests on the paddle")
		launch(game)
		Test.WaitTicks(20)
		local ball = game.Ball :: Entity
		local body = ball.RigidBody :: RigidBody
		Test.Expect(body:GetLinearVelocity().y > 0, "the ball flies up")
		Test.ExpectNear(ballSpeed(game), game.BallSpeed, 1e-3, "the ball leaves at the serve speed")
		Test.ExpectEqual(textOf(game.MessageText), "")
		-- Sample the speed every 10 ticks while the ball is in play: it bounces
		-- off bricks and walls in that time and keeps its speed, which changes
		-- only with the rally's paddle hits.
		local samples = 0
		while game.State.Phase == "Playing" and samples < 30 do
			Test.ExpectNear(ballSpeed(game), game:CurrentSpeed(), 1e-3, "the ball keeps its speed")
			Test.ExpectNear(ball.Transform.WorldPosition.z, 0, 1e-4, "the ball stays in the play plane")
			samples += 1
			Test.WaitTicks(10)
		end
		Test.Expect(samples >= 10, "the ball stays in play long enough to sample its speed")
		Test.Expect(game.BricksBroken > 0, "the sampled flight includes a brick hit")
	end, { TimeoutTicks = 900 })

	Test.Case("the paddle follows MoveX and stops at the walls", function()
		local game = getGame()
		local paddle = game.Paddle :: Entity
		Test.InjectAction("MoveX", nil, 1)
		Test.WaitTicks(90)
		Test.ExpectNear(game.PaddleX, game.PaddleLimit, 1e-4, "the paddle stops at the right wall")
		Test.ExpectNear(paddle.Transform.WorldPosition.x, game.PaddleLimit, 1e-3)
		Test.InjectAction("MoveX", nil, -1)
		Test.WaitTicks(150)
		Test.ExpectNear(game.PaddleX, -game.PaddleLimit, 1e-4, "the paddle stops at the left wall")
		Test.InjectAction("MoveX", nil, 0)
		Test.WaitTicks(5)
		local ball = game.Ball :: Entity
		Test.ExpectNear(ball.Transform.WorldPosition.x, -game.PaddleLimit, 1e-3, "the served ball follows the paddle")
	end)

	Test.Case("the ball breaks the brick it hits and scores", function()
		local game = getGame()
		launch(game)
		Test.WaitUntil(function(): boolean
			return game.BricksBroken > 0
		end, 240)
		Test.Expect(game.State.Score > 0, "breaking a brick scores")
		Test.ExpectEqual(game.State.BricksLeft, 50 - game.BricksBroken)
		Test.ExpectEqual(activeBricks(game), game.State.BricksLeft, "broken bricks disappear")
		Test.ExpectEqual(textOf(game.ScoreText), "SCORE " .. tostring(game.State.Score))
	end)

	Test.Case("each row scores its own points", function()
		local game = getGame()
		launch(game)
		local expected = { 5, 4, 3, 2, 1 }
		local score = 0
		for row = 1, 5 do
			local brick = Scene.FindByName(string.format("Brick_R%d_C01", row)) :: Entity
			local script = brick:GetScript() :: BrickScript.Brick
			Test.ExpectEqual(script.Points, expected[row])
			game:BreakBrick(brick)
			score += expected[row]
			Test.ExpectEqual(game.State.Score, score)
			Test.Expect(not brick:IsActive(), "a broken brick is gone")
		end
		Test.ExpectEqual(game.State.BricksLeft, 45)
	end)

	Test.Case("losing the ball costs a life and serves again", function()
		local game = getGame()
		launch(game)
		dropBall(game)
		Test.ExpectEqual(game.State.Lives, 2)
		Test.ExpectEqual(game.State.Phase, "Serve")
		Test.ExpectEqual(textOf(game.LivesText), "LIVES 2")
		Test.ExpectEqual(textOf(game.MessageText), "PRESS SPACE TO LAUNCH")
		Test.WaitTicks(2)
		local ball = game.Ball :: Entity
		Test.ExpectNear(ball.Transform.WorldPosition.y, game.PaddleY + 0.5, 1e-3, "the ball is back on the paddle")
	end)

	Test.Case("the game is over when the last life is lost", function()
		local game = getGame()
		for _ = 1, 3 do
			launch(game)
			dropBall(game)
		end
		Test.ExpectEqual(game.State.Lives, 0)
		Test.ExpectEqual(game.State.Phase, "GameOver")
		Test.ExpectEqual(textOf(game.MessageText), "GAME OVER\nPRESS R TO RESTART")
		Test.InjectAction("Launch", "Tap")
		Test.WaitTicks(5)
		Test.ExpectEqual(game.State.Phase, "GameOver", "launch does nothing after the game is over")
	end)

	Test.Case("breaking every brick wins", function()
		local game = getGame()
		launch(game)
		for _, brick in ipairs(game.Bricks) do
			game:BreakBrick(brick)
		end
		Test.ExpectEqual(game.State.Phase, "Won")
		Test.ExpectEqual(game.State.BricksLeft, 0)
		Test.ExpectEqual(game.State.Score, 10 * (5 + 4 + 3 + 2 + 1))
		Test.ExpectEqual(activeBricks(game), 0)
		Test.ExpectEqual(textOf(game.MessageText), "YOU WIN!\nPRESS R TO PLAY AGAIN")
		Test.WaitTicks(2)
		Test.ExpectNear(ballSpeed(game), 0, 1e-4, "the ball stops on the paddle after the win")
	end)

	Test.Case("restart after game over starts a new game", function()
		local game = getGame()
		launch(game)
		game:BreakBrick(Scene.FindByName("Brick_R1_C05") :: Entity)
		for _ = 1, 3 do
			if game.State.Phase == "Serve" then
				launch(game)
			end
			dropBall(game)
		end
		Test.ExpectEqual(game.State.Phase, "GameOver")
		Test.InjectAction("Restart", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(game.State.Phase, "Serve")
		Test.ExpectEqual(game.State.Lives, 3)
		Test.ExpectEqual(game.State.Score, 0)
		Test.ExpectEqual(game.State.BricksLeft, 50)
		Test.ExpectEqual(activeBricks(game), 50, "every brick is back")
		Test.ExpectEqual(textOf(game.ScoreText), "SCORE 0")
		Test.ExpectEqual(textOf(game.LivesText), "LIVES 3")
		Test.ExpectNear(game.PaddleX, 0, 1e-6, "the paddle is centred again")
	end)

	Test.Case("restart after a win restores every brick", function()
		local game = getGame()
		launch(game)
		for _, brick in ipairs(game.Bricks) do
			game:BreakBrick(brick)
		end
		Test.ExpectEqual(game.State.Phase, "Won")
		Test.InjectAction("Restart", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(game.State.Phase, "Serve")
		Test.ExpectEqual(activeBricks(game), 50)
		launch(game)
		Test.WaitUntil(function(): boolean
			return game.BricksBroken > 50
		end, 240)
		Test.Expect(game.State.Score > 0, "restored bricks break and score again")
	end)

	Test.Case("restart during play resets the score, lives and bricks", function()
		local game = getGame()
		launch(game)
		Test.WaitUntil(function(): boolean
			return game.BricksBroken > 0
		end, 240)
		dropBall(game)
		Test.ExpectEqual(game.State.Lives, 2, "the dropped ball cost a life")
		launch(game)
		Test.WaitTicks(10)
		Test.ExpectEqual(game.State.Phase, "Playing")
		Test.Expect(game.State.Score > 0, "the broken brick scored")
		local broken = game.BricksBroken
		Test.InjectAction("Restart", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(game.State.Phase, "Serve")
		Test.ExpectEqual(game.State.Lives, game.StartLives)
		Test.ExpectEqual(game.State.Score, 0)
		Test.ExpectEqual(game.State.BricksLeft, 50)
		Test.ExpectEqual(game.State.RallyHits, 0, "a restart ends the rally")
		Test.ExpectEqual(activeBricks(game), 50, "every broken brick is back")
		Test.ExpectEqual(textOf(game.ScoreText), "SCORE 0")
		Test.ExpectEqual(textOf(game.LivesText), "LIVES 3")
		Test.ExpectEqual(textOf(game.MessageText), "PRESS SPACE TO LAUNCH")
		Test.ExpectNear(game.PaddleX, 0, 1e-6, "the paddle is centred again")
		local ball = game.Ball :: Entity
		Test.ExpectNear(ball.Transform.WorldPosition.y, game.PaddleY + 0.5, 1e-3, "the ball rests on the paddle again")
		Test.ExpectNear(ballSpeed(game), 0, 1e-4, "the ball waits for the serve")
		Test.ExpectEqual(game.LivesLost, 1, "the session counters keep counting across a restart")
		Test.ExpectEqual(game.BricksBroken, broken)
	end)

	Test.Case("the ball speeds up every few paddle hits and a new serve resets it", function()
		local game = getGame()
		launch(game)
		local hits = game.SpeedUpHits
		-- Until the speed-up hit the ball keeps the serve speed.
		returnBall(game, hits - 1, function(g: GameScript.Game)
			if g.State.Phase == "Playing" then
				Test.ExpectNear(ballSpeed(g), g.BallSpeed, 1e-3, "no speed-up before the rally's speed-up hit")
			end
		end)
		returnBall(game, hits, function(_g: GameScript.Game) end)
		Test.WaitTicks(2)
		Test.ExpectEqual(game.State.RallyHits, hits)
		Test.ExpectNear(ballSpeed(game), game.BallSpeed + game.SpeedStep, 1e-3, "the speed-up hit makes the ball faster")
		Test.ExpectNear(game:CurrentSpeed(), game.BallSpeed + game.SpeedStep, 1e-6)
		Test.WaitTicks(30)
		if game.State.Phase == "Playing" and game.State.RallyHits == hits then
			Test.ExpectNear(ballSpeed(game), game.BallSpeed + game.SpeedStep, 1e-3, "the faster ball keeps its speed")
		end

		dropBall(game)
		Test.ExpectEqual(game.State.RallyHits, 0, "a lost ball ends the rally")
		launch(game)
		Test.WaitTicks(5)
		Test.ExpectNear(ballSpeed(game), game.BallSpeed, 1e-3, "the next serve leaves at the serve speed")
	end, { TimeoutTicks = 3000 })

	Test.Case("the ball never gets faster than its top speed", function()
		local game = getGame()
		-- Tuning for this case: a speed-up on every paddle hit and a top speed two steps up.
		game.SpeedUpHits = 1
		game.MaxBallSpeed = game.BallSpeed + 2 * game.SpeedStep
		launch(game)
		local fastest = 0
		returnBall(game, 4, function(g: GameScript.Game)
			fastest = math.max(fastest, ballSpeed(g))
		end)
		Test.WaitTicks(2)
		Test.ExpectNear(ballSpeed(game), game.MaxBallSpeed, 1e-3, "four speed-up hits reach the top speed")
		Test.Expect(fastest <= game.MaxBallSpeed + 1e-3, "the ball never exceeds its top speed")

		Test.InjectAction("Restart", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(game.State.RallyHits, 0, "a restart ends the rally")
		launch(game)
		Test.WaitTicks(5)
		Test.ExpectNear(ballSpeed(game), game.BallSpeed, 1e-3, "a restarted game serves at the serve speed")
	end, { TimeoutTicks = 3000 })
end)
