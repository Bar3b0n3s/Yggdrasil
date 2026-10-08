#include "EnginePCH.h"
#include "Engine/Project/ProjectSettings.h"

#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <bit>

namespace Engine {

	namespace Utils {

		// Architecture §9.2 ("up to 16 named project layers"); Physics/PhysicsLayers.h's MaxPhysicsLayers, which Project
		// cannot include (ADR 0014 decision 4; ProjectSettingsTests checks that they agree).
		static constexpr size_t MaxProjectPhysicsLayers = 16;
		// The largest magnitude of a Physics.Gravity component, in m/s^2: Physics/PhysicsTypes.h's MaxPhysicsGravity, which
		// the physics world refuses beyond (ADR 0014 decision 34; ProjectSettingsTests checks that they agree).
		static constexpr double MaxProjectGravity = 1.0e12;
		static constexpr std::string_view DefaultLayer = "Default";
		static constexpr double MinShadowMapSize = 256.0;
		static constexpr double MaxShadowMapSize = 8192.0;
		static constexpr uint32_t MinCallbackBudgetMs = 10;
		static constexpr size_t VersionPartCount = 3;

		static bool IsDigits(std::string_view text)
		{
			return !text.empty() && std::all_of(text.begin(), text.end(), [](char character)
			{
				return character >= '0' && character <= '9';
			});
		}

		// "major.minor.patch": three non-empty runs of ASCII digits separated by dots.
		static bool IsVersionText(std::string_view text)
		{
			size_t parts = 0;
			size_t begin = 0;
			while (begin <= text.size())
			{
				const size_t end = std::min(text.find('.', begin), text.size());
				if (!IsDigits(text.substr(begin, end - begin)))
					return false;
				++parts;
				begin = end + 1;
			}
			return parts == VersionPartCount;
		}

		static void ValidatePhysics(const PhysicsSettings& physics, ValidationContext& context)
		{
			const std::vector<std::string>& layers = physics.Layers;
			if (layers.empty() || layers.size() > MaxProjectPhysicsLayers)
				context.Error("Layers", std::format("must declare 1 to {} layers (got {})", MaxProjectPhysicsLayers, layers.size()));

			context.PushKey("Layers");
			for (size_t index = 0; index < layers.size(); ++index)
			{
				const std::string key = std::to_string(index);
				const auto previous = layers.begin() + static_cast<std::ptrdiff_t>(index);
				if (index == 0 && layers[index] != DefaultLayer)
					context.Error(key, std::format("the first layer must be '{}' (got '{}')", DefaultLayer, layers[index]));
				else if (layers[index].empty())
					context.Error(key, "a layer name must not be empty");
				else if (std::find(layers.begin(), previous, layers[index]) != previous)
					context.Error(key, std::format("the layer '{}' is declared twice", layers[index]));
			}
			context.PopKey();

			context.PushKey("Collisions");
			for (size_t index = 0; index < physics.Collisions.size(); ++index)
			{
				const std::vector<std::string>& pair = physics.Collisions[index];
				const std::string key = std::to_string(index);
				if (pair.size() != 2)
				{
					context.Error(key, std::format("a collision pair names exactly 2 layers (got {})", pair.size()));
					continue;
				}
				context.PushKey(key);
				for (size_t side = 0; side < pair.size(); ++side)
				{
					if (std::find(layers.begin(), layers.end(), pair[side]) == layers.end())
						context.Error(std::to_string(side), std::format("'{}' is not a declared layer", pair[side]));
				}
				context.PopKey();
			}
			context.PopKey();
		}

		static void GeneratePhysics(PhysicsSettings& physics, Random& random)
		{
			std::vector<std::string> layers = { std::string(DefaultLayer) };
			for (const std::string& layer : physics.Layers)
			{
				if (layers.size() == MaxProjectPhysicsLayers)
					break;
				if (!layer.empty() && std::find(layers.begin(), layers.end(), layer) == layers.end())
					layers.push_back(layer);
			}
			physics.Layers = std::move(layers);

			const int64_t lastLayer = static_cast<int64_t>(physics.Layers.size()) - 1;
			for (std::vector<std::string>& pair : physics.Collisions)
			{
				pair.resize(2);
				for (std::string& layer : pair)
					layer = physics.Layers[static_cast<size_t>(random.RangeInt(0, lastLayer))];
			}
		}

