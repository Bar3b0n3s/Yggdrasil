--!strict
-- Records what each fixed step and each frame of an Input.scene session reads from Input: keys, mouse buttons, the
-- cursor, wheel and movement, gamepad 0, and the project's actions and axes. The Input suite (Test.Inject*) and
-- Input.replay's expectations compare these records with the input they applied, phase by phase.
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
	Steps: { Reading },
	Frames: { Reading },
	StepAt: (self: InputProbe, tick: number) -> Reading?,
	Count: (self: InputProbe, phase: string, key: string, first: number?) -> number,
}

local InputProbe = {}

local function Read(): Reading
	return {
		Tick = Time.GetTick(),
		Frame = Time.GetFrameCount(),
		SpaceDown = Input.IsKeyDown("Space"),
		SpacePressed = Input.IsKeyPressed("Space"),
		SpaceReleased = Input.IsKeyReleased("Space"),
		DDown = Input.IsKeyDown("D"),
		LeftDown = Input.IsMouseButtonDown("Left"),
		LeftPressed = Input.IsMouseButtonPressed("Left"),
		LeftReleased = Input.IsMouseButtonReleased("Left"),
		RightDown = Input.IsMouseButtonDown("Right"),
		MousePosition = Input.GetMousePosition(),
		MouseDelta = Input.GetMouseDelta(),
		Scroll = Input.GetScrollDelta(),
		PadConnected = Input.IsGamepadConnected(0),
		SouthDown = Input.IsGamepadButtonDown(0, "South"),
		LeftX = Input.GetGamepadAxis(0, "LeftX"),
		LeftY = Input.GetGamepadAxis(0, "LeftY"),
		RightTrigger = Input.GetGamepadAxis(0, "RightTrigger"),
		JumpDown = Input.IsActionDown("Jump"),
		JumpPressed = Input.IsActionPressed("Jump"),
		JumpReleased = Input.IsActionReleased("Jump"),
		FireDown = Input.IsActionDown("Fire"),
		FirePressed = Input.IsActionPressed("Fire"),
		FireReleased = Input.IsActionReleased("Fire"),
		MoveX = Input.GetAxis("MoveX"),
		MoveY = Input.GetAxis("MoveY"),
	}
end

function InputProbe.OnCreate(self: InputProbe)
	self.Steps = {}
	self.Frames = {}
end

function InputProbe.OnFixedUpdate(self: InputProbe, dt: number)
	table.insert(self.Steps, Read())
end

function InputProbe.OnUpdate(self: InputProbe, dt: number)
	table.insert(self.Frames, Read())
end

-- The reading of the step that ran tick `tick`, or nil.
function InputProbe.StepAt(self: InputProbe, tick: number): Reading?
	return self.Steps[tick + 1]
end

-- How many readings of `phase` ("Steps" or "Frames") have `key` set, from index `first` on.
function InputProbe.Count(self: InputProbe, phase: string, key: string, first: number?): number
	local readings: { Reading } = if phase == "Steps" then self.Steps else self.Frames
	local count = 0
	for index = first or 1, #readings do
		if (readings[index] :: any)[key] then
			count += 1
		end
	end
	return count
end

return Script.Define("InputProbe", InputProbe)
