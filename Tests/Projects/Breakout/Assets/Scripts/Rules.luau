--!strict
-- The pure rules of Breakout: scoring, lives, winning, losing, the bounce
-- directions and the ball's speed-up. No engine calls, so the empty-scene
-- suites can test it directly.

export type Phase = "Serve" | "Playing" | "Won" | "GameOver"

export type State = {
	Score: number,
	Lives: number,
	BricksLeft: number,
	Phase: Phase,
	-- Paddle hits since the ball was last served; the ball speeds up with them.
	RallyHits: number,
}

export type BrickResult = "Ignored" | "Broken" | "Won"
export type BallResult = "Ignored" | "LifeLost" | "GameOver"

-- How the ball speeds up during a rally: every HitsPerStep paddle hits add
-- Step metres per second to Base, up to Max.
export type SpeedRule = {
	Base: number,
	HitsPerStep: number,
	Step: number,
	Max: number,
}

local Rules = {}

-- The smallest share of the ball's direction that must point up or down, so
-- the ball never ends up bouncing between the side walls forever.
Rules.MinVertical = 0.3

function Rules.New(brickCount: number, lives: number): State
	return {
		Score = 0,
		Lives = lives,
		BricksLeft = brickCount,
		Phase = "Serve",
		RallyHits = 0,
	}
end

-- Starts play from the serve. Returns whether the ball was launched.
function Rules.Launch(state: State): boolean
	if state.Phase ~= "Serve" then
		return false
	end
	state.Phase = "Playing"
	state.RallyHits = 0
	return true
end

-- Counts a paddle hit of the ball in play. Returns whether it counted.
function Rules.PaddleHit(state: State): boolean
	if state.Phase ~= "Playing" then
		return false
	end
	state.RallyHits += 1
	return true
end

-- Scores a broken brick; the last brick wins the game.
function Rules.BreakBrick(state: State, points: number): BrickResult
	if state.Phase ~= "Playing" or state.BricksLeft <= 0 then
		return "Ignored"
	end
	state.Score += points
	state.BricksLeft -= 1
	if state.BricksLeft == 0 then
		state.Phase = "Won"
		state.RallyHits = 0
		return "Won"
	end
	return "Broken"
end

-- The ball fell below the paddle: one life is lost; the last one ends the game.
function Rules.LoseBall(state: State): BallResult
	if state.Phase ~= "Playing" then
		return "Ignored"
	end
	state.Lives -= 1
	state.RallyHits = 0
	if state.Lives <= 0 then
		state.Lives = 0
		state.Phase = "GameOver"
		return "GameOver"
	end
	state.Phase = "Serve"
	return "LifeLost"
end

-- Starts a new game in place.
function Rules.Restart(state: State, brickCount: number, lives: number)
	state.Score = 0
	state.Lives = lives
	state.BricksLeft = brickCount
	state.Phase = "Serve"
	state.RallyHits = 0
end

-- The ball's speed after rallyHits paddle hits: one Step per completed
-- HitsPerStep hits, never above Max and never below Base.
function Rules.BallSpeed(rule: SpeedRule, rallyHits: number): number
	local steps = math.floor(math.max(rallyHits, 0) / math.max(rule.HitsPerStep, 1))
	local speed = rule.Base + steps * math.max(rule.Step, 0)
	return math.min(speed, math.max(rule.Max, rule.Base))
end

-- The text shown in the middle of the screen in each phase.
function Rules.Message(phase: Phase): string
	if phase == "Serve" then
		return "PRESS SPACE TO LAUNCH"
	elseif phase == "Won" then
		return "YOU WIN!\nPRESS R TO PLAY AGAIN"
	elseif phase == "GameOver" then
		return "GAME OVER\nPRESS R TO RESTART"
	end
	return ""
end

-- The unit direction (x, y) of a ball leaving the paddle: offset is where it
-- hit, from -1 (left edge) to 1 (right edge); the edges give maxAngle degrees
-- from straight up.
function Rules.BounceDirection(offset: number, maxAngle: number): (number, number)
	local angle = math.clamp(offset, -1, 1) * maxAngle * math.pi / 180
	return math.sin(angle), math.cos(angle)
end

-- Normalizes a velocity (vx, vy) to a unit direction whose vertical share is
-- at least minVertical; a ball at rest goes straight up.
function Rules.FixDirection(vx: number, vy: number, minVertical: number): (number, number)
	local length = math.sqrt(vx * vx + vy * vy)
	if length < 1e-6 then
		return 0, 1
	end
	local x, y = vx / length, vy / length
	if math.abs(y) < minVertical then
		y = if y < 0 then -minVertical else minVertical
		local side = math.sqrt(1 - y * y)
		x = if x < 0 then -side else side
	end
	return x, y
end

return Rules