		static void ValidateInput(const InputSettings& input, ValidationContext& context)
		{
			if (!input.Actions.contains(std::string()))
				return;
			// Located at the empty key itself: "/Actions/".
			context.PushKey("Actions");
			context.PushKey("");
			context.Error("", "an action name must not be empty");
			context.PopKey();
			context.PopKey();
		}

		static void GenerateInput(InputSettings& input, Random& /*random*/)
		{
			input.Actions.erase(std::string());
		}

		static void ValidateRendering(const RenderingSettings& rendering, ValidationContext& context)
		{
			if (!std::has_single_bit(rendering.ShadowMapSize))
				context.Error("ShadowMapSize", std::format("must be a power of two (got {})", rendering.ShadowMapSize));
		}

		static void GenerateRendering(RenderingSettings& rendering, Random& random)
		{
			// 2^8 = 256 to 2^13 = 8192, the range the field metadata allows.
			rendering.ShadowMapSize = 1u << static_cast<uint32_t>(random.RangeInt(8, 13));
		}

		static void ValidateExport(const ExportSettings& settings, ValidationContext& context)
		{
			if (!IsVersionText(settings.Version))
				context.Error("Version", std::format("must have the form major.minor.patch (got '{}')", settings.Version));
		}

		static void GenerateExport(ExportSettings& settings, Random& random)
		{
			const int64_t major = random.RangeInt(0, 99);
			const int64_t minor = random.RangeInt(0, 99);
			const int64_t patch = random.RangeInt(0, 999);
			settings.Version = std::format("{}.{}.{}", major, minor, patch);
		}

		static void ValidateOverrides(const TestSuiteOverrides& overrides, ValidationContext& context)
		{
			if (overrides.CallbackBudgetMs != 0 && overrides.CallbackBudgetMs < MinCallbackBudgetMs)
			{
				context.Error("CallbackBudgetMs",
					std::format("must be 0 (the project setting) or at least {} ms (got {})", MinCallbackBudgetMs, overrides.CallbackBudgetMs));
			}
		}

		static void GenerateOverrides(TestSuiteOverrides& overrides, Random& random)
		{
			if (overrides.CallbackBudgetMs != 0 && overrides.CallbackBudgetMs < MinCallbackBudgetMs)
				overrides.CallbackBudgetMs = static_cast<uint32_t>(random.RangeInt(MinCallbackBudgetMs, 60000));
		}

		static void ValidateSuite(const TestSuiteSettings& suite, ValidationContext& context)
		{
			const Json& parameters = suite.Parameters.Get();
			if (!parameters.is_null() && !parameters.is_object())
				context.Error("Parameters", "must be a JSON object, or null for none");

			context.PushKey("Clock");
			for (size_t index = 0; index < suite.Clock.size(); ++index)
			{
				if (!(suite.Clock[index] > 0.0f))
					context.Error(std::to_string(index), std::format("a frame delta must be > 0 (got {})", suite.Clock[index]));
			}
			context.PopKey();

			if (suite.Modes.empty())
				context.Error("Modes", "must list at least one run mode");
			context.PushKey("Modes");
			for (size_t index = 0; index < suite.Modes.size(); ++index)
			{
				const auto previous = suite.Modes.begin() + static_cast<std::ptrdiff_t>(index);
				if (std::find(suite.Modes.begin(), previous, suite.Modes[index]) != previous)
					context.Error(std::to_string(index), "the run mode is listed twice");
			}
			context.PopKey();
		}

