--!strict
-- Runs a game of Breakout: moves the paddle, serves and launches the ball,
-- keeps its speed constant between speed-ups (every few paddle hits of a
-- rally, up to a cap), breaks bricks, counts score and lives, plays the
-- sounds and shows the HUD. The rules themselves live in the Rules module.
local Rules = require("./Rules")
local BrickScript = require("./Brick")

export type Game = ScriptInstance & {
	Ball: Entity?,
	Paddle: Entity?,
	BrickRoot: Entity?,
	ScoreText: Entity?,
	LivesText: Entity?,
	MessageText: Entity?,
	BallSpeed: number,
	SpeedUpHits: number,
	SpeedStep: number,
	MaxBallSpeed: number,
	PaddleSpeed: number,
	PaddleLimit: number,
	PaddleHalfWidth: number,
	BallRadius: number,
	DeathY: number,
	StartLives: number,
	MaxBounceAngle: number,
	PaddleHitSound: AssetRef?,
	WallHitSound: AssetRef?,
	BrickBreakSound: AssetRef?,
	LifeLostSound: AssetRef?,
	WinSound: AssetRef?,
	GameOverSound: AssetRef?,

	State: Rules.State,
	Bricks: { Entity },
	PaddleX: number,
	PaddleY: number,
	PaddleHits: number,
	WallHits: number,
	BricksBroken: number,
	LivesLost: number,

	OnBallHit: (self: Game, other: Entity, contact: CollisionContact) -> (),
	BreakBrick: (self: Game, brick: Entity) -> (),
	LoseBall: (self: Game) -> (),
	Launch: (self: Game, offset: number) -> (),
	Restart: (self: Game) -> (),
	ResetBall: (self: Game) -> (),
	CurrentSpeed: (self: Game) -> number,
	SetBallVelocity: (self: Game, x: number, y: number) -> (),
	UpdateHud: (self: Game) -> (),
	Play: (self: Game, clip: AssetRef?) -> (),
}

local Game = {}

Game.Fields = {
	Ball = Field.Entity(),
	Paddle = Field.Entity(),
	BrickRoot = Field.Entity(),
	ScoreText = Field.Entity(),
	LivesText = Field.Entity(),
	MessageText = Field.Entity(),
	BallSpeed = Field.Number(9, { Min = 1, Tooltip = "The ball's speed when served, in metres per second." }),
	SpeedUpHits = Field.Integer(4, { Min = 1, Tooltip = "Paddle hits in one rally per speed-up of the ball." }),
	SpeedStep = Field.Number(1, { Min = 0, Tooltip = "How much faster each speed-up makes the ball, in metres per second." }),
	MaxBallSpeed = Field.Number(13, { Min = 1, Tooltip = "The fastest the ball gets, in metres per second." }),
	PaddleSpeed = Field.Number(14, { Min = 1, Tooltip = "The paddle's speed, in metres per second." }),
	PaddleLimit = Field.Number(7.3, { Min = 0, Tooltip = "How far the paddle centre may move from x = 0." }),
	PaddleHalfWidth = Field.Number(1.2, { Min = 0.1, Tooltip = "Half the paddle's width, in metres." }),
	BallRadius = Field.Number(0.25, { Min = 0.05, Tooltip = "The ball's radius, in metres." }),
	DeathY = Field.Number(-8.5, { Tooltip = "The ball is lost once its centre falls below this height." }),
	StartLives = Field.Integer(3, { Min = 1, Tooltip = "The lives of a new game." }),
	MaxBounceAngle = Field.Number(60, { Min = 0, Max = 75, Tooltip = "The bounce angle from vertical at the paddle's edges, in degrees." }),
	PaddleHitSound = Field.Asset("AudioClip"),
	WallHitSound = Field.Asset("AudioClip"),
	BrickBreakSound = Field.Asset("AudioClip"),
	LifeLostSound = Field.Asset("AudioClip"),
	WinSound = Field.Asset("AudioClip"),
	GameOverSound = Field.Asset("AudioClip"),
}

-- The ball's centre sits this far above the paddle's centre while serving.
local ServeHeight = 0.5

-- The launch leaves the paddle this far towards its right (or left) edge.
local LaunchOffset = 0.35

local function setText(entity: Entity?, value: string)
	if entity then
		local text = entity.Text
		if text and text.Text ~= value then
			text.Text = value
		end
	end
end

function Game.Play(self: Game, clip: AssetRef?)
	if clip then
		Audio.PlayOneShot(clip)
	end
end

function Game.UpdateHud(self: Game)
	setText(self.ScoreText, "SCORE " .. tostring(self.State.Score))
	setText(self.LivesText, "LIVES " .. tostring(self.State.Lives))
	setText(self.MessageText, Rules.Message(self.State.Phase))
end

-- The speed the ball keeps now: the serve speed plus the rally's speed-ups.
function Game.CurrentSpeed(self: Game): number
	return Rules.BallSpeed({
		Base = self.BallSpeed,
		HitsPerStep = self.SpeedUpHits,
		Step = self.SpeedStep,
		Max = self.MaxBallSpeed,
	}, self.State.RallyHits)
end

function Game.SetBallVelocity(self: Game, x: number, y: number)
	local ball = self.Ball
	if ball then
		local body = ball.RigidBody
		if body then
			local speed = self:CurrentSpeed()
			body:SetLinearVelocity(vector.create(x * speed, y * speed, 0))
		end
	end
