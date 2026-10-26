#include "TestsPCH.h"

#include "Editor/Private/EditorHostViewports.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"
#include "Support/DeviceLossTest.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TrimCounter.h"
#include "Support/WaitUntil.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// The host's GPU services over a headless device: its pipelines, the ImGui texture registry and one command list,
		// with frame-by-frame recording and submission like EditorApp's frame loop. Destroyed before the fixture's checks.
		class HostGpuHarness
		{
		public:
			HostGpuHarness(Test::HeadlessGpuFixture& gpu, EditorContext& editor)
				: m_Device(gpu.GetDevice()), m_Cache(m_Device, editor.GetAssets()), m_Gizmos(editor), m_Host(editor, m_Gizmos)
			{
				ImGui::GetIO().IniFilename = nullptr;
				ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1);
				auto pipelines = SceneRendererPipelines::Create(m_Device, gpu.GetPipelines());
				REQUIRE(pipelines.has_value());
				m_Pipelines = std::move(*pipelines);
				auto renderer = ImGuiRenderer::Create(m_Device, gpu.GetPipelines(), {});
				REQUIRE(renderer.has_value());
				m_Renderer = std::move(*renderer);
				auto command = m_Device.CreateCommandList();
				REQUIRE(command.has_value());
				m_Command = std::move(*command);
				REQUIRE(m_Host.Initialize(m_Device, *m_Pipelines, m_Cache, *m_Renderer).has_value());
			}
			~HostGpuHarness()
			{
				m_Device.WaitForIdle();
				m_Command = nullptr;
				m_Host.Retire();
				m_Renderer->DestroyTextures();
			}
			HostGpuHarness(const HostGpuHarness&) = delete;
			HostGpuHarness& operator=(const HostGpuHarness&) = delete;

			// One frame: records both views, submits them and notifies the host. Returns Render's status. `beforeSubmission`,
			// when set, runs after the recording and before the submission.
			Status Frame(uint64_t frame, std::optional<EditorViewportClick> click = {}, const std::function<void()>& beforeSubmission = {})
			{
				m_Device.WaitForIdle();
				m_Renderer->BeginFrame(static_cast<uint32_t>(frame % 2));
				m_Command->open();
				RenderContext context{ .Device = &m_Device, .CommandList = m_Command, .FrameIndex = frame };
				const Status rendered = m_Host.Render(context);
				if (click)
					REQUIRE(m_Host.RequestPick(*click).has_value());
				m_Command->close();
				if (beforeSubmission)
					beforeSubmission();
				const uint64_t submission = m_Device.ExecuteCommandList(*m_Command);
				m_Submitted = m_Host.Submitted(frame, submission);
				return rendered;
			}

			[[nodiscard]] EditorHostViewports& GetHost() { return m_Host; }
			// The views' shared pipeline set (a shader reload calls its CreatePipelines).
			[[nodiscard]] SceneRendererPipelines& GetPipelines() { return *m_Pipelines; }
			// The status of the last frame's Submitted notification.
			[[nodiscard]] const Status& GetSubmitted() const { return m_Submitted; }
		private:
			struct ImGuiScope
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~ImGuiScope() { ImGui::DestroyContext(Context); }
			};
			// Declared in construction order; the host goes first at destruction, after the destructor retired it.
			ImGuiScope m_Ui{};
			GraphicsDevice& m_Device;
			GpuResourceCache m_Cache;
			Scope<SceneRendererPipelines> m_Pipelines{};
			Scope<ImGuiRenderer> m_Renderer{};
			nvrhi::CommandListHandle m_Command{};
			GizmoController m_Gizmos;
			EditorHostViewports m_Host;
			Status m_Submitted{};
		};

		Json EvaluatePlay(Test::AutomationTestClient& client, std::string_view code)
		{
			auto result = client.Call("script.eval", Json{ { "context", "play" }, { "code", code } });
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			return (*result)["value"];
		}

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostViewports: captured game input reaches the session in the letterboxed game image's pixels")
		{
			Test::EditorTestFixture fixture("HostGameCoordinates");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Test::AutomationTestClient client(editor);
			REQUIRE(client.Call("play.start", Json{ { "paused", true } }).has_value());
			auto& play = editor.GetPlay();
			PlaySession& session = *play.GetSession();
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			// A fixed 1920x1080 game image letterboxed in the Game panel at (400, 80), 800x450 logical units; the editor
			// keeps the session's view at the image's size.
			REQUIRE(editor.GetViewportState().SetGameResolution({ 1920, 1080 }).has_value());
			host.SetRectangle(ViewportView::Game, { .Min = { 400.0f, 80.0f }, .Size = { 800.0f, 450.0f }, .ImageSize = { 1920, 1080 } });
			session.SetViewSize(1920, 1080);
			const auto forward = [&host, &play](const Event& event)
			{
				const std::optional<Event> game = host.ToGameEvent(event);
				if (game)
					play.OnInputEvent(*game, true);
				return game;
			};
			// Until the view captures input, device events are not game input.
			CHECK_FALSE(forward(MouseMoveEvent{ .Position = { 800.0f, 305.0f } }).has_value());
			CHECK_FALSE(forward(KeyEvent{ .KeyCode = Key::A }).has_value());
			host.SetGameInputFocused(true);
			const auto corner = forward(MouseMoveEvent{ .Position = { 400.0f, 80.0f } });
			REQUIRE(corner.has_value());
			CHECK(std::get<MouseMoveEvent>(*corner).Position == glm::vec2(0.0f, 0.0f));
			const auto center = forward(MouseMoveEvent{ .Position = { 800.0f, 305.0f } });
			REQUIRE(center.has_value());
			CHECK(std::get<MouseMoveEvent>(*center).Position == glm::vec2(960.0f, 540.0f));
			REQUIRE(forward(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Pressed }).has_value());
			session.Tick();
			CHECK(EvaluatePlay(client, "return Input.GetMousePosition() == vector.create(960, 540, 0)") == Json(true));
			CHECK(EvaluatePlay(client, "return Input.IsMouseButtonDown('Left')") == Json(true));
			CHECK(EvaluatePlay(client, "return Application.GetWindowSize() == vector.create(1920, 1080, 0)") == Json(true));
			// The cursor over the image's centre casts the camera's forward ray: mouse, view size and projection agree.
			CHECK(EvaluatePlay(client, R"(
local mouse = Input.GetMousePosition()
local origin, direction = Scene.FindByName('Camera'):GetComponent('Camera'):ScreenToWorldRay(mouse.x, mouse.y)
return vector.magnitude(direction - vector.create(0, 0, -1)) < 0.001)")
				== Json(true));
			// A press in the letterbox bar is no press on the game; its release still passes.
			REQUIRE(forward(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Released }).has_value());
			const auto bar = forward(MouseMoveEvent{ .Position = { 380.0f, 100.0f } });
			REQUIRE(bar.has_value());
			CHECK(std::get<MouseMoveEvent>(*bar).Position.x < 0.0f); // unclamped, so motion stays continuous
			CHECK_FALSE(forward(MouseButtonEvent{ .Button = MouseButton::Right, .Action = ButtonAction::Pressed }).has_value());
			CHECK(forward(MouseButtonEvent{ .Button = MouseButton::Right, .Action = ButtonAction::Released }).has_value());
			session.Tick();
			CHECK(EvaluatePlay(client, "return Input.IsMouseButtonDown('Right') or Input.IsMouseButtonDown('Left')") == Json(false));
			host.SetGameInputFocused(false);
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
		}

		TEST_CASE("EditorHostViewports: losing game input releases native input of ordinary play but never touches a lockstep session")
		{
			Test::EditorTestFixture fixture("HostGameRelease");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			SUBCASE("lockstep: only the owner's injected input exists, and it survives a UI focus change")
			{
				REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
				PlaySession& session = *editor.GetPlay().GetSession();
				REQUIRE(session.GetInput().Queue(session.GetTick(), { .KeyCode = Key::A }).has_value());
				REQUIRE(client.Call("play.step", Json{ { "ticks", 1 } }).has_value());
				REQUIRE(session.GetInput().GetSummary(InputPhase::Step).Down == std::vector<std::string>{ "Key.A" });
				host.SetGameInputFocused(true);
				host.SetGameInputFocused(false);
				CHECK(session.GetInput().GetQueuedEventCount() == 0);
				REQUIRE(client.Call("play.step", Json{ { "ticks", 2 } }).has_value());
				CHECK(session.GetInput().GetSummary(InputPhase::Step).Down == std::vector<std::string>{ "Key.A" });
				CHECK(session.GetInput().GetSummary(InputPhase::Step).Released.empty());
			}
			SUBCASE("ordinary play: held native input is released with the capture")
			{
				REQUIRE(client.Call("play.start", Json{ { "paused", true } }).has_value());
				PlaySession& session = *editor.GetPlay().GetSession();
				host.SetGameInputFocused(true);
				const auto key = host.ToGameEvent(KeyEvent{ .KeyCode = Key::A });
				REQUIRE(key.has_value());
				editor.GetPlay().OnInputEvent(*key, true);
				session.Tick();
				CHECK(session.GetInput().GetSummary(InputPhase::Step).Down == std::vector<std::string>{ "Key.A" });
				host.SetGameInputFocused(false);
				session.Tick();
				CHECK(session.GetInput().GetSummary(InputPhase::Step).Down.empty());
				CHECK(session.GetInput().GetSummary(InputPhase::Step).Released == std::vector<std::string>{ "Key.A" });
			}
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
		}

		TEST_CASE("EditorHostViewports: scene and game extraction carry the project's rendering quality")
		{
			Test::EditorTestFixture fixture("HostQuality");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Test::AutomationTestClient client(editor);
			REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "Rendering", Json{ { "ShadowMapSize", 512 }, { "SsaoHalfResolution", true } } } } } }).has_value());
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			for (const bool playing : { false, true })
			{
				CAPTURE(playing);
				if (playing)
					REQUIRE(client.Call("play.start", Json{ { "paused", true } }).has_value());
				for (const ViewportView view : { ViewportView::Scene, ViewportView::Game })
				{
					const auto snapshot = host.Extract(view, 160, 90);
					REQUIRE(snapshot.has_value());
					CHECK(snapshot->Quality.ShadowMapSize == 512);
					CHECK(snapshot->Quality.SsaoHalfResolution);
				}
			}
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
		}

		TEST_CASE("EditorHostViewports: frames forced while minimized render the images without publishing their extents" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostMinimized");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			REQUIRE(harness.Frame(0).has_value());
			REQUIRE(editor.GetViewportState().GetPixelSize(ViewportView::Scene).has_value());
			const uint64_t withdrawals = harness.GetHost().GetTextureWithdrawalCount();
			harness.GetHost().SetUnavailable();
			// Minimizing withdrew both registered images: a UI frame built before it is no longer capturable.
			CHECK(harness.GetHost().GetTextureWithdrawalCount() == withdrawals + 2);
			CHECK_FALSE(editor.GetViewportState().GetPixelSize(ViewportView::Scene).has_value());
			// editor.screenshot's offscreen frame while still minimized: the UI capture gets images, the views stay unavailable.
			REQUIRE(harness.Frame(1).has_value());
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).Texture != 0);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Texture != 0);
			CHECK_FALSE(editor.GetViewportState().GetPixelSize(ViewportView::Scene).has_value());
			CHECK_FALSE(editor.GetViewportState().GetPixelSize(ViewportView::Game).has_value());
			harness.GetHost().SetAvailable();
			REQUIRE(harness.Frame(2).has_value());
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Scene).value() == glm::uvec2(96, 64));
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Game).value() == glm::uvec2(80, 80));
		}

		TEST_CASE("EditorHostViewports: a failing scene view still renders and notifies the game view" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostPartialFailure");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			REQUIRE(harness.Frame(0).has_value());
			// A mesh with a non-finite world matrix is a partial render error of every view drawing it (SceneRenderer::Render:
			// the draw is skipped and the view still recorded). A light the shadow planners reject is not an error (M9-F03).
			Entity broken = editor.GetScene().CreateEntity("Broken");
			broken.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			broken.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x = std::numeric_limits<float>::quiet_NaN();
			});
			const Status rendered = harness.Frame(1);
			REQUIRE_FALSE(rendered.has_value());
			CHECK(rendered.error().GetCode() == ErrorCode::InvalidArgument);
			// The scene view's failure no longer stops the game view: both images belong to this frame and were notified.
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).FrameIndex == 1);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).FrameIndex == 1);
			CHECK(harness.GetSubmitted().has_value());
			const auto statistics = harness.GetHost().GetRenderer(ViewportView::Game)->GetRenderStats();
			CHECK_FALSE(statistics.Passes.empty());
		}

		TEST_CASE("EditorHostViewports: an image without a picking target refuses picks before submission" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostNoPickingTarget");
			fixture.CreateAndOpenProject(); // a project without an open scene, as after the launcher opens it
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			REQUIRE(harness.Frame(0).has_value());
			for (const ViewportView view : { ViewportView::Scene, ViewportView::Game })
			{
				CAPTURE(static_cast<int>(view));
				const EditorViewportImage image = harness.GetHost().GetImage(view);
				REQUIRE(image.Texture != 0); // the empty image keeps the layout ready for editor.screenshot
				CHECK_FALSE(image.Picking);
				const Status refused = harness.GetHost().RequestPick({ .View = view, .Pixel = { 4, 4 }, .FrameIndex = image.FrameIndex, .SceneRevision = image.SceneRevision, .Sequence = 1, .ViewGeneration = image.Generation });
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Unsupported);
			}
			REQUIRE(harness.Frame(1).has_value());
			CHECK(harness.GetSubmitted().has_value());
		}

		TEST_CASE("EditorHostViewports: a hidden view releases its renderer when the shown scene changes" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostHiddenRelease");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			REQUIRE(harness.Frame(0).has_value());
			REQUIRE(harness.Frame(1).has_value());
			// Hiding alone keeps the renderer: switching tabs must not rebuild it.
			harness.GetHost().SetRectangle(ViewportView::Game, {});
			REQUIRE(harness.Frame(2).has_value());
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Texture == 0);
			CHECK(harness.GetHost().GetRenderer(ViewportView::Game) != nullptr);
			// A scene swap while hidden releases it, with the binding caches that held the previous scene's textures.
			editor.SetScene(editor.CreateScene("Replacement"), std::nullopt);
			REQUIRE(harness.Frame(3).has_value());
			CHECK(harness.GetHost().GetRenderer(ViewportView::Game) == nullptr);
			CHECK(harness.GetHost().GetRenderer(ViewportView::Scene) != nullptr);
			// Displayed again, the view gets a new renderer and image.
			harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			REQUIRE(harness.Frame(4).has_value());
			REQUIRE(harness.GetHost().GetRenderer(ViewportView::Game) != nullptr);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Texture != 0);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).FrameIndex == 4);
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Game).value() == glm::uvec2(80, 80));
		}

		TEST_CASE("EditorHostViewports: ScriptedClock suites reject physical input and focus releases while retaining Test injection")
		{
			std::vector<uint64_t> baseline;
			for (const bool noise : std::array{ false, true })
			{
				INFO(noise);
				Test::EditorTestFixture fixture("ScriptedClockInput");
				fixture.CreateAndOpenProject();
				fixture.CreateAndOpenScene();
				auto& editor = fixture.GetEditor();
				auto& play = editor.GetPlay();
				GizmoController gizmos(editor);
				EditorHostViewports host(editor, gizmos);
				auto now = std::chrono::steady_clock::time_point{};
				auto specification = Test::MakeTestServerSpecification();
				specification.WallClock = [&now]()
				{
					now += std::chrono::milliseconds(100);
					return now;
				};
				Test::AutomationTestClient client(editor, std::move(specification), false);
				const auto call = [&client](std::string_view method, const Json& params)
				{
					auto result = client.Call(method, params);
					REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
					return std::move(*result);
				};
				call("script.write", Json{ { "path", "Assets/Tests/Input.test.luau" }, { "source", R"(
return Test.Suite("InputOwnership", function()
    Test.Case("injected input survives viewport changes", function()
        Test.InjectKey("A", "Down")
        Test.WaitTicks(8)
        Test.Expect(Input.IsKeyDown("A"), "viewport focus released test-owned input")
        Test.Expect(not Input.IsKeyDown("Space"), "physical input reached the suite")
    end)
end)
)" } });
				call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Input.test.luau" }, { "Clock", Json::array({ 0.02 }) }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } });
				static_cast<void>(client.Submit("test.run", Json::object()));
				std::vector<uint64_t> hashes;
				std::vector<Json> responses;
				bool observedInjected = false;
				REQUIRE(Test::WaitUntil([&client, &play, &host, &hashes, &responses, &observedInjected, noise]()
				{
					client.GetServer().Pump();
					if (PlaySession* session = play.GetSession())
					{
						REQUIRE(play.IsTestRunActive());
						REQUIRE_FALSE(session->IsLockstep());
						hashes.push_back(session->ComputeStateHash());
						const auto input = session->GetInput().GetSummary(InputPhase::Step);
						observedInjected = observedInjected || std::ranges::find(input.Down, "Key.A") != input.Down.end();
						if (noise)
						{
							host.SetGameInputFocused(true);
							const size_t queued = session->GetInput().GetQueuedEventCount();
							play.OnInputEvent(KeyEvent{ .KeyCode = Key::Space }, host.IsGameInputFocused());
							host.SetGameInputFocused(false);
							CHECK(session->GetInput().GetQueuedEventCount() == queued);
						}
						play.OnSafePoint();
						CHECK(play.GetSession() == session);
					}
					responses = client.GetServer().TakeInProcessResponses(client.GetClient());
					return !responses.empty();
				}));
				REQUIRE(responses.size() == 1);
				CHECK(responses.front()["result"]["passed"] == Json(true));
				CHECK(observedInjected);
				REQUIRE_FALSE(hashes.empty());
				if (!noise)
					baseline = std::move(hashes);
				else
					CHECK(hashes == baseline);
				// The same native ingress remains functional for ordinary Play.
				call("play.start", Json{ { "paused", true } });
				host.SetGameInputFocused(true);
				play.OnInputEvent(KeyEvent{ .KeyCode = Key::Space }, host.IsGameInputFocused());
				play.GetSession()->Tick();
				CHECK(call("script.eval", Json{ { "context", "play" }, { "code", "Input.IsKeyDown('Space')" } })["value"] == Json(true));
				host.SetGameInputFocused(false);
				play.GetSession()->Tick();
				CHECK(call("script.eval", Json{ { "context", "play" }, { "code", "Input.IsKeyDown('Space')" } })["value"] == Json(false));
				call("play.stop", Json::object());
			}
		}

		TEST_CASE("EditorHostViewports: displayed images own independent extents and delayed picks cancel on hiding" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			struct UiContext
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~UiContext() { ImGui::DestroyContext(Context); }
			} ui;
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1);
			Test::EditorTestFixture fixture("HostGpuViews");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("PickMe");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			GraphicsDevice& device = gpu.GetDevice();
			GpuResourceCache cache(device, editor.GetAssets());
			auto pipelines = SceneRendererPipelines::Create(device, gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), {});
			REQUIRE(renderer.has_value());
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			struct WaitForHostGpu
			{
				GraphicsDevice& Device;
				~WaitForHostGpu() { Device.WaitForIdle(); }
			} idle{ device };
			REQUIRE(host.Initialize(device, **pipelines, cache, **renderer).has_value());
			host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			host.SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			auto command = device.CreateCommandList();
			REQUIRE(command.has_value());
			const auto render = [&device, &renderer, &command, &host](uint64_t frame, uint64_t click)
			{
				device.WaitForIdle();
				(*renderer)->BeginFrame(static_cast<uint32_t>(frame % 2));
				(*command)->open();
				RenderContext context{ .Device = &device, .CommandList = *command, .FrameIndex = frame };
				REQUIRE(host.Render(context).has_value());
				if (click != 0)
				{
					const auto image = host.GetImage(ViewportView::Scene);
					REQUIRE(host.RequestPick({ .Pixel = { image.Width / 2, image.Height / 2 }, .FrameIndex = image.FrameIndex, .SceneRevision = image.SceneRevision, .Sequence = click, .ViewGeneration = image.Generation }).has_value());
				}
				(*command)->close();
				const uint64_t submission = device.ExecuteCommandList(**command);
				REQUIRE(host.Submitted(frame, submission).has_value());
			};
			render(0, 1);
			CHECK(editor.GetSelection().empty());
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Scene).value() == glm::uvec2(96, 64));
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Game).value() == glm::uvec2(80, 80));
			CHECK(host.GetImage(ViewportView::Scene).Texture != host.GetImage(ViewportView::Game).Texture);
			render(1, 0);
			CHECK(editor.GetSelection().empty());
			render(2, 0);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == mesh.GetUUID());
			editor.SetSelection({});
			render(3, 2);
			host.SetRectangle(ViewportView::Scene, {});
			render(4, 0);
			render(5, 0);
			CHECK(editor.GetSelection().empty());
			CHECK_FALSE(editor.GetViewportState().GetPixelSize(ViewportView::Scene).has_value());
			CHECK(editor.GetViewportState().GetSceneSize() == glm::uvec2(96, 64));
			device.WaitForIdle();
			host.Retire();
			(*renderer)->DestroyTextures();
		}

		TEST_CASE("EditorHostViewports: a click queued in the frame that lost the device is dropped without a failure"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 28): a loss that strikes the frame's submission still
			// notifies the host (Application::OnFrameRender). The click queued for that frame is dropped instead of asking
			// for a pick, whose staging texture, command list and query a lost device cannot create: that refusal is the
			// loss, which EditorApp would otherwise log at Error as a failed pick. The recovery then renders on.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			struct UiContext
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~UiContext() { ImGui::DestroyContext(Context); }
			} ui;
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1);
			Test::EditorTestFixture fixture("HostLostClick");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("PickMe");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			GraphicsDevice& device = gpu.GetDevice();
			GpuResourceCache cache(device, editor.GetAssets());
			auto pipelines = SceneRendererPipelines::Create(device, gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), {});
			REQUIRE(renderer.has_value());
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			struct WaitForHostGpu
			{
				GraphicsDevice& Device;
				~WaitForHostGpu() { Device.WaitForIdle(); }
			} idle{ device };
			REQUIRE(host.Initialize(device, **pipelines, cache, **renderer).has_value());
			host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			host.SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			// One frame in a command list of its own (a list kept across the loss would be an object no owner releases),
			// with a click on the scene view's centre when `click` is not 0, the device lost before the submission when
			// `loseDevice` is set; returns the host's Submitted status.
			const auto frame = [&device, &renderer, &host](uint64_t index, uint64_t click, bool loseDevice) -> Status
			{
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
				device.WaitForIdle();
				(*renderer)->BeginFrame(static_cast<uint32_t>(index % 2));
				(*list)->open();
				RenderContext context{ .Device = &device, .CommandList = *list, .FrameIndex = index };
				REQUIRE(host.Render(context).has_value());
				if (click != 0)
				{
					const auto image = host.GetImage(ViewportView::Scene);
					REQUIRE(host.RequestPick({ .Pixel = { image.Width / 2, image.Height / 2 }, .FrameIndex = image.FrameIndex, .SceneRevision = image.SceneRevision, .Sequence = click, .ViewGeneration = image.Generation }).has_value());
				}
				(*list)->close();
				if (loseDevice)
				{
					device.GetDiagnostics().SetDeviceLost(true);
					device.ReportDeviceLost("EditorHostViewportsTests");
				}
				return host.Submitted(index, device.ExecuteCommandList(**list));
			};
			REQUIRE(frame(0, 0, false).has_value());
			// The first click of the scene view: its picker slot has no staging texture yet.
			const Status submitted = frame(1, 1, true);
			CHECK_MESSAGE(submitted.has_value(), (submitted.has_value() ? std::string() : submitted.error().ToString()));

			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), (recovered.has_value() ? std::string() : recovered.error().ToString()));
			for (uint64_t index = 2; index < 5; ++index)
				REQUIRE(frame(index, 0, false).has_value());
			CHECK(editor.GetSelection().empty());
			device.WaitForIdle();
			host.Retire();
			(*renderer)->DestroyTextures();
		}

		TEST_CASE("EditorHostViewports: scene and game extraction keep independent extents cameras and overlay flags")
		{
			Test::EditorTestFixture fixture("HostExtraction");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			const Entity camera = editor.GetScene().CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(editor.SetSelection({ mesh.GetUUID() }, SceneTarget::Edit).has_value());
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			const auto scene = host.Extract(ViewportView::Scene, 800, 400);
			const auto game = host.Extract(ViewportView::Game, 320, 240);
			REQUIRE(scene.has_value());
			REQUIRE(game.has_value());
			CHECK(scene->Camera.ViewportWidth == 800);
			CHECK(scene->Camera.ViewportHeight == 400);
			CHECK(game->Camera.ViewportWidth == 320);
			CHECK(game->Camera.ViewportHeight == 240);
			CHECK(scene->Camera.View != game->Camera.View);
			CHECK(HasFlag(scene->Flags, RenderViewFlags::EditorOverlays));
			CHECK(HasFlag(scene->Flags, RenderViewFlags::Selection));
			CHECK(HasFlag(scene->Flags, RenderViewFlags::Picking));
			// Only the scene view picks; the game view renders like the game, without an EntityId target (ADR 0017 §2).
			CHECK(game->Flags == RenderViewFlags::None);
			CHECK(scene->SceneRevision == editor.GetScene().GetRevision());
			REQUIRE(game->Meshes.size() == 1);
			CHECK(game->Meshes.front().PickId != 0);
		}

		TEST_CASE("EditorHostViewports: scene options map to renderer flags without changing camera quality or annotations")
		{
			Test::EditorTestFixture fixture("HostViewport");
			GizmoController gizmos(fixture.GetEditor());
			EditorHostViewports host(fixture.GetEditor(), gizmos);
			RenderSnapshot snapshot;
			snapshot.Quality.ShadowMapSize = 512;
			snapshot.Annotations.Axes = true;
			const std::array selected{ UUID(0x1234) };
			REQUIRE(host.ConfigureSceneSnapshot(snapshot, { .Grid = false, .Colliders = true, .Icons = false, .Wireframe = true }, selected).has_value());
			CHECK(HasFlag(snapshot.Flags, RenderViewFlags::EditorOverlays));
			CHECK(HasFlag(snapshot.Flags, RenderViewFlags::Picking));
			CHECK(HasFlag(snapshot.Flags, RenderViewFlags::Selection));
			CHECK(HasFlag(snapshot.Flags, RenderViewFlags::Colliders));
			CHECK(HasFlag(snapshot.Flags, RenderViewFlags::Wireframe));
			CHECK_FALSE(HasFlag(snapshot.Flags, RenderViewFlags::Grid));
			CHECK_FALSE(HasFlag(snapshot.Flags, RenderViewFlags::Icons));
			CHECK(snapshot.Quality.ShadowMapSize == 512);
			CHECK(snapshot.Annotations.Axes);
			CHECK(snapshot.SelectedEntities == std::vector<UUID>(selected.begin(), selected.end()));
		}
		TEST_CASE("EditorHostViewports: no device refuses picking and unavailable images preserve the camera aspect")
		{
			Test::EditorTestFixture fixture("HostNoDevice");
			auto& editor = fixture.GetEditor();
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			const auto picked = host.RequestPick({});
			REQUIRE_FALSE(picked.has_value());
			CHECK(picked.error().GetCode() == ErrorCode::Unsupported);
			REQUIRE(editor.GetViewportState().SetPixelSize(ViewportView::Scene, glm::uvec2(800, 600)).has_value());
			host.SetUnavailable();
			CHECK_FALSE(editor.GetViewportState().GetPixelSize(ViewportView::Scene).has_value());
			CHECK_FALSE(editor.GetViewportState().GetPixelSize(ViewportView::Game).has_value());
			CHECK(editor.GetViewportState().GetSceneSize() == glm::uvec2(800, 600));
		}

		TEST_CASE("EditorHostViewports: a device loss registers the views' recreated images and renders the same images"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 32 and 33; it recovers through the shared loss test and
			// the SceneRendererPipelines and ImGuiRenderer owners). The host's release removes both views' ImGui keys
			// (withdrawals, so no UI frame built before the loss is captured) and forgets their images; each view's renderer
			// recovers itself, and the first frame after the recovery registers the recreated images and renders what the
			// views rendered before.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			struct UiContext
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~UiContext() { ImGui::DestroyContext(Context); }
			} ui;
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1);
			Test::EditorTestFixture fixture("HostDeviceLoss");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			GraphicsDevice& device = gpu.GetDevice();
			GpuResourceCache cache(device, editor.GetAssets());
			auto pipelines = SceneRendererPipelines::Create(device, gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), {});
			REQUIRE(renderer.has_value());
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			struct WaitForHostGpu
			{
				GraphicsDevice& Device;
				~WaitForHostGpu() { Device.WaitForIdle(); }
			} idle{ device };
			// The views keep the size their renderers were created with (640 x 360, no SetRectangle), so no target of an earlier
			// size waits in a renderer's pool for its release when the live counts are read before the loss.
			REQUIRE(host.Initialize(device, **pipelines, cache, **renderer).has_value());

			// One frame of both views in a command list of its own (a list kept across the loss would be an object no owner
			// releases), as EditorApp's frame records them, then both final images read back. Every error is returned, so it
			// also runs on the lost device.
			uint64_t frame = 0;
			Test::DeviceLossTest test;
			test.Owners = { "EditorHostViewports" };
			test.Use = [&device, &renderer, &host, &frame]() -> Result<std::vector<std::byte>>
			{
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
				device.WaitForIdle();
				(*renderer)->BeginFrame(static_cast<uint32_t>(frame % 2));
				list->open();
				RenderContext context{ .Device = &device, .CommandList = list, .FrameIndex = frame };
				const Status rendered = host.Render(context);
				list->close();
				ENGINE_TRY(host.Submitted(frame, device.ExecuteCommandList(*list)));
				++frame;
				ENGINE_TRY(rendered);
				device.WaitForIdle();
				std::vector<std::byte> pixels;
				Readback readback(device);
				for (const ViewportView view : { ViewportView::Scene, ViewportView::Game })
				{
					const SceneRenderer* viewRenderer = host.GetRenderer(view);
					if (viewRenderer == nullptr || viewRenderer->GetFinalTexture() == nullptr || host.GetImage(view).Texture == 0)
						return MakeError(ErrorCode::InvalidState, "a view has no registered image after its frame");
					ENGINE_TRY_ASSIGN(const Image image, readback.ReadTexture(*viewRenderer->GetFinalTexture()));
					pixels.insert(pixels.end(), image.Pixels.begin(), image.Pixels.end());
				}
				return pixels;
			};
			const uint64_t withdrawals = host.GetTextureWithdrawalCount();
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), (report.has_value() ? std::string() : report.error().ToString()));
			CHECK(host.GetTextureWithdrawalCount() == withdrawals + 2);
			device.WaitForIdle();
			host.Retire();
			(*renderer)->DestroyTextures();
		}

		TEST_CASE("EditorHostViewports: a view whose new size runs out of memory stays at its previous size until its size changes"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42 and 44): the scene view's new size runs out of memory
			// after its renderer's trim and retry, so the view goes on rendering at its previous size, which its image and its
			// published extent report (the panel shows the image scaled and maps clicks through it), the layout counts as ready,
			// and the failed size is not asked for again, without another trim, until the requested size changes.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomRenderTargetPersistent });
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostOutOfMemorySize");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			REQUIRE(harness.Frame(0).has_value());
			gpu.GetDevice().RunGarbageCollection(); // the first frame's collection arms the fault
			const Test::TrimCounter counter(gpu.GetDevice().GetRecreation());

			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(128, 72) });
			REQUIRE(harness.Frame(1).has_value());
			CHECK(counter.GetTrims() == 1);
			const EditorViewportImage image = harness.GetHost().GetImage(ViewportView::Scene);
			CHECK(image.Texture != 0);
			CHECK(image.FrameIndex == 1);
			CHECK(image.Width == 96);
			CHECK(image.Height == 64);
			CHECK(harness.GetHost().GetRenderer(ViewportView::Scene)->GetWidth() == 96);
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Scene).value() == glm::uvec2(96, 64));
			CHECK(harness.GetHost().IsLayoutReady());

			// The size that failed is not tried again.
			REQUIRE(harness.Frame(2).has_value());
			CHECK(counter.GetTrims() == 1);
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).Width == 96);
			// Another size is a new request.
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(120, 70) });
			REQUIRE(harness.Frame(3).has_value());
			CHECK(counter.GetTrims() == 2);
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).Width == 96);
			CHECK_FALSE(gpu.GetDevice().IsLost());
		}

		TEST_CASE("EditorHostViewports: a debug-view switch that runs out of memory sets the setting back to the mode that rendered"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decisions 42 and 44): the Albedo view's pipelines run out of memory again after their trim, so the scene
			// view renders in the mode it last rendered (Lit) and the host sets the scene view's debug-view setting back to it,
			// so the next frames do not try again; without the fault the switch stays.
			for (const GpuFault fault : { GpuFault::None, GpuFault::OomPipelinePersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				Test::EditorTestFixture fixture("HostOutOfMemoryDebugView");
				fixture.CreateAndOpenProject();
				fixture.CreateAndOpenScene();
				auto& editor = fixture.GetEditor();
				editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
				REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
				REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
				HostGpuHarness harness(gpu, editor);
				harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
				REQUIRE(harness.Frame(0).has_value());
				gpu.GetDevice().RunGarbageCollection();

				REQUIRE(editor.GetViewportState().SetDebugView(RenderDebugView::Albedo).has_value());
				REQUIRE(harness.Frame(1).has_value());
				CHECK(harness.GetHost().GetImage(ViewportView::Scene).FrameIndex == 1);
				const RenderDebugView expected = fault == GpuFault::None ? RenderDebugView::Albedo : RenderDebugView::Lit;
				CHECK(editor.GetViewportState().GetDebugView() == expected);
				REQUIRE(harness.Frame(2).has_value());
				CHECK(editor.GetViewportState().GetDebugView() == expected);
				CHECK_FALSE(gpu.GetDevice().IsLost());
			}
		}

		TEST_CASE("EditorHostViewports: a debug-view switch that runs out of memory sets the setting to the view its renderer stayed on"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decision 44): the setting goes back to the debug view the scene view's renderer drew instead
			// (SceneRenderer::GetRenderedDebugView), so the next frames do not try again. After a shader reload, whose
			// CreatePipelines drops the material views' pipelines, a view last rendered in Albedo falls back to Lit, not to
			// Albedo; and a view last rendered in a data view (Overdraw, whose pipelines exist from startup) stays on it, not
			// on Lit. Each failed switch trims once, and the frames after it trim no more.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomPipelinePersistent });
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostOutOfMemoryStayedView");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			REQUIRE(harness.Frame(0).has_value());
			// Before the arming: Albedo renders, creating its pipelines, and a shader reload drops them again.
			REQUIRE(editor.GetViewportState().SetDebugView(RenderDebugView::Albedo).has_value());
			REQUIRE(harness.Frame(1).has_value());
			REQUIRE(harness.GetHost().GetRenderer(ViewportView::Scene)->GetRenderedDebugView() == RenderDebugView::Albedo);
			gpu.GetDevice().WaitForIdle();
			const Status reloaded = harness.GetPipelines().CreatePipelines();
			REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
			gpu.GetDevice().RunGarbageCollection(); // arms the fault
			const Test::TrimCounter counter(gpu.GetDevice().GetRecreation());

			REQUIRE(harness.Frame(2).has_value());
			CHECK(counter.GetTrims() == 1);
			CHECK(editor.GetViewportState().GetDebugView() == RenderDebugView::Lit);
			REQUIRE(harness.Frame(3).has_value());
			CHECK(counter.GetTrims() == 1);
			CHECK(editor.GetViewportState().GetDebugView() == RenderDebugView::Lit);

			REQUIRE(editor.GetViewportState().SetDebugView(RenderDebugView::Overdraw).has_value());
			REQUIRE(harness.Frame(4).has_value());
			REQUIRE(editor.GetViewportState().SetDebugView(RenderDebugView::Emissive).has_value());
			REQUIRE(harness.Frame(5).has_value());
			CHECK(counter.GetTrims() == 2);
			CHECK(editor.GetViewportState().GetDebugView() == RenderDebugView::Overdraw);
			REQUIRE(harness.Frame(6).has_value());
			CHECK(counter.GetTrims() == 2);
			CHECK(editor.GetViewportState().GetDebugView() == RenderDebugView::Overdraw);
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).FrameIndex == 6);
			CHECK_FALSE(gpu.GetDevice().IsLost());
		}

		TEST_CASE("EditorHostViewports: a view render that runs out of memory is trimmed for after the frame's submission, and the next frame renders again"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decision 44): selecting an entity after the arming makes the scene view create its selection targets,
			// which run out of memory: Render returns the out-of-memory Gpu error (never fatal), nothing is trimmed while the
			// frame's list is open, and Submitted trims once after the frame's submission. Under oom-rendertarget the next frame
			// creates them and renders, without another trim; under oom-rendertarget-persistent every failing frame trims once,
			// and a frame that renders again trims no more.
			for (const GpuFault fault : { GpuFault::OomRenderTarget, GpuFault::OomRenderTargetPersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				Test::EditorTestFixture fixture("HostOutOfMemoryFrame");
				fixture.CreateAndOpenProject();
				fixture.CreateAndOpenScene();
				auto& editor = fixture.GetEditor();
				const Entity mesh = editor.GetScene().CreateEntity("Mesh");
				mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
				editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
				editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
				REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
				REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
				HostGpuHarness harness(gpu, editor);
				harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
				REQUIRE(harness.Frame(0).has_value());
				gpu.GetDevice().RunGarbageCollection(); // arms the fault
				const Test::TrimCounter counter(gpu.GetDevice().GetRecreation());

				REQUIRE(editor.SetSelection({ mesh.GetUUID() }, SceneTarget::Edit).has_value());
				uint32_t trimsBeforeSubmission = 0;
				const Status failed = harness.Frame(1, {}, [&counter, &trimsBeforeSubmission]()
				{
					trimsBeforeSubmission = counter.GetTrims();
				});
				REQUIRE_FALSE(failed.has_value());
				CHECK(failed.error().GetCode() == ErrorCode::Gpu);
				CHECK(IsGpuOutOfMemory(failed.error(), gpu.GetDevice()));
				CHECK(trimsBeforeSubmission == 0);
				CHECK(counter.GetTrims() == 1);
				CHECK(harness.GetHost().GetImage(ViewportView::Scene).FrameIndex == 1);
				CHECK(harness.GetHost().GetImage(ViewportView::Game).FrameIndex == 1);
				CHECK(harness.GetSubmitted().has_value());

				const Status next = harness.Frame(2);
				if (fault == GpuFault::OomRenderTarget)
				{
					REQUIRE_MESSAGE(next.has_value(), next.error().ToString());
					CHECK(counter.GetTrims() == 1);
					CHECK(harness.GetHost().GetRenderer(ViewportView::Scene)->GetRenderStats().FrameIndex == 2);
				}
				else
				{
					REQUIRE_FALSE(next.has_value());
					CHECK(next.error().GetCode() == ErrorCode::Gpu);
					CHECK(counter.GetTrims() == 2);
					REQUIRE(editor.SetSelection({}, SceneTarget::Edit).has_value());
					const Status deselected = harness.Frame(3);
					REQUIRE_MESSAGE(deselected.has_value(), deselected.error().ToString());
					CHECK(counter.GetTrims() == 2);
					CHECK(harness.GetHost().GetRenderer(ViewportView::Scene)->GetRenderStats().FrameIndex == 3);
				}
				CHECK_FALSE(gpu.GetDevice().IsLost());
			}
		}

		TEST_CASE("EditorHostViewports: a view's Gpu error is returned before another view's error, after both views rendered"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decision 44): the scene view (index 0, in the Overdraw data view, which shadows nothing) fails first
			// with a partial InvalidArgument error (a mesh with a non-finite world matrix), and the game view after it with the
			// Gpu error of its shadow cascades, which a sun added after the arming makes it create and which run out of memory.
			// Render returns the Gpu error, the one the host trims for after the frame, and both views rendered the frame.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomRenderTargetPersistent });
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostGpuErrorFirst");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			Entity camera = editor.GetScene().CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 1.5f, 5.0f);
			});
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetViewportState().SetDebugView(RenderDebugView::Overdraw).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			harness.GetHost().SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
			REQUIRE(harness.Frame(0).has_value());
			gpu.GetDevice().RunGarbageCollection(); // arms the fault
			const Test::TrimCounter counter(gpu.GetDevice().GetRecreation());

			Entity broken = editor.GetScene().CreateEntity("Broken");
			broken.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			broken.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x = std::numeric_limits<float>::quiet_NaN();
			});
			editor.GetScene().CreateEntity("Sun").AddComponent<DirectionalLightComponent>();
			const Status rendered = harness.Frame(1);
			REQUIRE_FALSE(rendered.has_value());
			CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).FrameIndex == 1);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).FrameIndex == 1);
			CHECK(harness.GetHost().GetRenderer(ViewportView::Scene)->GetRenderStats().FrameIndex == 1);
			CHECK(counter.GetTrims() == 1);
			CHECK_FALSE(gpu.GetDevice().IsLost());
		}

		// Docs/Decisions/0030-m19-contract.md decision 11 (R-SOAK): --headless-game-view forces the Game view. With its panel
		// closed (a headless editor displays none), the view still renders every frame at the project's window size,
		// publishes its extent and statistics, and follows the play scene; without the force it stays withdrawn.
		TEST_CASE("EditorHostViewports: a forced game view renders while no Game panel is displayed" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostForcedGame");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, false).has_value());
			HostGpuHarness harness(gpu, editor);
			REQUIRE(harness.Frame(0).has_value());
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Texture == 0);

			REQUIRE(harness.GetHost().ForceGameView().has_value());
			REQUIRE(harness.Frame(1).has_value());
			const WindowSettings& window = editor.GetProject().GetSettings().Window;
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Texture != 0);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Width == window.Width);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Height == window.Height);
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Game).value() == glm::uvec2(window.Width, window.Height));
			REQUIRE(harness.GetHost().GetRenderer(ViewportView::Game) != nullptr);

			// It follows the play scene.
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }).has_value());
			REQUIRE(harness.Frame(2).has_value());
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Texture != 0);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).FrameIndex == 2);
			REQUIRE(editor.GetPlay().Stop().has_value());
		}

		// The forced view renders as a displayed Game panel with a fixed resolution would: at viewport.setOptions'
		// gameResolution when one is set, and back at the project's window size once it is cleared.
		TEST_CASE("EditorHostViewports: a forced game view takes the game resolution when one is set" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostForcedGameResolution");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, false).has_value());
			HostGpuHarness harness(gpu, editor);
			REQUIRE(harness.GetHost().ForceGameView().has_value());
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(320, 180)).has_value());
			REQUIRE(harness.Frame(0).has_value());
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Width == 320);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Height == 180);
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Game).value() == glm::uvec2(320, 180));
			const auto statistics = harness.GetHost().GetRenderer(ViewportView::Game)->GetRenderStats();
			CHECK(statistics.Available);
			CHECK(statistics.Width == 320);
			CHECK(statistics.Height == 180);

			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(0)).has_value());
			REQUIRE(harness.Frame(1).has_value());
			const WindowSettings& window = editor.GetProject().GetSettings().Window;
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Game).value() == glm::uvec2(window.Width, window.Height));
		}

		// M18's adaptation of the forced Game view at its merge with M19 (Docs/Decisions/0029-m18-contract.md "Merge with main"
		// and the milestone review). A headless editor's default layout keeps the Game panel open but tabbed behind the Scene
		// view, so the panel asks for no size (an empty rectangle), while the forced view renders at the project's window
		// size: the layout counts as ready once the forced view rendered that size, so editor.screenshot's fresh UI frame
		// completes, and not before. Before the fix IsLayoutReady compared the image with the panel's 0 x 0, which matched both
		// the missing image and the out-of-memory rule's unset failed size, so the view counted as laid out before it rendered
		// and before it rendered a new game resolution.
		TEST_CASE("EditorHostViewports: a forced game view whose Game panel is open but not drawn counts as laid out once it rendered"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostForcedGameLayout");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, false).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Game, {}); // open, but not drawn: no size
			REQUIRE(harness.GetHost().ForceGameView().has_value());
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(320, 180)).has_value());
			CHECK_FALSE(harness.GetHost().IsLayoutReady()); // not rendered yet
			REQUIRE(harness.Frame(0).has_value());
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Width == 320);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Height == 180);
			CHECK(harness.GetHost().IsLayoutReady());
			// A new game resolution is a new size to render.
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(400, 224)).has_value());
			CHECK_FALSE(harness.GetHost().IsLayoutReady());
			REQUIRE(harness.Frame(1).has_value());
			CHECK(harness.GetHost().IsLayoutReady());
		}

		TEST_CASE("EditorHostViewports: a forced game view whose new size runs out of memory stays at its previous size until the game resolution changes"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decision 44) through the forced view's size: a game resolution whose targets keep running out of memory
			// leaves the forced view at its previous size, without a trim per frame, and the layout counts as ready, until the
			// resolution changes again, which is a new request with its own trim.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomRenderTargetPersistent });
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostForcedGameOutOfMemory");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, false).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Game, {});
			REQUIRE(harness.GetHost().ForceGameView().has_value());
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(320, 180)).has_value());
			REQUIRE(harness.Frame(0).has_value());
			gpu.GetDevice().RunGarbageCollection(); // the first frame's collection arms the fault
			const Test::TrimCounter counter(gpu.GetDevice().GetRecreation());

			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(384, 216)).has_value());
			REQUIRE(harness.Frame(1).has_value());
			CHECK(counter.GetTrims() == 1);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).FrameIndex == 1);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Width == 320);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Height == 180);
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Game).value() == glm::uvec2(320, 180));
			CHECK(harness.GetHost().IsLayoutReady());
			// The size that failed is not tried again.
			REQUIRE(harness.Frame(2).has_value());
			CHECK(counter.GetTrims() == 1);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Width == 320);
			CHECK(harness.GetHost().IsLayoutReady());
			// Another resolution is a new request.
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(400, 224)).has_value());
			REQUIRE(harness.Frame(3).has_value());
			CHECK(counter.GetTrims() == 2);
			CHECK(harness.GetHost().GetImage(ViewportView::Game).Width == 320);
			CHECK_FALSE(gpu.GetDevice().IsLost());
		}

		TEST_CASE("EditorHostViewports: a device loss registers the forced game view's recreated image and renders the same image"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decisions 32 and 33) through the forced view: with its panel closed, the forced Game view's renderer
			// is released and recreated by the recovery like a displayed view's, the host's release removes its ImGui key, and
			// the first frame after the recovery registers the recreated image and renders what the view rendered before.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			struct UiContext
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~UiContext() { ImGui::DestroyContext(Context); }
			} ui;
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1);
			Test::EditorTestFixture fixture("HostForcedGameDeviceLoss");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, false).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, false).has_value());
			// The game resolution is the size the views' renderers are created with (640 x 360), so the forced view's first
			// frame resizes nothing and no target of an earlier size waits in its renderer's pool when the live counts are read
			// before the loss. A frame more before the loss would not help: it creates the objects of a second frame slot (the
			// GPU profiler's timer queries), which the one frame after the recovery does not.
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(640, 360)).has_value());
			GraphicsDevice& device = gpu.GetDevice();
			GpuResourceCache cache(device, editor.GetAssets());
			auto pipelines = SceneRendererPipelines::Create(device, gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), {});
			REQUIRE(renderer.has_value());
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			struct WaitForHostGpu
			{
				GraphicsDevice& Device;
				~WaitForHostGpu() { Device.WaitForIdle(); }
			} idle{ device };
			REQUIRE(host.Initialize(device, **pipelines, cache, **renderer).has_value());
			REQUIRE(host.ForceGameView().has_value());

			// One frame of the forced view in a command list of its own, then its final image read back; every error is
			// returned, so it also runs on the lost device.
			uint64_t frame = 0;
			Test::DeviceLossTest test;
			test.Owners = { "EditorHostViewports" };
			test.Use = [&device, &renderer, &host, &frame]() -> Result<std::vector<std::byte>>
			{
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
				device.WaitForIdle();
				(*renderer)->BeginFrame(static_cast<uint32_t>(frame % 2));
				list->open();
				RenderContext context{ .Device = &device, .CommandList = list, .FrameIndex = frame };
				const Status rendered = host.Render(context);
				list->close();
				ENGINE_TRY(host.Submitted(frame, device.ExecuteCommandList(*list)));
				++frame;
				ENGINE_TRY(rendered);
				device.WaitForIdle();
				const SceneRenderer* viewRenderer = host.GetRenderer(ViewportView::Game);
				if (viewRenderer == nullptr || viewRenderer->GetFinalTexture() == nullptr || host.GetImage(ViewportView::Game).Texture == 0)
					return MakeError(ErrorCode::InvalidState, "the forced game view has no registered image after its frame");
				if (host.GetImage(ViewportView::Game).Width != 640 || host.GetImage(ViewportView::Game).Height != 360)
					return MakeError(ErrorCode::InvalidState, "the forced game view did not render at the game resolution");
				Readback readback(device);
				ENGINE_TRY_ASSIGN(const Image image, readback.ReadTexture(*viewRenderer->GetFinalTexture()));
				return image.Pixels;
			};
			const uint64_t withdrawals = host.GetTextureWithdrawalCount();
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), (report.has_value() ? std::string() : report.error().ToString()));
			CHECK(host.GetTextureWithdrawalCount() == withdrawals + 1);
			CHECK(host.IsLayoutReady());
			device.WaitForIdle();
			host.Retire();
			(*renderer)->DestroyTextures();
		}
	}

}
