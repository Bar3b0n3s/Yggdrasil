--!strict
-- The scripted clock [0.004, 0.016, 0.0334, 0.083, 0.5, 0.0] at the project's 64 Hz (§15.5, ADR 0022 decision 9):
-- frames of 0, 1, 2 and 5 steps, the 0.25 s clamp and dropped time. Every observation of TimingProbe is compared with
-- an analytic model of FixedStepScheduler (§4.2) fed with the float-stored frame deltas: steps per frame, phase-aware
-- Time.GetDeltaTime, Time.GetInterpolationAlpha, the extraction Alpha of Test.GetLastExtraction, exactly-once Pressed
-- and Released edges in both phases, and the interpolated positions of a script-moved, a physics-moved and a
-- teleported entity read with Test.GetExtractedPosition.
local ProbeScript = require("../../Scripts/Timing/TimingProbe")

local CLOCK = { 0.004, 0.016, 0.0334, 0.083, 0.5, 0.0 }
local FRAMES = 20
local MAX_FRAME_DELTA = 0.25
local MAX_STEPS = 5
local STEP_TOLERANCE = 1e-9
local POSITION_TOLERANCE = 0.0001

-- Rounds a number to float, as the settings store the clock.
local function F32(value: number): number
	local scratch = buffer.create(4)
	buffer.writef32(scratch, 0, value)
	return buffer.readf32(scratch, 0)
end

type ModelFrame = {
	Delta: number,
	Steps: number,
	FirstTick: number,
	Completed: number,
	Alpha: number,
	Dropped: number,
}

