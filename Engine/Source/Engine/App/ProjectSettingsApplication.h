#pragma once

#include "Engine/App/Application.h"
#include "Engine/Core/Base.h"
#include "Engine/Project/ProjectSettings.h"

#include <string_view>

// The settings sections that configure a game process or a test suite, applied in one place each (Architecture §15.5
// "Settings coverage", gate 8; Docs/Decisions/0022-m14-contract.md decision 12). The Runtime's ApplicationFactory and the
// editor and Runtime FeatureTest hosts call these instead of copying fields themselves, so the settings observation test
// observes what those processes actually apply. Pure; thread-safe.

namespace Engine {

	// Applies a game's identity, window and loop settings: Name to ApplicationSpecification::Name; Window.Title, Width,
	// Height, Resizable and Fullscreen to WindowSettings; Window.VSync to Graphics.VSync; Simulation.FixedHz and
	// MaxStepsPerFrame to Loop. Leaves every other member alone. The Runtime passes its manifest's copies (GameManifest Name,
	// Window, Simulation), which the exporter takes from the project unchanged.
	void ApplyGameSettings(ApplicationSpecification& specification, std::string_view name, const WindowSettings& window,
		const SimulationSettings& simulation);

	// The scripting settings a suite session runs with (§11.10 "Overrides", applied exactly, no Dist minimum): a non-zero
	// CallbackBudgetMs or MemoryLimitMB replaces the project's value, PauseOnError Pause or Continue replaces PauseOnError
	// (which a test-mode ScriptEngine ignores, §11.7) and Inherit keeps it; BlockPlayOnTypeErrors is the project's.
	[[nodiscard]] ScriptingSettings ApplyTestSuiteOverrides(const ScriptingSettings& project, const TestSuiteOverrides& overrides);

}
