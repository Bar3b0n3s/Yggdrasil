#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Reflection/VariantValue.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
	// ("Key.Space", "Gamepad.South", "Gamepad.RightTrigger"); Positive, Negative and Gamepad ("Gamepad.LeftX") form an Axis
	// action; Invert flips the axis. The project file's reader does not check control names, so a project holding a bad
	// one still opens; they are checked against Platform's key and gamepad tables by M2's InputActionMap, through
	// ValidateInputActions (Session/PlayInput.h; M34, Docs/Decisions/0049-m34-characters-input.md decisions 6 and 7):
	// when play.start builds a session, when project.setSettings writes Input (an agent's patch is refused, the editor's
	// own settings panel commits it with warnings) and in project.validate, which reports each problem as
	// INPUT_INVALID_ACTION (INPUT_UNKNOWN_ACTION is about script names, M13).
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
	// globs; Icon is a project-relative square PNG (>= 256 x 256, checked at export); Version is "major.minor.patch";
	// Company is free text under FindExportCompanyProblem's rule. Icon, Version and Company go into the exported
	// executable's resources (M15); PruneUnreferenced decides what Game.pak holds (M19).
	struct ExportSettings
	{
		// The longest Company in bytes. Every exported executable must be able to carry it: Windows' VS_VERSIONINFO block
		// holds at most 65535 bytes, and with every other string at its longest (a 64-byte ASCII Name, the most UTF-16 units
		// a Name can have, "<Name>.exe" and the version "65535.65535.65535.0") a Company of up to 32225 UTF-16 units fits.
		// A UTF-8 byte never decodes to more than one UTF-16 unit, so a byte bound below that holds on every host;
		// ExecutableResourcesTests builds the version resource at this bound.
		static constexpr size_t MaxCompanyBytes = 32000;

		std::vector<std::string> BuildScenes;
		std::vector<std::string> Exclude = { "Assets/Tests/**" };
		std::string Icon;
		std::string Version = "1.0.0";
		std::string Company;
		// M19 (Docs/Decisions/0030-m19-contract.md decision 21, B-PRUNE-GAME): Game.pak packages only what StartScene and
		// Export.BuildScenes (and a testing export's testing content) reach through references and literal Assets.Load
		// paths, instead of every asset below Assets/ that Export.Exclude leaves in (EditorCore/Export/Exporter.h), and
		// project.validate names each computed Assets.Load (SCRIPT_COMPUTED_ASSET_LOAD). Opt-in, off by default; the field
		// "PruneUnreferenced", registered after Company.
		bool PruneUnreferenced = false;
	};

	// The rule of every text an exported executable's resources carry (Export.Company, and the strings of the macOS
	// Info.plist), the same on every host: valid UTF-8, no control character (U+0000 to U+001F or U+007F) and neither U+FFFE
	// nor U+FFFF. Info.plist is XML 1.0, whose characters exclude U+FFFE, U+FFFF and every control character but tab, line
	// feed and carriage return; the rule refuses all of them, so that one rule holds on every host, although the Windows
	// version resource could carry them (the M15 fix review's C7). Returns the problem as a phrase that completes "<the
	// text> ..." (such as "holds the control character U+0009 at byte 4"), or nullopt for a valid text.
	[[nodiscard]] std::optional<std::string> FindExportTextProblem(std::string_view text);

	// Export.Company's rule, the same on every host (the M15 review's M02): at most ExportSettings::MaxCompanyBytes bytes,
	// and FindExportTextProblem's rule. Returns the problem as a phrase that completes "Export.Company ..." (such as
	// "holds the control character U+0009 at byte 4"), or nullopt for a valid Company. The settings validation reports it
	// at /Export/Company (project.setSettings and the .eproj loader), and so does the exporter's validation (ResolveExecutableResources).
	[[nodiscard]] std::optional<std::string> FindExportCompanyProblem(std::string_view company);

	// Registry struct "TestSuiteOverrides" (§11.10 "Overrides"), applied exactly (no Dist minimum) for one suite. 0 means
	// "use the project setting" for the two limits, and Inherit likewise for PauseOnError (ADR 0006 decision 11: the
	// registry has no optional fields). PauseOnError is accepted but has no effect: a test-mode ScriptEngine never pauses
	// on a script error, because the runner owns time (§11.7).
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
	// CaseTimeoutTicks and SuiteTickLimit >= 1. CoverageGates (M14, §15.6; Docs/Decisions/0022-m14-contract.md decision 3)
	// makes every unfiltered, non-recording test run of the project record coverage and evaluate the per-mode coverage
	// gates, failing the run on a gap; Projects/FeatureTest sets it, games leave it false.
	struct TestingSettings
	{
		std::vector<TestSuiteSettings> Suites;
		std::vector<std::string> Replays;
		uint32_t CaseTimeoutTicks = 600;
		uint32_t SuiteTickLimit = 36000;
		bool CoverageGates = false;
	};

	// Registry struct "SortingLayersSettings" (§6.1 "SortingLayers"; M21, Docs/Decisions/0032-m21-contract.md decision 4): the
	// sorting layers of sprites, back to front. Layers: 1 to MaxSortingLayers unique non-empty names, the first "Default"
	// (DefaultSortingLayerName), as PhysicsSettings.Layers. Registered as the section "SortingLayers" after Testing (ADR
	// 0028's phase-2 ledger); every host copies Layers into its RenderExtractionRequest::SortingLayers, and project.validate
	// checks the sprites' names against it (SPRITE_UNKNOWN_SORTING_LAYER).
	struct SortingLayersSettings
	{
		// The most sorting layers a project declares.
		static constexpr uint32_t MaxSortingLayers = 32;

		std::vector<std::string> Layers = { "Default" };
	};

	// Registry struct "Physics2DSettings" (§6.1 "Physics2D"; M21, Docs/Decisions/0032-m21-contract.md decision 9): the 2D
	// world's gravity in metres per second squared along X and Y, each component finite and at most MaxPhysicsGravity in
	// magnitude; its layers are Physics.Layers. Registered as the section "Physics2D" after SortingLayers (ADR 0028's
	// phase-2 ledger); the play session's Physics2DSystem reads it.
	struct Physics2DSettings
	{
		glm::vec2 Gravity = glm::vec2(0.0f, -9.81f);
	};

	// Registry struct "LocalizationSettings" (§6.1 "Localization", §11.5; M22, Docs/Decisions/0033-m22-contract.md decision 11).
	// Locale is the locale Loc.Get reads string tables in, fixed for each script VM's life (a play session's and an edit
	// evaluation's): empty, or a locale tag of a string table's form (1 to 35 ASCII letters, digits and '-', starting with a
	// letter: Asset/StringTableData.h, which Project cannot include). Empty reads every table in its fallback locale.
	struct LocalizationSettings
	{
		std::string Locale;
	};

	// Registry struct "UiSettings" (§6.1 "Ui", §5.7 UiInput; M22 C22.2, Docs/Decisions/0033-m22-contract.md decision 30): the
	// keyboard and gamepad navigation of the game's UI. Navigation false: focus moves only by the pointer and Ui.SetFocus,
	// and none of the bindings below, nor the stick, is read. Each list holds control names in an InputAction's Bindings
	// form ("Key.Enter", "Gamepad.South"; a gamepad control counts on any connected gamepad), keys and gamepad buttons only
	// (no mouse button or axis), unique, at most 16: a command is given when one of its controls is pressed in the tick's
	// step view. Next pressed with Key.LeftShift or Key.RightShift down is Previous. StickNavigation lets the left stick of a
	// connected gamepad step the four directions (Scene/UiInteraction.h's UiStickDirection). A control name that does not
	// parse (InputBinding::Parse) or names a mouse control or an axis fails a play session's start with Validation located
	// at /Ui/<list>/<index>, as an unknown action binding fails it at /Input/Actions. Registered by stream A with its
	// canonical fixtures, after Localization (ADR 0028's phase-2 ledger: M22's sections after Testing in landing order).
	struct UiSettings
	{
		bool Navigation = true;
		std::vector<std::string> Submit = { "Key.Enter", "Key.Space", "Gamepad.South" };
		std::vector<std::string> Cancel = { "Key.Escape", "Gamepad.East" };
		std::vector<std::string> Up = { "Key.Up", "Gamepad.DPadUp" };
		std::vector<std::string> Down = { "Key.Down", "Gamepad.DPadDown" };
		std::vector<std::string> Left = { "Key.Left", "Gamepad.DPadLeft" };
		std::vector<std::string> Right = { "Key.Right", "Gamepad.DPadRight" };
		std::vector<std::string> Next = { "Key.Tab" };
		bool StickNavigation = true;
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
		// M21 (Docs/Decisions/0032-m21-contract.md decisions 4 and 9): the sections "SortingLayers" and "Physics2D".
		SortingLayersSettings SortingLayers;
		Physics2DSettings Physics2D;
		// M22 (Docs/Decisions/0033-m22-contract.md decisions 11 and 30): the sections "Localization" and "Ui", after M21's
		// (ADR 0028's phase-2 ledger: landing order).
		LocalizationSettings Localization;
		UiSettings Ui;
	};

	// Registers the enums and structs above (InputActionType, TestPauseOnError, TestIsolation, TestMode, WindowSettings,
	// SimulationSettings, PhysicsSettings, InputAction, InputSettings, RenderingSettings, ScriptingSettings,
	// ExportSettings, TestSuiteOverrides, TestSuite, TestingSettings, SortingLayersSettings, Physics2DSettings,
	// LocalizationSettings, UiSettings, ProjectSettings) with their field metadata, the type-level validators of the rules
	// in the comments above and, for each validated type, the Generate hook that turns random field values into valid
	// settings (FieldBuilder::Generate). Call once per registry, before Freeze.
	void RegisterProjectSettingsTypes(TypeRegistry& registry);

}
