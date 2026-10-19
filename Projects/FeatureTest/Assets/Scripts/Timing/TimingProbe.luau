--!strict
-- Records what each fixed step and each frame of a Timing.scene session observes: ticks, phase-aware deltas, the
-- interpolation alpha, Space/Jump input edges, and the previous frame's render extraction (its frame, alpha and the
-- extracted positions of the moving entities). It also teleports Jumper in the frame phase of TeleportFrames.
-- It records only when the scene is loaded with the parameter RecordTiming = true, which the Timing suite's entry in
-- Testing.Suites passes: the extraction reads (Test.GetLastExtraction, Test.GetExtractedPosition) exist only in test
-- runs and the records grow every tick, so a play outside that suite (editor Play, the hardening soak) records nothing
-- and raises no script error. OnCreate reads the parameter before the first frame the suite checks.
export type StepRecord = {
	Tick: number,
	Frame: number,
	Dt: number,
	Delta: number,
	Fixed: number,
	Alpha: number,
	Time: number,
	Pressed: boolean,
	Released: boolean,
	Down: boolean,
	ActionPressed: boolean,
	ActionReleased: boolean,
}

export type FrameRecord = {
	Frame: number,
	Tick: number,
	Dt: number,
	Delta: number,
	Alpha: number,
	Pressed: boolean,
	Released: boolean,
	Down: boolean,
	ActionPressed: boolean,
	ActionReleased: boolean,
	LastFrame: number,
	LastAlpha: number,
	Slider: vector?,
	Drifter: vector?,
	Jumper: vector?,
}

export type TimingProbe = ScriptInstance & {
	Slider: Entity?,
	Drifter: Entity?,
	Jumper: Entity?,
	TeleportFrames: { number },
	TeleportTarget: vector,
	Record: boolean,
	Steps: { StepRecord },
	Frames: { FrameRecord },
	Teleports: { [number]: number },
}

local TimingProbe = {}

TimingProbe.Fields = {
	Slider = Field.Entity(),
	Drifter = Field.Entity(),
	Jumper = Field.Entity(),
	TeleportFrames = Field.Array(Field.Integer()),
	TeleportTarget = Field.Vector(vector.zero),
}

function TimingProbe.OnCreate(self: TimingProbe)
	self.Record = Scene.GetLoadParameters().RecordTiming == true
	self.Steps = {}
	self.Frames = {}
	self.Teleports = {}
end

function TimingProbe.OnFixedUpdate(self: TimingProbe, dt: number)
	if not self.Record then
		return
	end
	table.insert(self.Steps, {
		Tick = Time.GetTick(),
		Frame = Time.GetFrameCount(),
		Dt = dt,
		Delta = Time.GetDeltaTime(),
		Fixed = Time.GetFixedDeltaTime(),
		Alpha = Time.GetInterpolationAlpha(),
		Time = Time.GetTime(),
		Pressed = Input.IsKeyPressed("Space"),
		Released = Input.IsKeyReleased("Space"),
		Down = Input.IsKeyDown("Space"),
		ActionPressed = Input.IsActionPressed("Jump"),
		ActionReleased = Input.IsActionReleased("Jump"),
	})
end

local function Extracted(entity: Entity?): vector?
	if entity == nil then
		return nil
	end
	return Test.GetExtractedPosition(entity)
end

function TimingProbe.OnUpdate(self: TimingProbe, dt: number)
	if not self.Record then
		return
	end
	local last = Test.GetLastExtraction()
	table.insert(self.Frames, {
		Frame = Time.GetFrameCount(),
		Tick = Time.GetTick(),
		Dt = dt,
		Delta = Time.GetDeltaTime(),
		Alpha = Time.GetInterpolationAlpha(),
		Pressed = Input.IsKeyPressed("Space"),
		Released = Input.IsKeyReleased("Space"),
		Down = Input.IsKeyDown("Space"),
		ActionPressed = Input.IsActionPressed("Jump"),
		ActionReleased = Input.IsActionReleased("Jump"),
		LastFrame = last.Frame,
		LastAlpha = last.Alpha,
		Slider = Extracted(self.Slider),
		Drifter = Extracted(self.Drifter),
		Jumper = Extracted(self.Jumper),
	})
end

function TimingProbe.OnLateUpdate(self: TimingProbe, dt: number)
	local frame = Time.GetFrameCount()
	local jumper = self.Jumper
	if jumper ~= nil and table.find(self.TeleportFrames, frame) ~= nil then
		jumper.Transform:Teleport(self.TeleportTarget)
		self.Teleports[frame] = Time.GetTick()
	end
end

return Script.Define("TimingProbe", TimingProbe)
