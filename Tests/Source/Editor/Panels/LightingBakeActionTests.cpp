#include "TestsPCH.h"

#include "Editor/Panels/LightingBakeAction.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/ProbeCapture.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

// The editor's Bake Lighting action (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A):
// when the Tools menu's item is enabled. The click that queues lighting.bake is EditorLayerTests.cpp's.

namespace Engine {

	TEST_SUITE("Editor")
	{
		// Decision 8: the action is enabled exactly when lighting.bake can bake the active scene: an open, writable project
		// whose active edit scene is saved to a file, no play session and no test run, and an editor that renders (a
		// ProbeCapture injected at startup; none with --renderer none). Each condition alone disables it.
		TEST_CASE("LightingBakeAction: the action needs a writable project's saved scene, no play or test run, and a rendering editor"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("LightingBakeAction");
			EditorContext& editor = fixture.GetEditor();
			CHECK_FALSE(IsLightingBakeAvailable(editor)); // no project
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			// Every clock reading advances 100 ms, past the test run's frame budget, so one Pump advances a run by one runner
			// step and the run below is still pending after it however fast the build is.
			auto now = std::chrono::steady_clock::time_point{};
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(editor, std::move(specification));
			CHECK_FALSE(IsLightingBakeAvailable(editor)); // no renderer
			{
				GpuResourceCache cache(gpu.GetDevice(), editor.GetAssets());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<ProbeCapture>> capture = ProbeCapture::Create(gpu.GetDevice(), **pipelines, cache, editor.GetAssets());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				editor.SetProbeCapture(capture->get());
				CHECK(IsLightingBakeAvailable(editor));

				// An active scene never saved.
				const OpenSceneId saved = editor.GetActiveSceneId();
				REQUIRE(editor.AddScene(editor.CreateScene("Unsaved"), std::nullopt).has_value());
				CHECK_FALSE(IsLightingBakeAvailable(editor));
				REQUIRE(editor.ActivateScene(saved).has_value());
				CHECK(IsLightingBakeAvailable(editor));

				// A play session.
				REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }));
				CHECK_FALSE(IsLightingBakeAvailable(editor));
				REQUIRE(client.Call("play.stop", Json::object()));
				CHECK(IsLightingBakeAvailable(editor));

				// A test run, whose single case outlasts this check, owns the editor until its client leaves.
				const std::string longSuite = "return Test.Suite('Long', function() Test.Case('waits', function() Test.WaitTicks(100000) end) end)";
				REQUIRE(client.Call("script.write", Json{ { "path", "Assets/Tests/Long.test.luau" }, { "source", longSuite } }));
				const Json suite = Json{ { "Script", "Assets/Tests/Long.test.luau" }, { "Modes", Json::array({ "Editor" }) } };
				REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ suite }) } } } } } }));
				static_cast<void>(client.Submit("test.run", Json::object()));
				client.GetServer().Pump();
				REQUIRE(editor.GetPlay().IsTestRunActive());
				CHECK_FALSE(IsLightingBakeAvailable(editor));
				client.GetServer().DisconnectInProcess(client.GetClient());
				REQUIRE_FALSE(editor.GetPlay().IsTestRunActive());
				CHECK(IsLightingBakeAvailable(editor));

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
				CHECK_FALSE(IsLightingBakeAvailable(editor));

				editor.SetProbeCapture(nullptr);
				capture->reset();
				pipelines->reset();
				gpu.GetDevice().WaitForIdle();
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
