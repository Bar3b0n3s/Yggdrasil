--!strict
-- One game of Tetris as pure, tick-driven logic: spawning, SRS rotation, DAS/ARR shifting, gravity by level,
-- soft and hard drops, lock delay, hold, the next queue, line clears, scoring, levels and game over.
-- No engine calls: the Game behaviour feeds it Controls every tick and mirrors its state into the scene.
local Bag = require("./Bag")
local Board = require("./Board")
local Pieces = require("./Pieces")

-- The player's controls for one tick: held states and the presses of this tick.
export type Controls = {
	Left: boolean,
	Right: boolean,
	LeftPressed: boolean,
	RightPressed: boolean,
	SoftDrop: boolean,
	HardDrop: boolean,
	RotateCW: boolean,
	RotateCCW: boolean,
	Hold: boolean,
}

export type Match = {
	Board: Board.Board,
	Bag: Bag.Bag,
	-- The falling piece: its name, rotation state (0 to 3), box left column and box top row.
	Piece: string,
	State: number,
	Left: number,
	Top: number,
	Hold: string,
	HoldUsed: boolean,
	Queue: { string },
	Score: number,
	Lines: number,
	Level: number,
	GameOver: boolean,
	LockedCount: number,
	LastClear: number,
	LastClearPoints: number,
	GravityTimer: number,
	LockTimer: number,
	LockResets: number,
	LowestTop: number,
	ShiftDirection: number,
	ShiftTimer: number,
	-- What happened during the last Step, Reset or method call, in order (sounds and HUD read it).
	Events: { string },
	Step: (self: Match, controls: Controls) -> (),
	Move: (self: Match, columns: number) -> boolean,
	Rotate: (self: Match, direction: number) -> boolean,
	HardDrop: (self: Match) -> number,
	HoldPiece: (self: Match) -> boolean,
	SpawnPiece: (self: Match, name: string) -> boolean,
	DropDistance: (self: Match) -> number,
	GravityTicks: (self: Match) -> number,
	Reset: (self: Match) -> (),
}

local Match = {}

-- Ticks per row of gravity at 60 Hz, by level; levels past the end use the last entry.
Match.GravityByLevel = table.freeze({ 48, 43, 38, 33, 28, 23, 18, 13, 8, 6, 5, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1 })
-- Points per clear of 1, 2, 3 and 4 lines, multiplied by the level.
Match.LineScores = table.freeze({ 100, 300, 500, 800 })
Match.LinesPerLevel = 10
Match.SoftDropTicks = 2
Match.LockDelay = 30
Match.MaxLockResets = 15
Match.DasDelay = 10
Match.ArrTicks = 2
Match.PreviewCount = 3
Match.SpawnTop = 22

local function emit(self: Match, event: string)
	table.insert(self.Events, event)
end

local function fitsAt(self: Match, left: number, top: number): boolean
	return self.Board:Fits(self.Piece, self.State, left, top)
end

local function endGame(self: Match)
	if not self.GameOver then
		self.GameOver = true
		emit(self, "GameOver")
	end
end

-- After a successful move or rotation: a grounded piece gets its lock delay back, at most MaxLockResets times per row.
local function afterShift(self: Match)
	if not fitsAt(self, self.Left, self.Top - 1) and self.LockResets < Match.MaxLockResets then
		self.LockTimer = 0
		self.LockResets += 1
	end
end

local function spawnPiece(self: Match, name: string): boolean
	local shape = Pieces.Get(name)
	self.Piece = name
	self.State = 0
	self.Left = shape.SpawnLeft
	self.Top = Match.SpawnTop
	self.GravityTimer = 0
	self.LockTimer = 0
	self.LockResets = 0
	if not fitsAt(self, self.Left, self.Top) then
		endGame(self)
		return false
	end
	-- Pieces spawn in rows 21 and 22 and drop one row at once when nothing blocks them.
	if fitsAt(self, self.Left, self.Top - 1) then
		self.Top -= 1
	end
	self.LowestTop = self.Top
	return true
end

local function takeNext(self: Match): string
	local name = table.remove(self.Queue, 1) :: string
	table.insert(self.Queue, self.Bag:Draw())
	return name
end

local function spawnNext(self: Match)
	self.HoldUsed = false
	spawnPiece(self, takeNext(self))
end

local function lock(self: Match)
	-- Place reports a top-out: a lock out (every block above the visible rows) or a block above the well.
	local toppedOut = self.Board:Place(self.Piece, self.State, self.Left, self.Top)
	self.LockedCount += 1
	emit(self, "Lock")
	local cleared = self.Board:ClearLines()
	if cleared > 0 then
		local points = Match.LineScores[cleared] * self.Level
		self.Score += points
		self.Lines += cleared
		self.LastClear = cleared
		self.LastClearPoints = points
		emit(self, `Clear{cleared}`)
		local level = 1 + self.Lines // Match.LinesPerLevel
		if level > self.Level then
			self.Level = level
			emit(self, "LevelUp")
		end
	end
	if toppedOut then
		endGame(self)
		return
	end
	spawnNext(self)
end

local function move(self: Match, columns: number): boolean
	if self.GameOver or not fitsAt(self, self.Left + columns, self.Top) then
		return false
	end
	self.Left += columns
	emit(self, "Move")
	afterShift(self)
	return true
end

