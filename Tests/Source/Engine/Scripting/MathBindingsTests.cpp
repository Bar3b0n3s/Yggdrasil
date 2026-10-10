#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Core/Random.h"
#include "Engine/Scripting/Sandbox.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include <vector>

namespace Engine {

	static void CheckMathScript(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		if (!result)
			INFO(result.error().ToString());
		REQUIRE(result.has_value());
		CHECK(result->Value.Get() == Json(true));
	}

	namespace Test {

		void RunMathBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.Start().has_value());
			CheckMathScript(fixture, R"(
local function near(a, b, epsilon)
	assert(math.abs(a - b) <= (epsilon or 1e-12), tostring(a) .. " ~= " .. tostring(b))
end
assert(Math.Clamp(-10, -2, 5) == -2)
assert(Math.Clamp(20, -2, 5) == 5)
assert(Math.Clamp(1, -2, 5) == 1)
assert(Math.Clamp01(-1) == 0 and Math.Clamp01(2) == 1)
assert(Math.Lerp(2, 6, 0.25) == 3)
assert(Math.Lerp(2, 6, -1) == 2 and Math.Lerp(2, 6, 2) == 6)
assert(Math.InverseLerp(2, 6, 3) == 0.25)
assert(Math.InverseLerp(6, 2, 3) == 0.75)
assert(Math.InverseLerp(1, 1, 200) == 0)
assert(Math.InverseLerp(2, 6, -10) == 0 and Math.InverseLerp(2, 6, 10) == 1)
assert(Math.Remap(3, 2, 6, 10, 30) == 15)
assert(Math.Remap(3, 6, 2, 30, 10) == 15)
assert(Math.Remap(-10, 2, 6, 10, 30) == 10)
assert(Math.SmoothStep(2, 6, 0.25) == 2.625)
assert(Math.SmoothStep(2, 6, -1) == 2 and Math.SmoothStep(2, 6, 2) == 6)
local value, velocity = Math.SmoothDamp(0, 1, 0, 2, 1)
near(value, 0.26424111765711533)
near(velocity, 0.36787944117144233)
assert(Math.MoveTowards(1, 5, 2) == 3)
assert(Math.MoveTowards(5, 1, 2) == 3)
assert(Math.MoveTowards(1, 2, 20) == 2)
assert(Math.Approximately(1000000, 1000000.5))
assert(not Math.Approximately(0, 0.01))
assert(Math.Approximately(1, 1, 0) and not Math.Approximately(1, 2, 0))
assert(Math.DeltaAngle(350, 10) == 20)
assert(Math.DeltaAngle(10, 350) == -20)
assert(Math.DeltaAngle(0, -180) == 180)
assert(Math.LerpAngle(350, 10, 0.5) == 360)
assert(Math.LerpAngle(10, 350, 0.5) == 0)
assert(Math.Sign(-10) == -1 and Math.Sign(10) == 1)
assert(Math.Sign(0) == 0 and Math.Sign(-0.0) == 0)
assert(Math.Repeat(7, 3) == 1 and Math.Repeat(-7, 3) == 2)
assert(Math.Repeat(-6, 3) == 0)
assert(Math.PingPong(7, 3) == 1 and Math.PingPong(-7, 3) == 1)
assert(Math.PingPong(4, 3) == 2 and Math.PingPong(3, 3) == 3)
near(Math.Pi, 3.141592653589793, 2e-7)
near(Math.Deg2Rad * 180, 3.141592653589793, 2e-7)
near(Math.Rad2Deg * Math.Deg2Rad, 1, 2e-7)
return true
)");
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			size_t functions = 0;
			for (const auto& counter : coverage->Members)
			{
				if (counter.Owner != "Math" || counter.Kind == ScriptApiMemberKind::Constant)
					continue;
				CAPTURE(counter.Member);
				CHECK(counter.Calls > 0);
				++functions;
			}
			CHECK(functions == 14);
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("MathBindings: scalar functions cover every registered member")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunMathBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("MathBindings: vector interpolation and movement preserve geometric meaning")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckMathScript(fixture, R"(
local a = vector.create(-1, 2, 4)
local b = vector.create(1, 4, 8)
assert(Math.Clamp(a, vector.zero, vector.create(2, 3, 3)) == vector.create(0, 2, 3))
assert(Math.Clamp01(a) == vector.create(0, 1, 1))
assert(Math.Lerp(a, b, 0.5) == vector.create(0, 3, 6))
assert(Math.Lerp(a, b, -1) == a and Math.Lerp(a, b, 2) == b)
local target = vector.create(3, 4, 0)
local moved = Math.MoveTowards(vector.zero, target, 2.5)
assert(moved == vector.create(1.5, 2, 0))
assert(Math.MoveTowards(vector.zero, target, 10) == target)
assert(Math.MoveTowards(target, target, 0) == target)
local value, velocity = Math.SmoothDamp(vector.zero, vector.create(1, 2, -3), vector.zero, 2, 1)
assert(vector.magnitude(value - vector.create(0.26424111765711533, 0.5284822353142307, -0.792723352971346)) < 1e-6)
assert(vector.magnitude(velocity - vector.create(0.36787944117144233, 0.7357588823428847, -1.103638323514327)) < 1e-6)
return true
)");
		}

		TEST_CASE("MathBindings: damping is stable at zero time and reaches rest without overshoot")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckMathScript(fixture, R"(
local value, velocity = Math.SmoothDamp(3, 8, -2, 0.5, 0)
assert(value == 3 and velocity == -2)
value, velocity = Math.SmoothDamp(0, 1, 100, 1, 0.1)
assert(value == 1 and velocity == 0)
value, velocity = Math.SmoothDamp(2, -4, 1, 0.000001, 1e308)
assert(value == -4 and velocity == 0)
local oneValue, oneVelocity = Math.SmoothDamp(-2, 7, 0, 2, 1)
local halfValue, halfVelocity = Math.SmoothDamp(-2, 7, 0, 2, 0.5)
local twoValue, twoVelocity = Math.SmoothDamp(halfValue, 7, halfVelocity, 2, 0.5)
assert(math.abs(oneValue - twoValue) < 1e-12 and math.abs(oneVelocity - twoVelocity) < 1e-12)
local position, speed = 0, 0
for _ = 1, 300 do
	local nextPosition, nextSpeed = Math.SmoothDamp(position, 10, speed, 0.2, 1/60)
	assert(nextPosition >= position and nextPosition <= 10)
	position, speed = nextPosition, nextSpeed
end
assert(math.abs(position - 10) < 1e-9)
return true
)");
		}

		TEST_CASE("MathBindings: finite extremes do not overflow interpolation or angle wrapping")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckMathScript(fixture, R"(
assert(Math.Lerp(-1.7e308, 1.7e308, 0.5) == 0)
assert(Math.InverseLerp(-1.7e308, 1.7e308, 0) == 0.5)
assert(Math.Remap(0, -1.7e308, 1.7e308, -1.7e308, 1.7e308) == 0)
assert(not Math.Approximately(-1.7e308, 1.7e308, 1))
local delta = Math.DeltaAngle(-1.7e308, 1.7e308)
assert(delta > -180 and delta <= 180)
local repeated = Math.Repeat(1e308, 1e-300)
assert(repeated >= 0 and repeated < 1e-300)
local ping = Math.PingPong(1.7e308, 1.6e308)
assert(ping >= 0 and ping <= 1.6e308)
local tiny = 4.9406564584124654e-324
assert(Math.PingPong(tiny, tiny) == tiny)
local midpoint = Math.Lerp(vector.create(-3e38, 0, 0), vector.create(3e38, 0, 0), 0.5)
assert(midpoint == vector.zero)
return true
)");
		}

		TEST_CASE("MathBindings: invalid types ranges and nonfinite arguments raise script errors")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckMathScript(fixture, R"(
local invalid = {
	function() Math.Clamp(1, 2, 0) end,
	function() Math.Clamp(vector.zero, vector.create(0, 2, 0), vector.create(1, 1, 1)) end,
	function() Math.Clamp01("1") end,
	function() Math.Lerp(vector.zero, 2, 0.5) end,
	function() Math.InverseLerp(0, 1, math.huge) end,
	function() Math.Remap(1, 2, 2, 0, 1) end,
	function() Math.SmoothStep(0, 1, 0/0) end,
	function() Math.SmoothDamp(0, 1, 0, 0, 0.1) end,
	function() Math.SmoothDamp(0, 1, 0, 1, -0.1) end,
	function() Math.SmoothDamp(-1.7e308, 1.7e308, 0, 1, 0.1) end,
	function() Math.MoveTowards(0, 1, -1) end,
	function() Math.Approximately(0, 1, -1) end,
	function() Math.DeltaAngle(0, math.huge) end,
	function() Math.LerpAngle(0, 1, 0/0) end,
	function() Math.Sign(0/0) end,
	function() Math.Repeat(1, 0) end,
	function() Math.PingPong(1, -1) end,
}
for index, fn in invalid do
	assert(not pcall(fn), "invalid case accepted: " .. index)
end
return Math.Clamp01(0.5) == 0.5
)");
		}

		TEST_CASE("MathBindings: pure utilities run at load time and during read-only evaluation")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			REQUIRE(fixture.Start().has_value());
			CheckMathScript(fixture, "return Math.Lerp(2, 4, 0.5) == 3");
			CHECK(fixture.ExternalMutations.empty());
			auto sandbox = Sandbox::Create({ .Mode = SandboxMode::LoadTime, .ReadOnly = true });
			REQUIRE(sandbox.has_value());
			const auto result = (*sandbox)->Evaluate("return Math.Clamp01(2) == 1", {});
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			for (const auto& group : fixture.GetApi().GetModules())
			{
				if (group.Name != "Math")
					continue;
				for (const auto& member : group.Members)
				{
					CAPTURE(member.Name);
					CHECK(member.Options.Environments == ScriptApiEnvironment::All);
					CHECK(member.Options.Modes == RunModes::All);
					CHECK_FALSE(member.Options.Mutates);
					CHECK_FALSE(member.Description.empty());
				}
			}
		}

		TEST_CASE("MathBindings: nonfinite rejection retains the authored source location")
		{
			Random random(23);
			auto sandbox = Sandbox::Create({ .RandomStream = &random });
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/MathErrors.luau");
			REQUIRE(path.has_value());
			const auto result = (*sandbox)->Evaluate("local value = 0/0\nreturn Math.Clamp01(value)", *path);
			CHECK_FALSE(result.has_value());
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Script == "Assets/MathErrors.luau");
			CHECK(error->Line == 2);
			CHECK(error->Message.find("Math.Clamp01") != std::string::npos);
			CHECK(error->Message.find("NaN") != std::string::npos);
		}
	}

}
