--!strict
-- The level's flow: the timer carried across levels in whole ticks, the HUD, the banners, the win, the next level's
-- Scene.Load and Restart (ADR 0038 decision 4). It runs after the ball (ExecutionOrder 10), so it sees the tick's state.
local Actions = require("./Actions")
local BallScript = require("./Ball")
local Clock = require("./Clock")

export type Game = ScriptInstance & {
	LevelNumber: number,
	LevelCount: number,
	ParTicks: number,
	NextLevel: AssetRef?,
	FirstLevel: AssetRef?,
	WinSound: AssetRef?,
	Ball: Entity?,
	Goal: Entity?,
	Hud: Entity?,
	LoadDelayTicks: number,
	BannerTicks: number,
	StartTicks: number,
	Ticks: number,
	TotalTicks: number,
	Finished: boolean,
	FinishedTicks: number,
	LoadRequested: boolean,
	Leaving: boolean,
	SentParameters: { [string]: any },
	SeenOrder: number,
	BannerLeft: number,
	ShownTenths: number,
}

local Game = {}

Game.Fields = {
	LevelNumber = Field.Integer(1, { Min = 1 }),
	LevelCount = Field.Integer(3, { Min = 1 }),
	ParTicks = Field.Integer(1800, { Min = 1 }),
	NextLevel = Field.Asset("Scene"),
	FirstLevel = Field.Asset("Scene"),
	WinSound = Field.Asset("AudioClip"),
	Ball = Field.Entity(),
	Goal = Field.Entity(),
	Hud = Field.Entity(),
	LoadDelayTicks = Field.Integer(120, { Min = 1 }),
	BannerTicks = Field.Integer(60, { Min = 1 }),
}

local function hudChild(self: Game, name: string): Entity?
	local hud = self.Hud
	if not hud then
		return nil
	end
	return hud:FindChild(name, true)
end

local function setText(self: Game, name: string, text: string)
	local element = hudChild(self, name)
	local label = if element then element.UiText else nil
	if label then
		label.Text = text
	end
end

local function showTime(self: Game)
	local tenths = self.TotalTicks * 10 // 60
	if tenths ~= self.ShownTenths then
		self.ShownTenths = tenths
		setText(self, "TimeText", Clock.Format(self.TotalTicks))
	end
end

-- Shows the banner with a text, for ticks ticks (0: until the level ends).
local function showBanner(self: Game, text: string, ticks: number)
	setText(self, "BannerText", text)
	local banner = hudChild(self, "Banner")
	if banner then
		banner:SetActive(true)
	end
	self.BannerLeft = if ticks > 0 then ticks else -1
end

local function hideBanner(self: Game)
	local banner = hudChild(self, "Banner")
	if banner then
		banner:SetActive(false)
	end
	self.BannerLeft = 0
end

local function ballScript(self: Game): BallScript.Ball?
	local ball = self.Ball
	if not ball or not ball:IsValid() then
		return nil
	end
	return ball:GetScript("Ball") :: BallScript.Ball?
end

local function finish(self: Game)
	self.Finished = true
	local sound = self.WinSound
	if sound then
		Audio.PlayOneShot(sound)
	end
	local goal = self.Goal
	local burst = if goal then goal:FindChild("WinBurst") else nil
	local emitter = if burst then burst.ParticleEmitter else nil
	if emitter then
		emitter:Emit(150)
	end
	if self.NextLevel then
		showBanner(self, "LEVEL COMPLETE\n" .. Clock.Format(self.Ticks) .. "  PAR " .. Clock.Format(self.ParTicks), 0)
	else
		showBanner(self, "FINISHED  " .. Clock.Format(self.TotalTicks) .. "\nPRESS R TO PLAY AGAIN", 0)
	end
end

function Game.OnStart(self: Game)
	local carried = Scene.GetLoadParameters().ElapsedTicks
	self.StartTicks = if type(carried) == "number" then carried else 0
	self.Ticks = 0
	self.TotalTicks = self.StartTicks
	self.Finished = false
	self.FinishedTicks = 0
	self.LoadRequested = false
	self.Leaving = false
	self.SentParameters = {}
	self.SeenOrder = 0
	self.BannerLeft = 0
	self.ShownTenths = -1
	setText(self, "LevelText", string.format("LEVEL %d/%d", self.LevelNumber, self.LevelCount))
	setText(self, "ParText", "PAR " .. Clock.Format(self.ParTicks))
	showTime(self)
end

function Game.OnFixedUpdate(self: Game, _dt: number)
	-- A transition requested here applies after this tick, and the frame runs no further fixed step, so two requests can
	-- meet only within one tick. Once one is requested (Leaving), nothing more is: a second would raise InvalidState.
	if not self.Leaving and Input.IsActionPressed(Actions.Restart) then
		self.Leaving = true
		local first = self.FirstLevel
		if self.Finished and not self.NextLevel and first then
			Scene.Load(first, {})
		else
			Scene.Reload()
		end
		return
	end

	local ball = ballScript(self)
	if not self.Finished then
		if ball and ball.ReachedGoal then
			finish(self)
		else
			self.Ticks += 1
			self.TotalTicks = self.StartTicks + self.Ticks
			showTime(self)
		end
	else
		self.FinishedTicks += 1
		local nextLevel = self.NextLevel
		if nextLevel and not self.Leaving and self.FinishedTicks >= self.LoadDelayTicks then
			self.SentParameters = { ElapsedTicks = self.TotalTicks }
			Scene.Load(nextLevel, self.SentParameters)
			self.LoadRequested = true
			self.Leaving = true
		end
	end

	-- The countdown runs before a new banner shows, so a banner stays up for exactly its BannerTicks ticks.
	if self.BannerLeft > 0 then
		self.BannerLeft -= 1
		if self.BannerLeft == 0 then
			hideBanner(self)
		end
	end
	if ball and ball.LastOrder > self.SeenOrder then
		self.SeenOrder = ball.LastOrder
		if not self.Finished then
			showBanner(self, "CHECKPOINT", self.BannerTicks)
		end
	end
end

return Script.Define("Game", Game)