-- FixedStepScheduler::Advance for every frame of the clock, at time scale 1.
local function Schedule(fixed: number, count: number): { ModelFrame }
	local frames: { ModelFrame } = {}
	local accumulator, tick = 0, 0
	local belowFixed = fixed - math.ldexp(1, -59) -- the double just below 1/64
	for index = 0, count - 1 do
		local delta = F32(CLOCK[index % #CLOCK + 1])
		local clamped = math.min(delta, MAX_FRAME_DELTA)
		local dropped = delta - clamped
		accumulator += clamped
		local steps = 0
		while accumulator >= fixed - STEP_TOLERANCE and steps < MAX_STEPS do
			steps += 1
			accumulator = math.max(0, accumulator - fixed)
		end
		if steps == MAX_STEPS and accumulator >= fixed - STEP_TOLERANCE then
			local whole = math.floor((accumulator + STEP_TOLERANCE) / fixed)
			dropped += fixed * whole
			accumulator = math.clamp(accumulator - fixed * whole, 0, belowFixed)
		end
		table.insert(frames, {
			Delta = delta,
			Steps = steps,
			FirstTick = tick,
			Completed = tick + steps,
			Alpha = math.clamp(accumulator / fixed, 0, 1),
			Dropped = dropped,
		})
		tick += steps
	end
	return frames
end

-- The frame (0-based) whose steps include `tick`.
local function FrameOfTick(model: { ModelFrame }, tick: number): number
	for index, frame in model do
		if tick >= frame.FirstTick and tick < frame.Completed then
			return index - 1
		end
	end
	return -1
end

-- The position a fixed-phase mover is extracted at in frame `index`: its last step's previous and current poses
-- interpolated by the frame's alpha (§5.2), or its start before any step.
local function Interpolated(model: { ModelFrame }, index: number, start: vector, velocity: vector, fixed: number): vector
	local completed = model[index + 1].Completed
	if completed == 0 then
		return start
	end
	local previous = start + velocity * (fixed * (completed - 1))
	local current = start + velocity * (fixed * completed)
	return previous + (current - previous) * F32(model[index + 1].Alpha)
end

local function Probe(): ProbeScript.TimingProbe
	local entity = Scene.FindByName("Probe")
	assert(entity ~= nil, "Timing.scene has a Probe")
	return entity:GetScript() :: ProbeScript.TimingProbe
end

return Test.Suite("Timing", function()
	Test.Case("frames run 0, 1, 2 and 5 steps with clamped and dropped time and phase-aware observations", function()
		local fixed = Time.GetFixedDeltaTime()
		Test.ExpectEqual(fixed, 1 / 64, "FeatureTest runs at 64 Hz")
		local model = Schedule(fixed, FRAMES)
		local first = {}
		for index = 1, #CLOCK do
			table.insert(first, model[index].Steps)
		end
		Test.ExpectEqual(table.concat(first, ","), "0,1,2,5,5,0", "the clock's first cycle")
		Test.ExpectNear(model[5].Dropped, 0.25 + 11 * fixed, 1e-12, "the 0.5 s frame drops its clamped part and 11 steps")
		Test.Expect(model[6].Alpha > 0 and model[6].Alpha < 1, "the empty frame keeps a partial step")

		-- Space taps and holds, injected at chosen ticks; an injection applies at the next tick.
		local probe = Probe()
		local downs, ups = {}, {}
		local function Inject(state: InputEventState)
			local tick = Time.GetTick() + 1
			Test.InjectKey("Space", state)
			if state ~= "Up" then
				table.insert(downs, tick)
			end
			if state ~= "Down" then
				table.insert(ups, state == "Tap" and tick + 1 or tick)
			end
		end
		local function WaitForTick(tick: number)
			Test.WaitUntil(function(): boolean
				return Time.GetTick() >= tick
			end, 100)
			Test.ExpectEqual(Time.GetTick(), tick)
		end
		Inject("Tap") -- pressed and released in the two steps of frame 2
		WaitForTick(4)
		Inject("Tap") -- pressed and released inside the five steps of frame 3
		WaitForTick(7)
		Inject("Down") -- pressed in the first step of frame 4, the clamped 0.5 s frame
		WaitForTick(12)
		Inject("Up") -- released in frame 7, after the empty frames 5 and 6
		Test.WaitUntil(function(): boolean
			return #probe.Frames >= FRAMES
		end, 200)

		-- Frames: delta, steps, alpha and the previous frame's extraction.
		local frames = probe.Frames
		for index = 0, FRAMES - 1 do
			local record = frames[index + 1]
			local expected = model[index + 1]
			Test.ExpectEqual(record.Frame, index, "frame index")
			Test.ExpectEqual(record.Tick, expected.Completed, "ticks completed by frame " .. index)
			Test.ExpectEqual(record.Dt, expected.Delta, "OnUpdate dt of frame " .. index)
			Test.ExpectEqual(record.Delta, expected.Delta, "frame-phase Time.GetDeltaTime of frame " .. index)
			Test.ExpectNear(record.Alpha, expected.Alpha, 1e-6, "frame-phase alpha of frame " .. index)
			-- A frame's own extraction follows its update phases, so it reads its predecessor's: Frame counts the
			-- extractions completed so far.
			Test.ExpectEqual(record.LastFrame, index, "extractions completed before frame " .. index)
			if index > 0 then
				Test.ExpectNear(record.LastAlpha, model[index].Alpha, 1e-6, "extraction alpha of frame " .. index - 1)
			end
		end

		-- Steps: one record per tick, all with the fixed delta and alpha 1, each inside the frame the model expects.
		local steps = probe.Steps
		local total = model[FRAMES].Completed
		Test.Expect(#steps >= total, "every modelled step ran")
		for index = 1, total do
			local record = steps[index]
			Test.ExpectEqual(record.Tick, index - 1, "tick of step " .. index)
			Test.ExpectEqual(record.Frame, FrameOfTick(model, index - 1), "frame of tick " .. index - 1)
			Test.ExpectEqual(record.Dt, fixed)
			Test.ExpectEqual(record.Delta, fixed, "fixed-phase Time.GetDeltaTime")
			Test.ExpectEqual(record.Fixed, fixed)
			Test.ExpectEqual(record.Alpha, 1, "Time.GetInterpolationAlpha is 1 in the fixed phase")
			Test.ExpectEqual(record.Time, (index - 1) * fixed, "Time.GetTime is tick times the fixed delta")
		end

		-- Input edges: each press and release is seen exactly once in the fixed phase (at its tick) and exactly once in
		-- the frame phase (in the frame whose steps applied it).
		local function Ticks(kind: string): { number }
			local result: { number } = {}
			for index = 1, total do
				local record = steps[index] :: any
				if record[kind] then
					table.insert(result, record.Tick)
				end
			end
			return result
		end
		local function Frames(kind: string): { number }
			local result: { number } = {}
			for _, record in frames do
				if (record :: any)[kind] then
					table.insert(result, record.Frame)
				end
			end
			return result
		end
		local function FramesOf(ticks: { number }): { number }
			local result: { number } = {}
			for _, tick in ticks do
				local frame = FrameOfTick(model, tick)
				if table.find(result, frame) == nil then
					table.insert(result, frame)
				end
			end
			return result
		end
		Test.ExpectEqual(table.concat(Ticks("Pressed"), ","), table.concat(downs, ","), "fixed-phase key presses")
		Test.ExpectEqual(table.concat(Ticks("Released"), ","), table.concat(ups, ","), "fixed-phase key releases")
		Test.ExpectEqual(table.concat(Ticks("ActionPressed"), ","), table.concat(downs, ","), "fixed-phase Jump presses")
		Test.ExpectEqual(table.concat(Ticks("ActionReleased"), ","), table.concat(ups, ","), "fixed-phase Jump releases")
		Test.ExpectEqual(table.concat(Frames("Pressed"), ","), table.concat(FramesOf(downs), ","), "frame-phase key presses")
		Test.ExpectEqual(table.concat(Frames("Released"), ","), table.concat(FramesOf(ups), ","), "frame-phase key releases")
		Test.ExpectEqual(table.concat(Frames("ActionPressed"), ","), table.concat(FramesOf(downs), ","), "frame-phase Jump presses")
		Test.ExpectEqual(table.concat(Frames("ActionReleased"), ","), table.concat(FramesOf(ups), ","), "frame-phase Jump releases")
		Test.ExpectEqual(table.concat(FramesOf(downs), ","), "2,3,4", "the frames whose steps applied the presses")
		Test.ExpectEqual(table.concat(FramesOf(ups), ","), "2,3,7", "the frames whose steps applied the releases")
		for _, index in { 5, 6 } do
			local record = frames[index + 1]
			Test.Expect(record.Down and not record.Pressed and not record.Released, "Space stays held without edges in empty frame " .. index)
		end

		-- Extracted positions of frame i are read in frame i + 1.
		local slider = Scene.FindByName("Slider") :: Entity
		local drifter = Scene.FindByName("Drifter") :: Entity
		local jumper = Scene.FindByName("Jumper") :: Entity
		local sliderStart = vector.create(-4, 1, 0)
		local sliderVelocity = vector.create(2, 0, 0)
		local drifterStart = vector.create(-2, 3, -2)
		local drifterVelocity = vector.create(1.5, 0, 0)
		local jumperStart = vector.create(2, 1, 0)
		local jumperVelocity = vector.create(0, 0, 3)
		Test.ExpectEqual(probe.Slider, slider)
		Test.ExpectEqual(probe.Drifter, drifter)
		Test.ExpectEqual(probe.Jumper, jumper)
		local teleported = 0
		local jumperOrigin, originCompleted = jumperStart, 0
		for index = 0, FRAMES - 2 do
			local record = frames[index + 2]
			local expected = model[index + 1]
			Test.ExpectNear(record.Slider :: vector, Interpolated(model, index, sliderStart, sliderVelocity, fixed), POSITION_TOLERANCE,
				"script-moved Slider in frame " .. index)
			Test.ExpectNear(record.Drifter :: vector, Interpolated(model, index, drifterStart, drifterVelocity, fixed), POSITION_TOLERANCE,
				"physics-moved Drifter in frame " .. index)
			-- Jumper moves like Slider from its last teleport; in a frame that teleported it, and until its next step,
			-- it renders at the teleport target without interpolation.
			local completed = expected.Completed
			local jumperExpected: vector
			if probe.Teleports[index] ~= nil then
				teleported += 1
				Test.ExpectEqual(probe.Teleports[index], completed, "Jumper teleports after frame " .. index .. "'s steps")
				jumperOrigin, originCompleted = probe.TeleportTarget, completed
				jumperExpected = probe.TeleportTarget
			elseif completed == originCompleted then
				jumperExpected = jumperOrigin
			else
				local previous = jumperOrigin + jumperVelocity * (fixed * (completed - 1 - originCompleted))
				local current = jumperOrigin + jumperVelocity * (fixed * (completed - originCompleted))
				jumperExpected = previous + (current - previous) * F32(expected.Alpha)
			end
			Test.ExpectNear(record.Jumper :: vector, jumperExpected, POSITION_TOLERANCE, "teleported Jumper in frame " .. index)
		end
		Test.ExpectEqual(teleported, #probe.TeleportFrames, "every teleport frame was observed")
	end)
end)