end

-- Parks the ball on the paddle, at rest.
function Game.ResetBall(self: Game)
	local ball = self.Ball
	if ball then
		local body = ball.RigidBody
		if body then
			body:Teleport(vector.create(self.PaddleX, self.PaddleY + ServeHeight, 0))
			body:SetLinearVelocity(vector.create(0, 0, 0))
		end
	end
end

function Game.Launch(self: Game, offset: number)
	if not Rules.Launch(self.State) then
		return
	end
	local x, y = Rules.BounceDirection(offset, self.MaxBounceAngle)
	self:SetBallVelocity(x, y)
	self:UpdateHud()
end

function Game.BreakBrick(self: Game, brick: Entity)
	if not brick:IsValid() or not brick:IsActiveSelf() then
		return
	end
	local script = brick:GetScript() :: BrickScript.Brick?
	local points = if script then script.Points else 1
	local result = Rules.BreakBrick(self.State, points)
	if result == "Ignored" then
		return
	end
	brick:SetActive(false)
	self.BricksBroken += 1
	if result == "Won" then
		self:ResetBall()
		self:Play(self.WinSound)
	else
		self:Play(self.BrickBreakSound)
	end
	self:UpdateHud()
end

function Game.LoseBall(self: Game)
	local result = Rules.LoseBall(self.State)
	if result == "Ignored" then
		return
	end
	self.LivesLost += 1
	self:ResetBall()
	if result == "GameOver" then
		self:Play(self.GameOverSound)
	else
		self:Play(self.LifeLostSound)
	end
	self:UpdateHud()
end

function Game.Restart(self: Game)
	for _, brick in ipairs(self.Bricks) do
		if brick:IsValid() and not brick:IsActiveSelf() then
			brick:SetActive(true)
		end
	end
	Rules.Restart(self.State, #self.Bricks, self.StartLives)
	self.PaddleX = 0
	local paddle = self.Paddle
	if paddle then
		local body = paddle.RigidBody
		if body then
			body:Teleport(vector.create(self.PaddleX, self.PaddleY, 0))
		end
	end
	self:ResetBall()
	self:UpdateHud()
end

-- Called by the Ball behaviour when the ball starts touching something.
function Game.OnBallHit(self: Game, other: Entity, contact: CollisionContact)
	if self.State.Phase ~= "Playing" then
		return
	end
	if other:HasTag("Paddle") then
		local ball = self.Ball
		if not ball then
			return
		end
		-- Count the hit first, so a speed-up applies to this bounce.
		Rules.PaddleHit(self.State)
		local ballPosition = ball.Transform.WorldPosition
		local paddlePosition = other.Transform.WorldPosition
		-- Only the top of the paddle steers; a ball already below it is lost.
		if ballPosition.y > paddlePosition.y then
			local offset = (ballPosition.x - paddlePosition.x) / (self.PaddleHalfWidth + self.BallRadius)
			local x, y = Rules.BounceDirection(offset, self.MaxBounceAngle)
			self:SetBallVelocity(x, y)
		end
		self.PaddleHits += 1
		self:Play(self.PaddleHitSound)
	elseif other:HasTag("Brick") then
		self:BreakBrick(other)
	elseif other:HasTag("Wall") then
		self.WallHits += 1
		self:Play(self.WallHitSound)
	end
end

function Game.OnStart(self: Game)
	self.Bricks = {}
	local root = self.BrickRoot
	if root then
		for _, child in ipairs(root:GetChildren()) do
			if child:HasTag("Brick") then
				table.insert(self.Bricks, child)
			end
		end
	end
	local paddle = self.Paddle
	local paddlePosition = if paddle then paddle.Transform.WorldPosition else vector.create(0, -6.5, 0)
	self.PaddleX = paddlePosition.x
	self.PaddleY = paddlePosition.y
	self.PaddleHits = 0
	self.WallHits = 0
	self.BricksBroken = 0
	self.LivesLost = 0
	self.State = Rules.New(#self.Bricks, self.StartLives)
	self:ResetBall()
	self:UpdateHud()
end

function Game.OnFixedUpdate(self: Game, dt: number)
	if Input.IsActionPressed("Restart") then
		self:Restart()
		return
	end

	local axis = Input.GetAxis("MoveX")
	self.PaddleX = math.clamp(self.PaddleX + axis * self.PaddleSpeed * dt, -self.PaddleLimit, self.PaddleLimit)
	local paddle = self.Paddle
	if paddle then
		local body = paddle.RigidBody
		if body then
			body:MoveKinematic(vector.create(self.PaddleX, self.PaddleY, 0), Quat.Identity())
		end
	end

	local phase = self.State.Phase
	if phase == "Playing" then
		local ball = self.Ball
		if ball then
			if ball.Transform.WorldPosition.y < self.DeathY then
				self:LoseBall()
			else
				local body = ball.RigidBody
				if body then
					local velocity = body:GetLinearVelocity()
					local x, y = Rules.FixDirection(velocity.x, velocity.y, Rules.MinVertical)
					self:SetBallVelocity(x, y)
				end
			end
		end
	else
		-- Serving, won or over: the ball rests on the paddle and follows it.
		self:ResetBall()
		if phase == "Serve" and Input.IsActionPressed("Launch") then
			self:Launch(if axis < 0 then -LaunchOffset else LaunchOffset)
		end
	end
end

return Script.Define("Game", Game)
