#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Scripting/Sandbox.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include <vector>

namespace Engine {

	static void CheckColorScript(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		if (!result)
			INFO(result.error().ToString());
		REQUIRE(result.has_value());
		CHECK(result->Value.Get() == Json(true));
	}

	namespace Test {

		void RunColorBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.Start().has_value());
			CheckColorScript(fixture, R"(
local c = Color.New(1, 2, 3)
assert(c.r == 1 and c.g == 2 and c.b == 3 and c.a == 1)
c.r = 4; c.g = 5; c.b = 6; c.a = 0.5
assert(c.r == 4 and c.g == 5 and c.b == 6 and c.a == 0.5)
local half = Color.Lerp(c, Color.New(0, 1, 2, 1), 0.5)
assert(half.r == 2 and half.g == 3 and half.b == 4 and half.a == 0.75)
local hex = Color.FromHex("#FF000080")
assert(hex.r == 1 and hex.g == 0 and hex.b == 0)
assert(math.abs(hex.a - 128/255) < 1e-7)
return true
)");
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			size_t members = 0;
			for (const auto& counter : coverage->Members)
			{
				if (counter.Owner != "Color")
					continue;
				CAPTURE(counter.Member);
				if (counter.Kind == ScriptApiMemberKind::Property)
				{
					CHECK(counter.Reads > 0);
					CHECK(counter.Writes > 0);
				}
				else
					CHECK(counter.Calls > 0);
				++members;
			}
			CHECK(members == 7);
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ColorBindings: constructors interpolation and fields have counted calls")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunColorBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("ColorBindings: hex parsing accepts exact six or eight digit linear channels")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckColorScript(fixture, R"(
local white = Color.FromHex("#ffffff")
assert(white.r == 1 and white.g == 1 and white.b == 1 and white.a == 1)
local black = Color.FromHex("#00000000")
assert(black.r == 0 and black.g == 0 and black.b == 0 and black.a == 0)
local mixed = Color.FromHex("#aB20Ef7F")
assert(math.abs(mixed.r - 171/255) < 1e-7)
assert(math.abs(mixed.g - 32/255) < 1e-7)
assert(math.abs(mixed.b - 239/255) < 1e-7)
assert(math.abs(mixed.a - 127/255) < 1e-7)
for _, text in {"", "#", "#fff", "FFFFFF", "#0000000", "#000000000", "#GG0000", "#000000\0", " #000000", "#000000 "} do
	assert(not pcall(function() Color.FromHex(text) end), "accepted invalid hex: " .. text)
end
return true
)");
		}

		TEST_CASE("ColorBindings: interpolation clamps time without clamping HDR channels")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckColorScript(fixture, R"(
local a = Color.New(-4, 8, 12, -1)
local b = Color.New(4, 0, -12, 3)
local low, high = Color.Lerp(a, b, -2), Color.Lerp(a, b, 2)
assert(low.r == -4 and low.g == 8 and low.b == 12 and low.a == -1)
assert(high.r == 4 and high.g == 0 and high.b == -12 and high.a == 3)
local middle = Color.Lerp(a, b, 0.5)
assert(middle.r == 0 and middle.g == 4 and middle.b == 0 and middle.a == 1)
middle.r = 100
assert(a.r == -4 and b.r == 4)
local extreme = Color.Lerp(Color.New(-3e38, 3e38, 0), Color.New(3e38, -3e38, 0), 0.5)
assert(extreme.r == 0 and extreme.g == 0)
return true
)");
		}

		TEST_CASE("ColorBindings: local edits work in read-only eval and failed setters preserve values")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			REQUIRE(fixture.Start().has_value());
			CheckColorScript(fixture, R"(
local c = Color.New(1, 2, 3, 4)
c.r = 5; c.g = 6; c.b = 7; c.a = 8
assert(c.r == 5 and c.g == 6 and c.b == 7 and c.a == 8)
for _, field in {"r", "g", "b", "a"} do
	local before = c[field]
	assert(not pcall(function() c[field] = 0/0 end))
	assert(not pcall(function() c[field] = math.huge end))
	assert(not pcall(function() c[field] = 1e308 end))
	assert(not pcall(function() c[field] = "1" end))
	assert(c[field] == before)
end
assert(not pcall(function() c.unknown = 10 end))
assert(not pcall(function() Color.New = false end))
return true
)");
			CHECK(fixture.ExternalMutations.empty());
			for (const auto& group : fixture.GetApi().GetTypes())
			{
				if (group.Name != "Color")
					continue;
				for (const auto& member : group.Members)
				{
					CAPTURE(member.Name);
					CHECK(member.Options.Environments == ScriptApiEnvironment::All);
					CHECK(member.Options.Modes == RunModes::All);
					CHECK_FALSE(member.Options.Mutates);
					CHECK_FALSE(member.Options.SetterMutates);
					CHECK_FALSE(member.Description.empty());
				}
			}
		}

		TEST_CASE("ColorBindings: constructors reject nonfinite values and wrong userdata kinds")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckColorScript(fixture, R"(
local invalid = {
	function() Color.New(0/0, 0, 0) end,
	function() Color.New(0, math.huge, 0) end,
	function() Color.New(0, 0, -math.huge) end,
	function() Color.New(0, 0, 0, math.huge) end,
	function() Color.New(1e308, 0, 0) end,
	function() Color.New(true, 0, 0) end,
	function() Color.FromHex(123456) end,
	function() Color.Lerp(Quat.Identity(), Color.New(1, 1, 1), 0.5) end,
	function() Color.Lerp(Color.New(1, 1, 1), vector.create(1, 1, 1), 0.5) end,
	function() Color.Lerp(Color.New(1, 1, 1), Color.New(0, 0, 0), math.huge) end,
}
for index, fn in invalid do
	assert(not pcall(fn), "invalid case accepted: " .. index)
end
return Color.New(1, 2, 3, nil).a == 1
)");
		}

		TEST_CASE("ColorBindings: local colors are available to load-time modules")
		{
			auto sandbox = Sandbox::Create({ .Mode = SandboxMode::LoadTime, .ReadOnly = true });
			REQUIRE(sandbox.has_value());
			const auto result = (*sandbox)->Evaluate(
				"local c = Color.FromHex(\"#FF0000\"); c.g = 2; return c.r == 1 and c.g == 2 and c.a == 1", {});
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			auto path = VfsPath::Parse("project://Assets/ColorErrors.luau");
			REQUIRE(path.has_value());
			CHECK_FALSE((*sandbox)->Evaluate("local c = Color.New(1, 0, 0)\nc.r = math.huge", *path).has_value());
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Script == "Assets/ColorErrors.luau");
			CHECK(error->Line == 2);
		}
	}

}
