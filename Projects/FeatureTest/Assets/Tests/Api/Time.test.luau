--!strict
-- The Time module (§4.2, §11.5) in a lockstep test session at 64 Hz: ticks, simulation time, the phase-aware delta and
-- alpha, frames, the time scale and the non-deterministic real time. The Timing suite covers scripted clocks.
local FIXED = 1 / 64

return Test.Suite("Api.Time", function()
	Test.Case("simulation time counts ticks of the project's fixed rate", function()
		Test.ExpectEqual(Time.GetFixedDeltaTime(), FIXED)
		local tick = Time.GetTick()
		Test.ExpectEqual(Time.GetTime(), tick * FIXED)
		Test.ExpectEqual(Time.GetDeltaTime(), FIXED, "a case runs in the fixed phase")
		Test.ExpectEqual(Time.GetInterpolationAlpha(), 1, "alpha is 1 in the fixed phase")
		local frame = Time.GetFrameCount()
		Test.WaitTicks(4)
		Test.ExpectEqual(Time.GetTick(), tick + 4)
		Test.ExpectEqual(Time.GetTime(), (tick + 4) * FIXED)
		Test.ExpectEqual(Time.GetFrameCount(), frame + 4, "lockstep renders one frame per tick")
	end)

	Test.Case("the time scale is kept by the session and never stretches a fixed step", function()
		Test.ExpectEqual(Time.GetTimeScale(), 1)
		Time.SetTimeScale(0.5)
		Test.ExpectEqual(Time.GetTimeScale(), 0.5)
		Test.WaitTicks(1)
		Test.ExpectEqual(Time.GetDeltaTime(), FIXED, "a fixed step never stretches; the scale sets how many steps a frame runs")
		Test.ExpectEqual(Time.GetFixedDeltaTime(), FIXED, "the fixed delta is unscaled")
		Time.SetTimeScale(1)
		Test.Expect(not pcall(Time.SetTimeScale, -1), "a negative scale is refused")
		Test.Expect(not pcall(Time.SetTimeScale, 101), "a scale above 100 is refused")
	end)

	Test.Case("real time is a finite number that never runs backwards", function()
		local first = Time.GetRealTime()
		local second = Time.GetRealTime()
		Test.Expect(first == first and math.abs(first) < math.huge, "finite")
		Test.Expect(second >= first, "monotonic")
	end)
end)
