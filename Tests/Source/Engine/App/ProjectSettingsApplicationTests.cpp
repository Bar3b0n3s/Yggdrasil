#include "TestsPCH.h"

#include "Engine/App/ProjectSettingsApplication.h"

#include <cstdint>
#include <optional>

namespace Engine {

	TEST_SUITE("App")
	{
		// ADR 0022 decision 12: the Runtime's factory applies its manifest through this function, so gate 8 observes it.
		TEST_CASE("ProjectSettingsApplication: a game's name, window and loop settings reach the application specification")
		{
			ApplicationSpecification specification;
			specification.Graphics.VSync = true;
			specification.MaxFrames = 7;
			specification.Loop.MaxFrameDelta = 0.5;
			const WindowSettings window{ .Title = "All Settings", .Width = 1280, .Height = 720, .VSync = false, .Fullscreen = true, .Resizable = false };
			const SimulationSettings simulation{ .FixedHz = 120, .MaxStepsPerFrame = 8, .Seed = 1337, .MaxEntities = 4096 };
			ApplyGameSettings(specification, "AllSettings", window, simulation);
			CHECK(specification.Name == "AllSettings");
			CHECK(specification.WindowSettings.Title == "All Settings");
			CHECK(specification.WindowSettings.Width == 1280);
			CHECK(specification.WindowSettings.Height == 720);
			CHECK(specification.WindowSettings.Fullscreen);
			CHECK_FALSE(specification.WindowSettings.Resizable);
			CHECK_FALSE(specification.Graphics.VSync);
			CHECK(specification.Loop.FixedHz == 120);
			CHECK(specification.Loop.MaxStepsPerFrame == 8);
			// Every other member is left alone.
			CHECK(specification.MaxFrames == std::optional<uint64_t>(7));
			CHECK(specification.Loop.MaxFrameDelta == doctest::Approx(0.5));
		}

		TEST_CASE("ProjectSettingsApplication: suite overrides replace exactly the overridden scripting settings")
		{
			const ScriptingSettings project{ .MemoryLimitMB = 128, .CallbackBudgetMs = 250, .PauseOnError = false, .BlockPlayOnTypeErrors = true };
			const ScriptingSettings inherited = ApplyTestSuiteOverrides(project, TestSuiteOverrides{});
			CHECK(inherited.MemoryLimitMB == 128);
			CHECK(inherited.CallbackBudgetMs == 250);
			CHECK_FALSE(inherited.PauseOnError);
			CHECK(inherited.BlockPlayOnTypeErrors);

			const TestSuiteOverrides overrides{ .CallbackBudgetMs = 50, .MemoryLimitMB = 16, .PauseOnError = TestSuiteOverrides::PauseOnErrorOverride::Pause };
			const ScriptingSettings overridden = ApplyTestSuiteOverrides(project, overrides);
			CHECK(overridden.MemoryLimitMB == 16);
			CHECK(overridden.CallbackBudgetMs == 50);
			CHECK(overridden.PauseOnError);
			CHECK(overridden.BlockPlayOnTypeErrors);
		}
	}

}
