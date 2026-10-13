#include "EnginePCH.h"
#include "Engine/App/ProjectSettingsApplication.h"

namespace Engine {

	void ApplyGameSettings(ApplicationSpecification& specification, std::string_view name, const WindowSettings& window,
		const SimulationSettings& simulation)
	{
		specification.Name = std::string(name);
		specification.WindowSettings = WindowSpecification{
			.Title = window.Title,
			.Width = window.Width,
			.Height = window.Height,
			.Resizable = window.Resizable,
			.Fullscreen = window.Fullscreen,
		};
		specification.Graphics.VSync = window.VSync;
		specification.Loop.FixedHz = simulation.FixedHz;
		specification.Loop.MaxStepsPerFrame = simulation.MaxStepsPerFrame;
	}

	ScriptingSettings ApplyTestSuiteOverrides(const ScriptingSettings& project, const TestSuiteOverrides& overrides)
	{
		ScriptingSettings settings = project;
		if (overrides.CallbackBudgetMs != 0)
			settings.CallbackBudgetMs = overrides.CallbackBudgetMs;
		if (overrides.MemoryLimitMB != 0)
			settings.MemoryLimitMB = overrides.MemoryLimitMB;
		if (overrides.PauseOnError != TestSuiteOverrides::PauseOnErrorOverride::Inherit)
			settings.PauseOnError = overrides.PauseOnError == TestSuiteOverrides::PauseOnErrorOverride::Pause;
		return settings;
	}

}
