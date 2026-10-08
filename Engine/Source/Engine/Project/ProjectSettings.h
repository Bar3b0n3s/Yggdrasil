#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Reflection/VariantValue.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The project settings (Architecture §6.1, §11.10): one reflected struct per section of the .eproj file, so the
// serializer, automation (project.getSettings/setSettings, M4), the settings panel (M10), docs and coverage gate 8 are
// generic. Every struct registers its fields in member declaration order, which is the file's key order. Defaults below
// are the values a new project gets and what a missing key reads as; Tests/Data/Project/AllSettings.eproj sets every
// field to a non-default value (gate 8). Plain value types; thread-compatible.

namespace Engine {

	class TypeRegistry;

	// Registry struct "WindowSettings" (§6.1 "Window"). Width and Height >= 1.
	struct WindowSettings
	{
		std::string Title = "Game";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool VSync = true;
		bool Fullscreen = false;
		bool Resizable = true;
	};

	// Registry struct "SimulationSettings" (§6.1 "Simulation", §4.2, §5.7). FixedHz in [1, FrameLoopConfig::MaxFixedHz]
	// (ADR 0003 decision 8); MaxStepsPerFrame >= 1; Seed is the project half of each play session's seed (Project Seed ^
	// Scene Seed); MaxEntities >= 1 caps the entities of a session.
	struct SimulationSettings
	{
		uint32_t FixedHz = 60;
		uint32_t MaxStepsPerFrame = 5;
		uint32_t Seed = 0;
		uint32_t MaxEntities = 65536;
	};

	// Registry struct "PhysicsSettings" (§6.1 "Physics", §9). Layers: 1 to 16 unique non-empty names, the first "Default"
	// (§9.2; Docs/Decisions/0014-m11-decisions.md decision 4).
	// Collisions: pairs of declared layer names that collide (each inner array exactly 2 names); unlisted pairs do not.
	struct PhysicsSettings
	{
		glm::vec3 Gravity = glm::vec3(0.0f, -9.81f, 0.0f);
		std::vector<std::string> Layers = { "Default" };
		std::vector<std::vector<std::string>> Collisions = { std::vector<std::string>{ "Default", "Default" } };
	};

	// Registry struct "InputAction" (§6.1 "Input.Actions" values, §4.3). Bindings are the control names of a Button action
	// ("Key.Space", "Gamepad.South"); Positive, Negative and Gamepad ("Gamepad.LeftX") form an Axis action; Invert flips
	// the axis. Control names are validated against Platform's key and gamepad tables when the project is applied (M2's
	// InputActionMap), and by the project validator (INPUT_UNKNOWN_ACTION is about script names, M13).
	struct InputActionSettings
	{
		// Registry enum "InputActionType". Nested so it cannot collide with Platform's input types (M2).
		enum class ActionType : uint8_t
		{
			Button,
			Axis
		};

		ActionType Type = ActionType::Button;
		std::vector<std::string> Bindings;
		std::vector<std::string> Positive;
		std::vector<std::string> Negative;
		std::string Gamepad;
		bool Invert = false;
	};

	// Registry struct "InputSettings" (§6.1 "Input"). Actions is a Map: keys (action names, non-empty) are written sorted.
	struct InputSettings
	{
		std::map<std::string, InputActionSettings> Actions;
	};

	// Registry struct "RenderingSettings" (§6.1 "Rendering"). ShadowMapSize is a power of two in [256, 8192].
	struct RenderingSettings
	{
		uint32_t ShadowMapSize = 2048;
		bool SsaoHalfResolution = false;
	};

	// Registry struct "ScriptingSettings" (§6.1 "Scripting", §11.1). MemoryLimitMB >= 1 (soft limit, plus 16 MB headroom);
	// CallbackBudgetMs >= 10 (Dist raises a budget below 5,000 ms to 5,000 ms outside test runs).
	struct ScriptingSettings
	{
		uint32_t MemoryLimitMB = 256;
		uint32_t CallbackBudgetMs = 1000;
		bool PauseOnError = true;
		bool BlockPlayOnTypeErrors = false;
	};

