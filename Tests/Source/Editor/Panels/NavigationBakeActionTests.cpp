#include "TestsPCH.h"

#include "Editor/Panels/NavigationBakeAction.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

// The editor's Bake Navigation action (M30, Docs/Decisions/0045-m30-navigation.md decision 8): when the Tools menu's item is
// enabled. The click that queues navigation.bake is EditorLayerTests.cpp's.

namespace Engine {

	TEST_SUITE("Editor")
	{
		// The action is enabled exactly when navigation.bake can bake the active scene: an open, writable project whose active
		// edit scene is saved to a file, no play session and no test run. Each condition alone disables it; no renderer is
		// needed (the fixture's editor has none).
		TEST_CASE("NavigationBakeAction: the action needs a writable project's saved scene and no play or test run, and no renderer")
		{
			Test::EditorTestFixture fixture("NavigationBakeAction");
			EditorContext& editor = fixture.GetEditor();
			CHECK_FALSE(IsNavigationBakeAvailable(editor)); // no project
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			// Every clock reading advances 100 ms, past the test run's frame budget, so one Pump advances a run by one runner step
			// and the run below is still pending after it however fast the build is.
			auto now = std::chrono::steady_clock::time_point{};
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(editor, std::move(specification));
			REQUIRE(editor.GetProbeCapture() == nullptr);
			CHECK(IsNavigationBakeAvailable(editor));

			// An active scene never saved.
			const OpenSceneId saved = editor.GetActiveSceneId();
			REQUIRE(editor.AddScene(editor.CreateScene("Unsaved"), std::nullopt).has_value());
			CHECK_FALSE(IsNavigationBakeAvailable(editor));
			REQUIRE(editor.ActivateScene(saved).has_value());
			CHECK(IsNavigationBakeAvailable(editor));

			// A play session.
			REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }));
			CHECK_FALSE(IsNavigationBakeAvailable(editor));
			REQUIRE(client.Call("play.stop", Json::object()));
			CHECK(IsNavigationBakeAvailable(editor));

			// A test run, whose single case outlasts this check, owns the editor until its client leaves.
			const std::string longSuite = "return Test.Suite('Long', function() Test.Case('waits', function() Test.WaitTicks(100000) end) end)";
			REQUIRE(client.Call("script.write", Json{ { "path", "Assets/Tests/Long.test.luau" }, { "source", longSuite } }));
			const Json suite = Json{ { "Script", "Assets/Tests/Long.test.luau" }, { "Modes", Json::array({ "Editor" }) } };
			REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ suite }) } } } } } }));
			static_cast<void>(client.Submit("test.run", Json::object()));
			client.GetServer().Pump();
			REQUIRE(editor.GetPlay().IsTestRunActive());
			CHECK_FALSE(IsNavigationBakeAvailable(editor));
			client.GetServer().DisconnectInProcess(client.GetClient());
			REQUIRE_FALSE(editor.GetPlay().IsTestRunActive());
			CHECK(IsNavigationBakeAvailable(editor));

			// The same project and saved scene opened read-only.
			const std::filesystem::path projectFile = editor.GetProject().GetProjectFile();
			const VfsPath scenePath = *editor.GetScenePath();
			const Result<std::string> sceneText = FileSystem::ReadText(projectFile.parent_path() / "Assets" / "Scenes" / "Main.scene");
			REQUIRE(sceneText.has_value());
			REQUIRE(editor.CloseProject().has_value());
			Result<Scope<LoadedProject>> readOnly = ProjectManager::OpenProject(projectFile,
				{ .ReadOnly = true, .ReadOnlyCacheDirectory = fixture.GetDirectory() / "ReadOnlyCache" }, editor.GetTypeRegistry());
			REQUIRE_MESSAGE(readOnly.has_value(), readOnly.error().ToString());
			REQUIRE(editor.OpenProject(std::move(*readOnly)).has_value());
			Scope<Scene> scene = editor.CreateScene("Main");
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(*scene, *sceneText, {}, report).has_value());
			editor.SetScene(std::move(scene), scenePath);
			REQUIRE(editor.IsReadOnly());
			CHECK_FALSE(IsNavigationBakeAvailable(editor));
		}
	}

}