		static void GenerateSuite(TestSuiteSettings& suite, Random& random)
		{
			if (!suite.Parameters.IsNull() && !suite.Parameters.Get().is_object())
			{
				Json parameters = Json::object();
				parameters["Value"] = random.RangeInt(-1000, 1000);
				suite.Parameters.Set(std::move(parameters));
			}

			for (float& delta : suite.Clock)
			{
				if (!(delta > 0.0f))
					delta = static_cast<float>(random.RangeDouble(0.001, 0.1));
			}

			std::vector<TestSuiteSettings::Mode> modes;
			for (const TestSuiteSettings::Mode mode : suite.Modes)
			{
				if (std::find(modes.begin(), modes.end(), mode) == modes.end())
					modes.push_back(mode);
			}
			if (modes.empty())
				modes.push_back(static_cast<TestSuiteSettings::Mode>(random.RangeInt(0, 2)));
			suite.Modes = std::move(modes);
		}

	}

	void RegisterProjectSettingsTypes(TypeRegistry& registry)
	{
		registry.Enum<InputActionSettings::ActionType>("InputActionType", "How an input action reads its controls.")
			.Entry(InputActionSettings::ActionType::Button, "Button", "Pressed or released, driven by the action's Bindings.")
			.Entry(InputActionSettings::ActionType::Axis, "Axis", "A value in [-1, 1] from the Positive and Negative controls and the Gamepad axis.");

		registry.Enum<TestSuiteOverrides::PauseOnErrorOverride>("TestPauseOnError", "Whether a script error pauses one test suite.")
			.Entry(TestSuiteOverrides::PauseOnErrorOverride::Inherit, "Inherit", "Use the project's Scripting.PauseOnError.")
			.Entry(TestSuiteOverrides::PauseOnErrorOverride::Pause, "Pause", "Pause the suite on a script error.")
			.Entry(TestSuiteOverrides::PauseOnErrorOverride::Continue, "Continue", "Keep running after a script error.");

		registry.Enum<TestSuiteSettings::IsolationMode>("TestIsolation", "How much state the cases of a test suite share.")
			.Entry(TestSuiteSettings::IsolationMode::Suite, "Suite", "Every case of the suite runs in one play session.")
			.Entry(TestSuiteSettings::IsolationMode::Case, "Case", "Every case runs in a fresh play session.");

		registry.Enum<TestSuiteSettings::Mode>("TestMode", "A run mode a test suite runs in.")
			.Entry(TestSuiteSettings::Mode::Editor, "Editor", "The editor's play mode, headless lockstep included.")
			.Entry(TestSuiteSettings::Mode::Release, "Release", "An exported Release build.")
			.Entry(TestSuiteSettings::Mode::Dist, "Dist", "An exported Dist build.");

		registry.Struct<WindowSettings>("WindowSettings", "The window of the exported game.")
			.Field("Title", &WindowSettings::Title, "Title of the game window.")
			.Field("Width", &WindowSettings::Width, "Initial client width of the window in pixels.", { .Min = 1.0, .Unit = "px" })
			.Field("Height", &WindowSettings::Height, "Initial client height of the window in pixels.", { .Min = 1.0, .Unit = "px" })
			.Field("VSync", &WindowSettings::VSync, "Synchronizes presentation with the display's refresh.")
			.Field("Fullscreen", &WindowSettings::Fullscreen, "Starts the game in fullscreen.")
			.Field("Resizable", &WindowSettings::Resizable, "Lets the player resize the window.");

		registry.Struct<SimulationSettings>("SimulationSettings", "The fixed-step simulation of play sessions.")
			.Field("FixedHz", &SimulationSettings::FixedHz, "Fixed simulation steps per second.",
				{ .Min = 1.0, .Max = static_cast<double>(FrameLoopConfig::MaxFixedHz), .Unit = "Hz" })
			.Field("MaxStepsPerFrame", &SimulationSettings::MaxStepsPerFrame,
				"Most fixed steps one frame runs; beyond it the simulation slows down instead of falling further behind.", { .Min = 1.0 })
			.Field("Seed", &SimulationSettings::Seed, "The project half of each play session's random seed (Project Seed ^ Scene Seed).")
			.Field("MaxEntities", &SimulationSettings::MaxEntities, "Most entities one play session may hold.", { .Min = 1.0 });

		registry.Struct<PhysicsSettings>("PhysicsSettings", "World gravity and the collision layers of physics bodies.")
			.Field("Gravity", &PhysicsSettings::Gravity, "World gravity in metres per second squared; each component at most 1e12 in magnitude.",
				{ .Min = -Utils::MaxProjectGravity, .Max = Utils::MaxProjectGravity, .Unit = "m/s^2" })
			.Field("Layers", &PhysicsSettings::Layers, "Collision layer names, unique and non-empty: 1 to 16, the first one \"Default\".")
			.Field("Collisions", &PhysicsSettings::Collisions, "Pairs of declared layer names whose bodies collide; unlisted pairs do not.")
			.Validate(&Utils::ValidatePhysics)
			.Generate(&Utils::GeneratePhysics);

		registry.Struct<InputActionSettings>("InputAction", "One named input action and the controls that drive it.")
			.Field("Type", &InputActionSettings::Type, "Button or Axis.")
			.Field("Bindings", &InputActionSettings::Bindings, "Controls of a Button action, such as \"Key.Space\" or \"Gamepad.South\".")
			.Field("Positive", &InputActionSettings::Positive, "Controls that push an Axis action towards +1.")
			.Field("Negative", &InputActionSettings::Negative, "Controls that push an Axis action towards -1.")
			.Field("Gamepad", &InputActionSettings::Gamepad, "Gamepad axis of an Axis action, such as \"Gamepad.LeftX\"; empty for none.")
			.Field("Invert", &InputActionSettings::Invert, "Flips the sign of an Axis action.");

		registry.Struct<InputSettings>("InputSettings", "The input actions that scripts read by name.")
			.Field("Actions", &InputSettings::Actions, "Input actions by non-empty name, written sorted by name.")
			.Validate(&Utils::ValidateInput)
			.Generate(&Utils::GenerateInput);

		registry.Struct<RenderingSettings>("RenderingSettings", "Project-wide rendering quality settings.")
			.Field("ShadowMapSize", &RenderingSettings::ShadowMapSize, "Shadow map resolution in texels: a power of two from 256 to 8192.",
				{ .Min = Utils::MinShadowMapSize, .Max = Utils::MaxShadowMapSize, .Unit = "px" })
			.Field("SsaoHalfResolution", &RenderingSettings::SsaoHalfResolution, "Computes ambient occlusion at half resolution.")
			.Validate(&Utils::ValidateRendering)
			.Generate(&Utils::GenerateRendering);

		registry.Struct<ScriptingSettings>("ScriptingSettings", "Limits and error behaviour of the script VM.")
			.Field("MemoryLimitMB", &ScriptingSettings::MemoryLimitMB, "Soft memory limit of the script VM in megabytes (plus 16 MB headroom).",
				{ .Min = 1.0, .Unit = "MB" })
			.Field("CallbackBudgetMs", &ScriptingSettings::CallbackBudgetMs,
				"Longest one script callback may run, in milliseconds; Dist raises a budget below 5000 ms to 5000 ms outside test runs.",
				{ .Min = static_cast<double>(Utils::MinCallbackBudgetMs), .Unit = "ms" })
			.Field("PauseOnError", &ScriptingSettings::PauseOnError, "Pauses the play session when a script raises an error.")
			.Field("BlockPlayOnTypeErrors", &ScriptingSettings::BlockPlayOnTypeErrors,
				"Refuses to start play while the type checker reports errors.");

		registry.Struct<ExportSettings>("ExportSettings", "What an exported game contains and how its executable is labelled.")
			.Field("BuildScenes", &ExportSettings::BuildScenes, "Project-relative scenes packed into the export.")
			.Field("Exclude", &ExportSettings::Exclude, "Project-relative globs of assets left out of the export.")
			.Field("Icon", &ExportSettings::Icon, "Project-relative PNG used as the executable's icon, at least 256 x 256 pixels.")
			.Field("Version", &ExportSettings::Version, "Version of the exported game, in the form major.minor.patch.")
			.Field("Company", &ExportSettings::Company, "Company name written into the executable's resources.")
			.Validate(&Utils::ValidateExport)
			.Generate(&Utils::GenerateExport);

		registry.Struct<TestSuiteOverrides>("TestSuiteOverrides", "Scripting limits of one test suite, applied exactly (no Dist minimum).")
			.Field("CallbackBudgetMs", &TestSuiteOverrides::CallbackBudgetMs,
				"Callback budget of the suite in milliseconds: 0 uses the project setting, any other value is at least 10.", { .Unit = "ms" })
			.Field("MemoryLimitMB", &TestSuiteOverrides::MemoryLimitMB, "Script memory limit of the suite in megabytes: 0 uses the project setting.",
				{ .Unit = "MB" })
			.Field("PauseOnError", &TestSuiteOverrides::PauseOnError, "Whether a script error pauses the suite; Inherit uses the project setting.")
			.Validate(&Utils::ValidateOverrides)
			.Generate(&Utils::GenerateOverrides);

		registry.Struct<TestSuiteSettings>("TestSuite", "One suite of the test runner.")
			.Field("Script", &TestSuiteSettings::Script, "Project-relative test script.")
			.Field("Scene", &TestSuiteSettings::Scene, "Project-relative scene the suite runs in; empty runs it in an empty scene.")
			.Field("Parameters", &TestSuiteSettings::Parameters, "Scene.Load parameters of the suite's scene: a JSON object, or null for none.")
			.Field("Isolation", &TestSuiteSettings::Isolation, "Whether the cases share one play session or each gets a fresh one.")
			.Field("Clock", &TestSuiteSettings::Clock, "Frame deltas in seconds that select ScriptedClock; empty runs lockstep with ManualClock.",
				{ .Unit = "s" })
			.Field("Overrides", &TestSuiteSettings::Overrides, "Scripting limits that apply to this suite only.")
			.Field("ExpectQuit", &TestSuiteSettings::ExpectQuit, "Exit code the suite must end with through Application.Quit; -1: it must not quit.",
				{ .Min = -1.0 })
			.Field("Modes", &TestSuiteSettings::Modes, "Run modes the suite runs in, unique and non-empty.")
			.Validate(&Utils::ValidateSuite)
			.Generate(&Utils::GenerateSuite);

		registry.Struct<TestingSettings>("TestingSettings", "The project's test suites and recorded replays.")
			.Field("Suites", &TestingSettings::Suites, "Test suites in run order.")
			.Field("Replays", &TestingSettings::Replays, "Project-relative globs of the .replay files the test runner verifies.")
			.Field("CaseTimeoutTicks", &TestingSettings::CaseTimeoutTicks, "Ticks after which a test case fails with a timeout.",
				{ .Min = 1.0, .Unit = "ticks" })
			.Field("SuiteTickLimit", &TestingSettings::SuiteTickLimit, "Ticks after which a whole test suite fails with a timeout.",
				{ .Min = 1.0, .Unit = "ticks" });

		registry.Struct<ProjectSettings>("ProjectSettings", "The settings of a project: the .eproj file below its header.")
			.Field("Name", &ProjectSettings::Name, "Display name of the game; exported games name their user-data folder after it.")
			.Field("StartScene", &ProjectSettings::StartScene, "Project-relative scene the game starts with; empty while none is chosen.")
			.Field("Window", &ProjectSettings::Window, "The window of the exported game.")
			.Field("Simulation", &ProjectSettings::Simulation, "The fixed-step simulation.")
			.Field("Physics", &ProjectSettings::Physics, "Gravity and collision layers.")
			.Field("Input", &ProjectSettings::Input, "Named input actions.")
			.Field("Rendering", &ProjectSettings::Rendering, "Rendering quality.")
			.Field("Scripting", &ProjectSettings::Scripting, "Script VM limits and error behaviour.")
			.Field("Export", &ProjectSettings::Export, "Contents and labelling of exported games.")
			.Field("Testing", &ProjectSettings::Testing, "Test suites and replays.");
	}

}