-- Rotates clockwise (1) or counter-clockwise (-1), trying each SRS kick in order.
local function rotate(self: Match, direction: number): boolean
	if self.GameOver or self.Piece == "O" then
		return false
	end
	local from = self.State
	local to = (from + direction) % 4
	for _, kick in Pieces.Kicks(self.Piece, from, to) do
		local left, top = self.Left + kick[1], self.Top + kick[2]
		if self.Board:Fits(self.Piece, to, left, top) then
			self.State = to
			self.Left = left
			self.Top = top
			emit(self, "Rotate")
			afterShift(self)
			return true
		end
	end
	return false
end

local function dropDistance(self: Match): number
	local distance = 0
	while fitsAt(self, self.Left, self.Top - distance - 1) do
		distance += 1
	end
	return distance
end

-- Drops the piece to the floor and locks it there; returns the rows it fell.
local function hardDrop(self: Match): number
	if self.GameOver then
		return 0
	end
	local distance = dropDistance(self)
	self.Top -= distance
	emit(self, "HardDrop")
	lock(self)
	return distance
end

-- Swaps the falling piece with the held one (or the next one when nothing is held), once per piece.
local function holdPiece(self: Match): boolean
	if self.GameOver or self.HoldUsed then
		return false
	end
	local current = self.Piece
	local incoming = if self.Hold == "" then takeNext(self) else self.Hold
	self.Hold = current
	emit(self, "Hold")
	spawnPiece(self, incoming)
	self.HoldUsed = true
	return true
end

local function gravityTicks(self: Match): number
	local table_ = Match.GravityByLevel
	return table_[math.min(self.Level, #table_)]
end

local function shift(self: Match, controls: Controls)
	if controls.LeftPressed then
		self.ShiftDirection = -1
		self.ShiftTimer = 0
		move(self, -1)
		return
	elseif controls.RightPressed then
		self.ShiftDirection = 1
		self.ShiftTimer = 0
		move(self, 1)
		return
	end
	local held = (self.ShiftDirection == -1 and controls.Left) or (self.ShiftDirection == 1 and controls.Right)
	if not held then
		self.ShiftDirection = if controls.Left then -1 elseif controls.Right then 1 else 0
		self.ShiftTimer = 0
		return
	end
	self.ShiftTimer += 1
	if self.ShiftTimer >= Match.DasDelay and (self.ShiftTimer - Match.DasDelay) % Match.ArrTicks == 0 then
		move(self, self.ShiftDirection)
	end
end

local function step(self: Match, controls: Controls)
	table.clear(self.Events)
	if self.GameOver then
		return
	end
	if controls.Hold then
		holdPiece(self)
		if self.GameOver then
			return
		end
	end
	if controls.RotateCW then
		rotate(self, 1)
	end
	if controls.RotateCCW then
		rotate(self, -1)
	end
	shift(self, controls)
	if controls.HardDrop then
		hardDrop(self)
		return
	end

	local interval = gravityTicks(self)
	if controls.SoftDrop then
		interval = math.min(interval, Match.SoftDropTicks)
	end
	self.GravityTimer += 1
	if self.GravityTimer >= interval then
		self.GravityTimer = 0
		if fitsAt(self, self.Left, self.Top - 1) then
			self.Top -= 1
			if self.Top < self.LowestTop then
				self.LowestTop = self.Top
				self.LockResets = 0
			end
			self.LockTimer = 0
		end
	end

	if fitsAt(self, self.Left, self.Top - 1) then
		self.LockTimer = 0
	else
		self.LockTimer += 1
		if self.LockTimer >= Match.LockDelay then
			lock(self)
		end
	end
end

-- Starts a fresh game on the same generator: an empty well, a new bag and the score, lines and level reset.
local function reset(self: Match)
	table.clear(self.Events)
	self.Board:Clear()
	self.Bag:Reset()
	table.clear(self.Queue)
	for _ = 1, Match.PreviewCount do
		table.insert(self.Queue, self.Bag:Draw())
	end
	self.Hold = ""
	self.Score = 0
	self.Lines = 0
	self.Level = 1
	self.GameOver = false
	self.LockedCount = 0
	self.LastClear = 0
	self.LastClearPoints = 0
	self.ShiftDirection = 0
	self.ShiftTimer = 0
	emit(self, "Restart")
	spawnNext(self)
end

function Match.New(generator: RandomGenerator): Match
	local match: Match = {
		Board = Board.New(),
		Bag = Bag.New(generator),
		Piece = "",
		State = 0,
		Left = 0,
		Top = 0,
		Hold = "",
		HoldUsed = false,
		Queue = {},
		Score = 0,
		Lines = 0,
		Level = 1,
		GameOver = false,
		LockedCount = 0,
		LastClear = 0,
		LastClearPoints = 0,
		GravityTimer = 0,
		LockTimer = 0,
		LockResets = 0,
		LowestTop = 0,
		ShiftDirection = 0,
		ShiftTimer = 0,
		Events = {},
		Step = step,
		Move = move,
		Rotate = rotate,
		HardDrop = hardDrop,
		HoldPiece = holdPiece,
		SpawnPiece = spawnPiece,
		DropDistance = dropDistance,
		GravityTicks = gravityTicks,
		Reset = reset,
	}
	reset(match)
	table.clear(match.Events)
	return match
end

-- Controls with nothing held or pressed, for callers that fill them in each tick.
function Match.NewControls(): Controls
	return {
		Left = false,
		Right = false,
		LeftPressed = false,
		RightPressed = false,
		SoftDrop = false,
		HardDrop = false,
		RotateCW = false,
		RotateCCW = false,
		Hold = false,
	}
end

return Match
