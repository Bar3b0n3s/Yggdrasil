#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <vector>

namespace Engine {

	namespace Test {

		void RunApplicationBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			fixture.Environment = { .IsEditor = true, .IsHeadless = false, .IsFocused = true, .Platform = "TestPlatform", .Version = "7.8.9", .WindowSize = glm::vec2(800.0f, 600.0f) };
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(Application.IsEditor() and not Application.IsHeadless() and Application.IsFocused())
assert(Application.GetPlatform() == "TestPlatform" and Application.GetVersion() == "7.8.9")
assert(Application.GetWindowSize() == vector.create(800, 600, 0))
Application.Quit(-12)
return Time.GetTick() == 0
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.QuitCode == -12);
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Application")
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls > 0);
				}
			fixture.Environment.IsEditor = false;
			fixture.Environment.IsHeadless = true;
			fixture.Environment.IsFocused = false;
			fixture.Environment.WindowSize = glm::vec2(0.0f);
			const auto runtime = fixture.Evaluate("Application.Quit(); return not Application.IsEditor() and Application.IsHeadless() and not Application.IsFocused() and Application.GetWindowSize() == vector.zero");
			REQUIRE(runtime.has_value());
			CHECK(runtime->Value.Get() == Json(true));
			CHECK(fixture.QuitCode == 0);
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ApplicationBindings: getters and deferred quit use the host")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunApplicationBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("ApplicationBindings: invalid exit codes and read-only quit are refused before effects")
		{
			for (const bool readOnly : { false, true })
			{
				Test::ScriptTestFixture fixture({ .ReadOnly = readOnly });
				REQUIRE(fixture.Start().has_value());
				const auto result = fixture.Evaluate(R"(
for _, value in {2147483648, -2147483649, 1.5, math.huge, 0/0, "0", false} do
	assert(not pcall(Application.Quit, value))
end
return Application.IsHeadless()
)");
				REQUIRE(result.has_value());
				CHECK(result->Value.Get() == Json(true));
				if (readOnly)
				{
					const auto refused = fixture.Evaluate("return not pcall(Application.Quit)");
					REQUIRE(refused.has_value());
					CHECK(refused->Value.Get() == Json(true));
				}
				CHECK_FALSE(fixture.QuitCode.has_value());
				CHECK(fixture.ExternalMutations.empty());
			}
		}

		TEST_CASE("ApplicationBindings: runtime modules reject load-time access before requesting a host")
		{
			auto sandbox = Sandbox::Create({ .Mode = SandboxMode::LoadTime });
			REQUIRE(sandbox.has_value());
			const auto result = (*sandbox)->Evaluate(R"(
for name, fn in {
	Scene = function() return Scene.GetEntityCount() end,
	Input = function() return Input.GetMousePosition() end,
	Time = function() return Time.GetTime() end,
	Assets = function() return Assets.Load("Assets/Missing.scene") end,
	Task = function() return Task.Spawn(function() end) end,
	Random = function() return Random.New(1) end,
	Debug = function() return Debug.Break() end,
	Log = function() return Log.Info("load-time") end,
	Application = function() return Application.IsEditor() end,
} do
	local ok, message = pcall(fn)
	assert(not ok and string.find(message, "not available at load time", 1, true), name)
end
return true
)",
				{});
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
		}
	}

}
