#include "TestsPCH.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Export/ExecutableResources.h"
#include "EditorCore/Export/Exporter.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectValidator.h"
#include "Engine/App/ProjectSettingsApplication.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Platform/Events.h"
#include "Engine/Platform/Input/InputActionMap.h"
#include "Engine/Platform/Input/InputState.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/TrackingAllocator.h"
#include "Engine/Session/PlayInput.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"
#include "Support/DefaultFields.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"
#include "Support/TestTimeout.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace Engine {

	namespace {

		constexpr uint64_t MiB = 1024ull * 1024ull;

		// The failure bound of the waits for the suite's test.run (it simulates more ticks than the default suite tick limit:
		// about 25 s in Debug, longer on a loaded machine) and for the export: far above how long either takes and below the
		// case's timeout, so only a hang reaches it (WaitUntil.h; Test::DefaultWaitBound is for short waits).
		constexpr std::chrono::seconds LongOperationWaitBound{ 480 };

		ProjectSettings LoadSettings(const TypeRegistry& registry, std::string_view relative)
		{
			const Result<std::string> text = Test::ReadTestDataText(relative);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			ProjectLoadReport report;
			Result<ProjectSettings> settings = ProjectSerializer::LoadFromString(*text, registry, ProjectLoadOptions{}, report);
			REQUIRE_MESSAGE(settings.has_value(), settings.error().ToString());
			return std::move(*settings);
		}

		// The project the observation test opens (ADR 0022 decision 12): Tests/Data/SettingsObservation/Project/ holds every
		// file AllSettings.eproj names (Assets/Icon.png, a PNG of at least 256 x 256; the start, build and suite scenes; the
		// suite script; whatever else a consumer needs), written by Tests/Data/Generate/MakeValidationFixtures.py. It is copied
		// into the test's directory together with Tests/Data/Project/AllSettings.eproj, so the run never writes Tests/Data.
		std::filesystem::path CopyObservationProject(Test::EditorTestFixture& fixture)
		{
			const std::filesystem::path source = Test::GetTestDataPath("SettingsObservation/Project");
			const std::filesystem::path target = fixture.GetDirectory().GetPath() / "AllSettings";
			std::error_code error;
			REQUIRE_MESSAGE(std::filesystem::is_directory(source, error), "missing fixture project ", source.generic_string());
			std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
			REQUIRE_MESSAGE(!error, error.message());
			std::filesystem::copy_file(Test::GetTestDataPath("Project/AllSettings.eproj"), target / "AllSettings.eproj", error);
			REQUIRE_MESSAGE(!error, error.message());
			return target;
		}

		// Every serialized leaf of `type` as gate 8 names it: a JSON pointer, "*" for an array element or map value of a struct.
		void CollectLeafPointers(const StructInfo& type, const std::string& pointer, std::set<std::string>& leaves)
		{
			for (const Scope<FieldInfo>& field : type.GetFields())
			{
				if (!field->GetMeta().Serialized)
					continue;
				const std::string fieldPointer = JsonReader::AppendPointer(pointer, field->GetName());
				const TypeInfo& fieldType = field->GetType();
				if (fieldType.GetKind() == FieldType::Struct)
				{
					CollectLeafPointers(*fieldType.GetStruct(), fieldPointer, leaves);
					continue;
				}
				const TypeInfo* element = fieldType.GetElement();
				if (element != nullptr && element->GetKind() == FieldType::Struct)
				{
					CollectLeafPointers(*element->GetStruct(), fieldPointer + "/*", leaves);
					continue;
				}
				leaves.insert(fieldPointer);
			}
		}

		Json CallOrFail(Test::AutomationTestClient& client, std::string_view method, const Json& params)
		{
			Result<Json> result = client.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), method, ": ", (result ? std::string() : result.error().ToString()));
			return std::move(*result);
		}

		void ApplyPatch(EditorContext& editor, std::string_view patchText)
		{
			const Result<Json> patch = JsonReader::Parse(patchText);
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Observation");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
		}

		// The InputState after `events` (gamepad 0 connected first) and one step latch.
		InputState MakeInput(std::initializer_list<Event> events)
		{
			InputState state;
			state.Inject(GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Connected });
			for (const Event& event : events)
				state.Inject(event);
			state.LatchStep();
			return state;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Architecture §15.5 "Settings coverage" and §15.6 gate 8 (ADR 0022 decision 12). Every leaf JSON pointer of
		// ProjectSettings is observed through its production consumer, never by reading the settings struct back: the game
		// process's application specification, the exporter's executable resources and its Export.Exclude, the runner's
		// inventory and run, the play session (its fixed delta, seed, entity cap and view, its physics world's gravity and
		// layers, its input actions, its extraction's quality, its ScriptEngine's limits and enforcement), the play
		// controller's PauseOnError and BlockPlayOnTypeErrors behaviour, the suite overrides' application and the validator's
		// StartScene and BuildScenes checks. The test ends by comparing the observed pointers with the reflected leaf walk and
		// naming each unobserved one.
		TEST_CASE("ProjectSettings: every AllSettings.eproj value is observed by its consumer")
		{
			Test::SetTestCaseTimeLimit(600.0);
			Test::EditorTestFixture fixture("SettingsObservation");
			const std::filesystem::path project = CopyObservationProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor, Test::MakeTestServerSpecification(), false);
			CallOrFail(client, "project.open", Json{ { "path", FileSystem::PathToUtf8(project) } });
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			// The expected values only: every observation below reads a consumer's output.
			const ProjectSettings settings = LoadSettings(*registry, "Project/AllSettings.eproj");
			REQUIRE_FALSE(settings.Testing.Suites.empty());
			const TestSuiteSettings& suite = settings.Testing.Suites.front();
			std::set<std::string> observed;
			const auto observe = [&observed](std::string_view pointer, bool seen)
			{
				CHECK_MESSAGE(seen, "not observed: ", std::string(pointer));
				if (seen)
					observed.insert(std::string(pointer));
			};

			// The game process (the Runtime's ApplicationFactory applies its manifest, which the exporter copies from the
			// project, through ApplyGameSettings).
			ApplicationSpecification application;
			ApplyGameSettings(application, settings.Name, settings.Window, settings.Simulation);
			observe("/Name", application.Name == settings.Name);
			observe("/Window/Title", application.WindowSettings.Title == settings.Window.Title);
			observe("/Window/Width", application.WindowSettings.Width == settings.Window.Width);
			observe("/Window/Height", application.WindowSettings.Height == settings.Window.Height);
			observe("/Window/VSync", application.Graphics.VSync == settings.Window.VSync);
			observe("/Window/Fullscreen", application.WindowSettings.Fullscreen == settings.Window.Fullscreen);
			observe("/Window/Resizable", application.WindowSettings.Resizable == settings.Window.Resizable);
			observe("/Simulation/FixedHz", application.Loop.FixedHz == settings.Simulation.FixedHz);
			observe("/Simulation/MaxStepsPerFrame", application.Loop.MaxStepsPerFrame == settings.Simulation.MaxStepsPerFrame);

			// The exporter's executable resources, from the open project's settings.
			const auto resources = ResolveExecutableResources(editor, editor.GetProject().GetSettings().Export);
			REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
			observe("/Export/Icon", resources->Icon == settings.Export.Icon);
			observe("/Export/Version", resources->VersionMajor == 2 && resources->VersionMinor == 3 && resources->VersionPatch == 4);
			observe("/Export/Company", resources->Company == settings.Export.Company);

			// The runner's inventory (test.list, FeatureTestRunner::Discover): the suite's script, scene and modes, the cases'
			// effective timeout and the replay globs.
			const Json inventory = CallOrFail(client, "test.list", Json::object());
			REQUIRE(inventory["suites"].size() == 1);
			const Json& listed = inventory["suites"][0];
			observe("/Testing/Suites/*/Script", listed["script"] == Json(suite.Script));
			observe("/Testing/Suites/*/Scene", listed["scene"] == Json(suite.Scene));
			observe("/Testing/Suites/*/Modes", listed["modes"] == Json::array({ "Editor", "Release" }));
			bool timeouts = !listed["cases"].empty();
			for (const Json& item : listed["cases"])
				timeouts = timeouts && item["timeoutTicks"] == Json(settings.Testing.CaseTimeoutTicks);
			observe("/Testing/CaseTimeoutTicks", timeouts);
			observe("/Testing/Replays", inventory["replays"] == Json::array({ "Assets/Tests/Replays/Main.replay" }));

			// The runner's run of the suite. Its session runs with the suite's overrides (the live VM's allocator); its cases
			// pass only when the scene, parameters, clock, isolation and both limits reached it, and it ends with the
			// expected quit. A filtered run of a coverage project reports the six gates unevaluated.
			static_cast<void>(client.Submit("test.run", Json{ { "filter", "Main/" } }));
			std::optional<uint64_t> suiteSoftLimit;
			std::optional<Json> run;
			REQUIRE(Test::WaitUntil([&client, &editor, &suiteSoftLimit, &run]()
			{
				client.GetServer().Pump();
				if (const PlaySession* session = editor.GetPlay().GetSession(); session != nullptr && session->GetScripts() != nullptr && !suiteSoftLimit)
					suiteSoftLimit = session->GetScripts()->GetMemoryState().SoftLimitBytes;
				for (Json& response : client.GetServer().TakeInProcessResponses(client.GetClient()))
					run = std::move(response);
				return run.has_value();
			}, LongOperationWaitBound));
			REQUIRE_MESSAGE(run->contains("result"), run->dump());
			const Json& result = (*run)["result"];
			observe("/Testing/Suites/*/Overrides/MemoryLimitMB", suiteSoftLimit == std::optional<uint64_t>(suite.Overrides.MemoryLimitMB * MiB));
			REQUIRE(result["suites"].size() == 1);
			const Json& suiteResult = result["suites"][0];
			const auto caseStatus = [&result](std::string_view name)
			{
				for (const Json& row : result["cases"])
				{
					if (row["case"] == Json(name))
						return row;
				}
				return Json();
			};
			observe("/Testing/Suites/*/Parameters", caseStatus("parameters reach the scene")["status"] == Json("passed"));
			observe("/Testing/Suites/*/Clock", caseStatus("a scripted clock does not step once per frame")["status"] == Json("passed"));
			const bool isolated = caseStatus("case isolation restarts the scene")["status"] == Json("passed")
				&& caseStatus("case isolation restarts the scene again")["status"] == Json("passed");
			observe("/Testing/Suites/*/Isolation", isolated);
			const Json quit = caseStatus("the suite quits with its expected code");
			observe("/Testing/Suites/*/ExpectQuit", quit["status"] == Json("quit") && quit["quitCode"] == Json(3) && quit["quitExpected"] == Json(true));
			uint32_t suiteTicks = 0;
			if (const auto ticks = JsonReader(suiteResult).ReadMember<uint32_t>("ticks"))
				suiteTicks = *ticks;
			observe("/Testing/SuiteTickLimit", suiteResult["passed"] == Json(true) && suiteTicks > TestingSettings{}.SuiteTickLimit);
			bool unevaluated = result["coverage"]["gates"].size() == 6;
			for (const Json& gate : result["coverage"]["gates"])
				unevaluated = unevaluated && gate["evaluated"] == Json(false);
			observe("/Testing/CoverageGates", unevaluated);

			// The suite overrides as both hosts apply them (§11.10, no Dist minimum): each replaces its project value, and
			// PauseOnError "Continue" turns a project's pause off.
			const ScriptingSettings overridden = ApplyTestSuiteOverrides(ScriptingSettings{}, suite.Overrides);
			observe("/Testing/Suites/*/Overrides/CallbackBudgetMs", overridden.CallbackBudgetMs == suite.Overrides.CallbackBudgetMs);
			observe("/Testing/Suites/*/Overrides/PauseOnError", !overridden.PauseOnError && ScriptingSettings{}.PauseOnError);
			CHECK(overridden.MemoryLimitMB == suite.Overrides.MemoryLimitMB);

			// A lockstep play session of the start scene.
			CallOrFail(client, "play.start", Json{ { "lockstep", true }, { "paused", true }, { "scene", settings.StartScene } });
			PlaySession* session = editor.GetPlay().GetSession();
			REQUIRE(session != nullptr);
			observe("/Simulation/Seed", session->GetSeed() == PlaySession::ComputeSessionSeed(settings.Simulation.Seed, 7));
			observe("/Simulation/MaxEntities", session->GetMaxEntities() == settings.Simulation.MaxEntities);
			CHECK(session->GetFixedDelta() == doctest::Approx(1.0 / static_cast<double>(settings.Simulation.FixedHz)));
			CHECK(session->GetViewSize() == glm::uvec2(settings.Window.Width, settings.Window.Height));
			observe("/Physics/Gravity", session->GetPhysics().GetGravity() == settings.Physics.Gravity);
			const PhysicsLayerTable& layers = session->GetPhysics().GetLayers();
			const std::vector<std::string> layerNames(layers.GetNames().begin(), layers.GetNames().end());
			observe("/Physics/Layers", layerNames == settings.Physics.Layers);
			const auto collide = [&layers](std::string_view a, std::string_view b)
			{
				const std::optional<uint32_t> first = layers.FindLayer(a);
				const std::optional<uint32_t> second = layers.FindLayer(b);
				return first.has_value() && second.has_value() && layers.LayersCollide(*first, *second);
			};
			observe("/Physics/Collisions", collide("Default", "Ball") && collide("Ball", "Track") && collide("Ball", "Trigger") && !collide("Default", "Default") && !collide("Track", "Trigger"));

			// The session's input actions, resolved against device states.
			const InputActionMap& actions = session->GetInput().GetActions();
			const std::optional<uint32_t> jump = actions.FindAction("Jump");
			const std::optional<uint32_t> moveX = actions.FindAction("MoveX");
			REQUIRE((jump.has_value() && moveX.has_value()));
			const InputState space = MakeInput({ KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Pressed } });
			const InputState south = MakeInput({ GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Button, .Button = GamepadButton::South, .Pressed = true } });
			const InputState right = MakeInput({ KeyEvent{ .KeyCode = Key::D, .Action = ButtonAction::Pressed } });
			const InputState left = MakeInput({ KeyEvent{ .KeyCode = Key::A, .Action = ButtonAction::Pressed } });
			const InputState stick = MakeInput({ GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Axis, .Axis = GamepadAxis::LeftX, .Value = 0.5f } });
			observe("/Input/Actions/*/Bindings", actions.IsDown(space, InputPhase::Step, *jump) && actions.IsDown(south, InputPhase::Step, *jump));
			observe("/Input/Actions/*/Positive", actions.GetAxis(right, InputPhase::Step, *moveX) == 1.0f);
			observe("/Input/Actions/*/Negative", actions.GetAxis(left, InputPhase::Step, *moveX) == -1.0f);
			// Only an axis action reads a negative value from its own bindings.
			observe("/Input/Actions/*/Type", actions.GetAxis(left, InputPhase::Step, *moveX) < 0.0f && actions.GetAxis(space, InputPhase::Step, *jump) >= 0.0f);
			// The bound stick through the dead zone, negated by Invert (the keys are never inverted).
			const float gamepadAxis = actions.GetAxis(stick, InputPhase::Step, *moveX);
			const float deadZoned = InputActionMap::ApplyDeadZone(0.5f);
			observe("/Input/Actions/*/Gamepad", deadZoned > 0.0f && std::abs(gamepadAxis) == doctest::Approx(deadZoned));
			observe("/Input/Actions/*/Invert", gamepadAxis < 0.0f && actions.GetAxis(right, InputPhase::Step, *moveX) > 0.0f);

			// The extraction of a frame carries the project's rendering quality; the VM's allocator the memory limit.
			session->Tick();
			observe("/Rendering/ShadowMapSize", session->GetLastExtraction().Quality.ShadowMapSize == settings.Rendering.ShadowMapSize);
			observe("/Rendering/SsaoHalfResolution", session->GetLastExtraction().Quality.SsaoHalfResolution == settings.Rendering.SsaoHalfResolution);
			REQUIRE(session->GetScripts() != nullptr);
			observe("/Scripting/MemoryLimitMB", session->GetScripts()->GetMemoryState().SoftLimitBytes == settings.Scripting.MemoryLimitMB * MiB);
			CallOrFail(client, "play.stop", Json::object());

			// Ordinary play enforces the callback budget, and with PauseOnError false keeps running after the fault.
			CallOrFail(client, "scene.open", Json{ { "path", settings.StartScene } });
			const std::string spinSource = "--!strict\nlocal Spin = {}\n\nfunction Spin.OnFixedUpdate(self: ScriptInstance, dt: number)\n"
										   "\twhile true do\n\tend\nend\n\nreturn Script.Define(\"Spin\", Spin)\n";
			CallOrFail(client, "script.write", Json{ { "path", "Assets/Scripts/Spin.luau" }, { "source", spinSource } });
			CallOrFail(client, "entity.create", Json{ { "name", "Spin" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Spin.luau" } } }) } } } } } });
			editor.GetAssets().WaitIdle();
			CallOrFail(client, "play.start", Json::object());
			const uint64_t cursor = editor.GetPlay().GetScriptErrors().GetCursor();
			{
				Test::ExpectLog timeout(LogLevel::Error, "exceeded");
				editor.GetPlay().OnFixedStep();
			}
			const std::vector<ScriptError> faults = editor.GetPlay().GetScriptErrors().Read(cursor);
			const bool budget = std::ranges::any_of(faults, [&settings](const ScriptError& error)
			{
				return error.Kind == ScriptErrorKind::Timeout && error.Message.contains(std::format("{} ms", settings.Scripting.CallbackBudgetMs));
			});
			observe("/Scripting/CallbackBudgetMs", budget);
			REQUIRE(editor.GetPlay().GetSession() != nullptr);
			observe("/Scripting/PauseOnError", !faults.empty() && !editor.GetPlay().GetSession()->IsPaused());
			CallOrFail(client, "play.stop", Json::object());

			// BlockPlayOnTypeErrors refuses Play while a script has a type error.
			CallOrFail(client, "script.write", Json{ { "path", "Assets/Scripts/Typo.luau" }, { "source", "--!strict\nlocal count: number = \"three\"\nreturn count\n" } });
			const Result<Json> refused = client.Call("play.start", Json::object());
			observe("/Scripting/BlockPlayOnTypeErrors", !refused.has_value() && refused.error().ToString().contains("script errors block Play"));
			CallOrFail(client, "script.write", Json{ { "path", "Assets/Scripts/Typo.luau" }, { "source", "--!strict\nlocal count: number = 3\nreturn count\n" } });

			// The exporter leaves out what Export.Exclude matches: a build scene under the second glob fails its validation.
			ApplyPatch(editor, R"({"Export":{"BuildScenes":["Assets/Scenes/Main.scene","Assets/Scenes/Level2.scene","Assets/Drafts/Sketch.scene"]}})");
			ExportSpecification specification;
			specification.BinaryRoot = project;
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			std::optional<Result<ExportReport>> exported;
			REQUIRE(Test::WaitUntil([&exporter, &exported]()
			{
				exported = (*exporter)->Poll();
				return exported.has_value();
			}, LongOperationWaitBound));
			REQUIRE_FALSE(exported->has_value());
			const bool excluded = std::ranges::any_of(exported->error().GetIssues(), [](const ErrorIssue& issue)
			{
				return issue.JsonPointer == "/Export/BuildScenes/2" && issue.Message.contains("Assets/Drafts/**");
			});
			observe("/Export/Exclude", excluded);

			// The validator checks StartScene and every build scene against the files.
			const auto clean = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE_MESSAGE(clean.has_value(), clean.error().ToString());
			CHECK(std::ranges::none_of(clean->Diagnostics, [](const ProjectDiagnostic& diagnostic)
			{
				return diagnostic.Code == BuildSceneMissingCode || diagnostic.Code == BuildStartSceneMissingCode;
			}));
			REQUIRE(FileSystem::Remove(project / "Assets" / "Scenes" / "Level2.scene").has_value());
			REQUIRE(FileSystem::Remove(project / "Assets" / "Scenes" / "Main.scene").has_value());
			const auto missing = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE_MESSAGE(missing.has_value(), missing.error().ToString());
			const auto reports = [&missing](std::string_view code, std::string_view scene)
			{
				return std::ranges::any_of(missing->Diagnostics, [code, scene](const ProjectDiagnostic& diagnostic)
				{
					return diagnostic.Code == code && diagnostic.Message.contains(scene);
				});
			};
			observe("/StartScene", reports(BuildStartSceneMissingCode, settings.StartScene));
			observe("/Export/BuildScenes", reports(BuildSceneMissingCode, "Assets/Scenes/Level2.scene"));

			// Every leaf of the reflected settings, compared with what the consumers showed.
			const StructInfo* type = registry->FindStruct<ProjectSettings>();
			REQUIRE(type != nullptr);
			std::set<std::string> leaves;
			CollectLeafPointers(*type, "", leaves);
			std::vector<std::string> unobserved;
			for (const std::string& leaf : leaves)
			{
				if (!observed.contains(leaf))
					unobserved.push_back(leaf);
			}
			std::vector<std::string> unknown;
			for (const std::string& pointer : observed)
			{
				if (!leaves.contains(pointer))
					unknown.push_back(pointer);
			}
			CHECK(unobserved == std::vector<std::string>{});
			CHECK(unknown == std::vector<std::string>{});
		}

		// Tests/Data/CoverageGaps/Gate8/AllSettings.eproj leaves exactly one field at its default; Expected.json names it. The
		// walk is gate 8's own (Support/DefaultFields.h), shared with the AllSettings.eproj test.
		TEST_CASE("ProjectSettings: the gate 8 seeded-gap fixture names exactly its default field")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ProjectSettings gap = LoadSettings(*registry, "CoverageGaps/Gate8/AllSettings.eproj");
			const StructInfo* type = registry->FindStruct<ProjectSettings>();
			REQUIRE(type != nullptr);
			const Result<Json> actual = type->ToJson(&gap);
			REQUIRE(actual.has_value());
			std::vector<std::string> uncovered;
			Test::CollectDefaultFields(*type, { &*actual }, type->MakeDefaultJson(), "", uncovered);

			const Result<std::string> expectedText = Test::ReadTestDataText("CoverageGaps/Gate8/Expected.json");
			REQUIRE_MESSAGE(expectedText.has_value(), expectedText.error().ToString());
			const auto expected = JsonReader::Parse(*expectedText);
			REQUIRE(expected.has_value());
			const JsonReader reader(*expected);
			REQUIRE(reader.ReadFormatHeader("CoverageGap", 1, 1).has_value());
			CHECK(reader.ReadMember<std::string>("Gate") == std::string("8"));
			const auto missing = reader.GetMember("Missing");
			REQUIRE(missing.has_value());
			REQUIRE(missing->GetArraySize() == static_cast<size_t>(1));
			const auto only = missing->GetElement(0);
			REQUIRE(only.has_value());
			REQUIRE(uncovered.size() == 1);
			CHECK(only->ReadString() == uncovered.front());

			// The gap is AllSettings.eproj with that one field at its default: setting it back restores the full fixture.
			const Result<std::string> full = Test::ReadTestDataText("Project/AllSettings.eproj");
			const Result<std::string> seeded = Test::ReadTestDataText("CoverageGaps/Gate8/AllSettings.eproj");
			REQUIRE((full.has_value() && seeded.has_value()));
			const auto fullJson = JsonReader::Parse(*full);
			auto seededJson = JsonReader::Parse(*seeded);
			REQUIRE((fullJson.has_value() && seededJson.has_value()));
			(*seededJson)["Rendering"]["SsaoHalfResolution"] = true;
			CHECK(*seededJson == *fullJson);
		}
	}

}
