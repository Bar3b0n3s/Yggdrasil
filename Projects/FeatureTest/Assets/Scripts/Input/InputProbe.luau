--!strict
-- Records what each fixed step and each frame of an Input.scene session reads from Input: keys, mouse buttons, the
-- cursor, wheel and movement, gamepad 0, and the project's actions and axes. The Input suite (Test.Inject*) and
-- Input.replay's expectations compare these records with the input they applied, phase by phase.
--
-- Readings are numbered from 1, the session's first of each phase, so the step reading of tick t is step t + 1. The
-- probe holds only the latest HISTORY readings of each phase, in rings whose tables it refills once full, so a long
-- session (the hardening soak plays this scene for 10,000 ticks) neither grows the script heap nor makes garbage. The
-- suite and the replay read at most a few dozen readings back; reading one the ring has already dropped is an error,
-- never a silent miss.
export type Reading = {
	Tick: number,
	Frame: number,
	SpaceDown: boolean,
	SpacePressed: boolean,
	SpaceReleased: boolean,
	DDown: boolean,
	LeftDown: boolean,
	LeftPressed: boolean,
	LeftReleased: boolean,
	RightDown: boolean,
	MousePosition: vector,
	MouseDelta: vector,
	Scroll: vector,
	PadConnected: boolean,
	SouthDown: boolean,
	LeftX: number,
	LeftY: number,
	RightTrigger: number,
	JumpDown: boolean,
	JumpPressed: boolean,
	JumpReleased: boolean,
	FireDown: boolean,
	FirePressed: boolean,
	FireReleased: boolean,
	MoveX: number,
	MoveY: number,
}

export type InputProbe = ScriptInstance & {
	StepCount: number,
	FrameCount: number,
	StepRing: { Reading },
	FrameRing: { Reading },
	Step: (self: InputProbe, index: number) -> Reading?,
	Frame: (self: InputProbe, index: number) -> Reading?,
	LastStep: (self: InputProbe) -> Reading,
	LastFrame: (self: InputProbe) -> Reading,
	StepAt: (self: InputProbe, tick: number) -> Reading?,
	Count: (self: InputProbe, phase: string, key: string, first: number?) -> number,
}

local HISTORY = 256

local InputProbe = {}

-- Fills `reading` with what Input reports now and returns it.
local function Fill(reading: Reading): Reading
	reading.Tick = Time.GetTick()
	reading.Frame = Time.GetFrameCount()
	reading.SpaceDown = Input.IsKeyDown("Space")
	reading.SpacePressed = Input.IsKeyPressed("Space")
	reading.SpaceReleased = Input.IsKeyReleased("Space")
	reading.DDown = Input.IsKeyDown("D")
	reading.LeftDown = Input.IsMouseButtonDown("Left")
	reading.LeftPressed = Input.IsMouseButtonPressed("Left")
	reading.LeftReleased = Input.IsMouseButtonReleased("Left")
	reading.RightDown = Input.IsMouseButtonDown("Right")
	reading.MousePosition = Input.GetMousePosition()
	reading.MouseDelta = Input.GetMouseDelta()
	reading.Scroll = Input.GetScrollDelta()
	reading.PadConnected = Input.IsGamepadConnected(0)
	reading.SouthDown = Input.IsGamepadButtonDown(0, "South")
	reading.LeftX = Input.GetGamepadAxis(0, "LeftX")
	reading.LeftY = Input.GetGamepadAxis(0, "LeftY")
	reading.RightTrigger = Input.GetGamepadAxis(0, "RightTrigger")
	reading.JumpDown = Input.IsActionDown("Jump")
	reading.JumpPressed = Input.IsActionPressed("Jump")
	reading.JumpReleased = Input.IsActionReleased("Jump")
	reading.FireDown = Input.IsActionDown("Fire")
	reading.FirePressed = Input.IsActionPressed("Fire")
	reading.FireReleased = Input.IsActionReleased("Fire")
	reading.MoveX = Input.GetAxis("MoveX")
	reading.MoveY = Input.GetAxis("MoveY")
	return reading
end

-- Takes reading `count` of a ring into its slot, refilling the table already there once the ring is full.
local function Record(ring: { Reading }, count: number)
	local slot = (count - 1) % HISTORY + 1
	ring[slot] = Fill(ring[slot] or ({} :: any))
end

-- The reading `index` of a ring that took `count` readings, or nil when it was not taken yet.
local function At(ring: { Reading }, count: number, index: number, phase: string): Reading?
	if index < 1 or index > count then
		return nil
	end
	if index <= count - HISTORY then
		error(`InputProbe: {phase} reading {index} was dropped; only the latest {HISTORY} are kept`)
	end
	return ring[(index - 1) % HISTORY + 1]
end

function InputProbe.OnCreate(self: InputProbe)
	self.StepCount = 0
	self.FrameCount = 0
	self.StepRing = {}
	self.FrameRing = {}
end

function InputProbe.OnFixedUpdate(self: InputProbe, dt: number)
	self.StepCount += 1
	Record(self.StepRing, self.StepCount)
end

function InputProbe.OnUpdate(self: InputProbe, dt: number)
	self.FrameCount += 1
	Record(self.FrameRing, self.FrameCount)
end

-- Step reading `index` (1 is the session's first fixed step), or nil when that step has not run yet.
function InputProbe.Step(self: InputProbe, index: number): Reading?
	return At(self.StepRing, self.StepCount, index, "step")
end

-- Frame reading `index` (1 is the session's first frame), or nil when that frame has not run yet.
function InputProbe.Frame(self: InputProbe, index: number): Reading?
	return At(self.FrameRing, self.FrameCount, index, "frame")
end

-- The latest step reading; a session that has run no step yet is an error.
function InputProbe.LastStep(self: InputProbe): Reading
	local reading = self:Step(self.StepCount)
	assert(reading ~= nil, "InputProbe: no step has run yet")
	return reading
end

-- The latest frame reading; a session that has run no frame yet is an error.
function InputProbe.LastFrame(self: InputProbe): Reading
	local reading = self:Frame(self.FrameCount)
	assert(reading ~= nil, "InputProbe: no frame has run yet")
	return reading
end

-- The reading of the step that ran tick `tick`, or nil.
function InputProbe.StepAt(self: InputProbe, tick: number): Reading?
	return self:Step(tick + 1)
end

-- How many readings of `phase` ("Steps" or "Frames") have `key` set, from reading `first` on.
function InputProbe.Count(self: InputProbe, phase: string, key: string, first: number?): number
	local steps = phase == "Steps"
	local total = if steps then self.StepCount else self.FrameCount
	local count = 0
	for index = first or 1, total do
		local reading = if steps then self:Step(index) else self:Frame(index)
		if (reading :: any)[key] then
			count += 1
		end
	end
	return count
end

return Script.Define("InputProbe", InputProbe)