	// Registry struct "ExportSettings" (§6.1 "Export", §7.6, §14.2). BuildScenes and Exclude are project-relative paths and
	// globs; Icon is a project-relative PNG (>= 256 x 256, checked at export); Version is "major.minor.patch"; Company is
	// free text. Icon, Version and Company go into the exported executable's resources (M15).
	struct ExportSettings
	{
		std::vector<std::string> BuildScenes;
		std::vector<std::string> Exclude = { "Assets/Tests/**" };
		std::string Icon;
		std::string Version = "1.0.0";
		std::string Company;
	};

	// Registry struct "TestSuiteOverrides" (§11.10 "Overrides"), applied exactly (no Dist minimum) for one suite. 0 means
	// "use the project setting" for the two limits, and Inherit likewise for PauseOnError (ADR 0006 decision 11: the
	// registry has no optional fields).
	struct TestSuiteOverrides
	{
		// Registry enum "TestPauseOnError".
		enum class PauseOnErrorOverride : uint8_t
		{
			Inherit,
			Pause,
			Continue
		};

		uint32_t CallbackBudgetMs = 0;
		uint32_t MemoryLimitMB = 0;
		PauseOnErrorOverride PauseOnError = PauseOnErrorOverride::Inherit;
	};

	// Registry struct "TestSuite" (§11.10 "Suites" entries). Script is the project-relative test script; Scene the scene it
	// runs in (empty: an empty scene); Parameters the Scene.Load parameters (free-form JSON object, a resolver-less Variant,
	// ADR 0006 decision 11); Isolation per suite or per case; Clock a list of frame deltas selecting ScriptedClock (empty:
	// lockstep ManualClock); ExpectQuit the exit code the suite must end with through Application.Quit (-1: it must not
	// quit); Modes the run modes the suite runs in (unique, non-empty).
	struct TestSuiteSettings
	{
		// Registry enum "TestIsolation".
		enum class IsolationMode : uint8_t
		{
			Suite,
			Case
		};

		// Registry enum "TestMode": the modes of §15.6.
		enum class Mode : uint8_t
		{
			Editor,
			Release,
			Dist
		};

		std::string Script;
		std::string Scene;
		VariantValue Parameters;
		IsolationMode Isolation = IsolationMode::Suite;
		std::vector<float> Clock;
		TestSuiteOverrides Overrides;
		int32_t ExpectQuit = -1;
		std::vector<Mode> Modes = { Mode::Editor, Mode::Release, Mode::Dist };
	};

	// Registry struct "TestingSettings" (§6.1 "Testing", §11.10). Replays are project-relative globs of .replay files;
	// CaseTimeoutTicks and SuiteTickLimit >= 1.
	struct TestingSettings
	{
		std::vector<TestSuiteSettings> Suites;
		std::vector<std::string> Replays;
		uint32_t CaseTimeoutTicks = 600;
		uint32_t SuiteTickLimit = 36000;
	};

	// Registry struct "ProjectSettings": the whole .eproj below its "Format"/"Version" header (§6.1). Name is the game's
	// display name (exported games use it for their user-data folder, §14.1); StartScene is the project-relative scene the
	// game starts with (empty: none yet).
	struct ProjectSettings
	{
		std::string Name = "Untitled";
		std::string StartScene;
		WindowSettings Window;
		SimulationSettings Simulation;
		PhysicsSettings Physics;
		InputSettings Input;
		RenderingSettings Rendering;
		ScriptingSettings Scripting;
		ExportSettings Export;
		TestingSettings Testing;
	};

	// Registers the enums and structs above (InputActionType, TestPauseOnError, TestIsolation, TestMode, WindowSettings,
	// SimulationSettings, PhysicsSettings, InputAction, InputSettings, RenderingSettings, ScriptingSettings,
	// ExportSettings, TestSuiteOverrides, TestSuite, TestingSettings, ProjectSettings) with their field metadata, the
	// type-level validators of the rules in the comments above and, for each validated type, the Generate hook that turns
	// random field values into valid settings (FieldBuilder::Generate). Call once per registry, before Freeze.
	void RegisterProjectSettingsTypes(TypeRegistry& registry);

}
