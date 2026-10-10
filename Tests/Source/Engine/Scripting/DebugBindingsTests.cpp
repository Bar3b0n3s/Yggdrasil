#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Renderer/DebugDrawList.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <variant>

#include <vector>

namespace Engine {

	namespace Test {

		void RunDebugBreakCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);

		void RunDebugBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			RunDebugBreakCoverage(mode, snapshots);
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local a, b = vector.create(1, 2, 3), vector.create(4, 6, 3)
local color = Color.New(0.25, 0.5, 0.75, 0.5)
Debug.DrawLine(a, b, color, 2)
Debug.DrawRay(a, vector.create(3, 4, 0))
Debug.DrawBox(a, vector.create(2, 3, 4), Quat.New(0, 0, 0, 2), color, 1)
Debug.DrawSphere(a, 2)
Debug.DrawArrow(a, b)
Debug.DrawText(a, "Text ✓", color, 3)
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			const auto* list = fixture.GetDebugDraw();
			REQUIRE(list != nullptr);
			const auto commands = list->GetCommands();
			REQUIRE(commands.size() == 6);
			CHECK(std::get<DebugLine>(commands[0].Shape).From == glm::vec3(1.0f, 2.0f, 3.0f));
			CHECK(commands[0].Color == glm::vec4(0.25f, 0.5f, 0.75f, 0.5f));
			CHECK(commands[0].Duration == 2.0f);
			const auto ray = std::get<DebugRay>(commands[1].Shape);
			CHECK(ray.Length == 5.0f);
			CHECK(ray.Direction.x == doctest::Approx(0.6f));
			CHECK(ray.Direction.y == doctest::Approx(0.8f));
			CHECK(commands[1].Color == glm::vec4(1.0f));
			CHECK(commands[1].Duration == 0.0f);
			const auto box = std::get<DebugBox>(commands[2].Shape);
			CHECK(box.HalfExtents == glm::vec3(2.0f, 3.0f, 4.0f));
			CHECK(box.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(std::get<DebugSphere>(commands[3].Shape).Radius == 2.0f);
			CHECK(std::get<DebugArrow>(commands[4].Shape).HeadSize == 0.1f);
			const auto& text = std::get<DebugText>(commands[5].Shape);
			CHECK(text.Text == "Text ✓");
			CHECK(text.Size == 16.0f);
			for (const auto& command : commands)
				CHECK(command.Depth == DebugDepthMode::Tested);
			CHECK(fixture.ExternalMutations.size() == 1);
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Debug" && counter.Member != "Break")
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls == 1);
				}
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("DebugBindings: validated primitives reach the real debug draw list")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunDebugBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("DebugBindings: malformed primitives never append a command")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
for _, fn in {
	function() Debug.DrawLine(vector.zero, vector.create(math.huge, 0, 0)) end,
	function() Debug.DrawRay(vector.zero, vector.zero) end,
	function() Debug.DrawRay(vector.create(3e38, 0, 0), vector.create(3e38, 0, 0)) end,
	function() Debug.DrawBox(vector.zero, vector.create(1, -1, 1)) end,
	function() Debug.DrawBox(vector.zero, vector.one, Color.New(1, 1, 1)) end,
	function() Debug.DrawSphere(vector.zero, 0) end,
	function() Debug.DrawSphere(vector.zero, 1e100) end,
	function() Debug.DrawArrow(vector.zero, vector.one, nil, -1) end,
	function() Debug.DrawText(vector.zero, string.char(255)) end,
	function() Debug.DrawText(vector.zero, "x\000y") end,
	function() Debug.DrawText(vector.zero, "text", nil, 0/0) end,
	function() Debug.DrawLine(vector.zero, vector.one, {}) end,
} do assert(not pcall(fn)) end
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			REQUIRE(fixture.GetDebugDraw() != nullptr);
			CHECK(fixture.GetDebugDraw()->IsEmpty());
			CHECK(fixture.ExternalMutations.empty());
		}

		TEST_CASE("DebugBindings: break pauses the editor and is harmless in both runtime modes")
		{
			for (const auto mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				Test::ScriptTestFixture fixture({ .Mode = mode });
				fixture.Environment.IsEditor = mode == RunModes::Editor;
				REQUIRE(fixture.Start().has_value());
				const auto result = fixture.Evaluate("Debug.Break(); return true");
				REQUIRE(result.has_value());
				CHECK(result->Value.Get() == Json(true));
				CHECK(fixture.PauseRequested == (mode == RunModes::Editor));
				CHECK(fixture.ExternalMutations.size() == (mode == RunModes::Editor ? 1 : 0));
			}
		}

		TEST_CASE("DebugBindings: read-only calls cannot draw or pause")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(not pcall(Debug.DrawLine, vector.zero, vector.one))
assert(not pcall(Debug.DrawRay, vector.zero, vector.one))
assert(not pcall(Debug.DrawBox, vector.zero, vector.one))
assert(not pcall(Debug.DrawSphere, vector.zero, 1))
assert(not pcall(Debug.DrawArrow, vector.zero, vector.one))
assert(not pcall(Debug.DrawText, vector.zero, "text"))
assert(not pcall(Debug.Break))
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			REQUIRE(fixture.GetDebugDraw() != nullptr);
			CHECK(fixture.GetDebugDraw()->IsEmpty());
			CHECK_FALSE(fixture.PauseRequested);
			CHECK(fixture.ExternalMutations.empty());
		}
	}

}
