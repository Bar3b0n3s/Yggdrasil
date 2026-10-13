#include "TestsPCH.h"

#include "Editor/Private/EditorHostViewports.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/WaitUntil.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

			// One frame: records both views, submits them and notifies the host. Returns Render's status.
			Status Frame(uint64_t frame, std::optional<EditorViewportClick> click = {})
			{
				m_Device.WaitForIdle();
				m_Renderer->BeginFrame(static_cast<uint32_t>(frame % 2));
				m_Command->open();
				RenderContext context{ .Device = &m_Device, .CommandList = m_Command, .FrameIndex = frame };
				const Status rendered = m_Host.Render(context);
				if (click)
					REQUIRE(m_Host.RequestPick(*click).has_value());
				m_Command->close();
				const uint64_t submission = m_Device.ExecuteCommandList(*m_Command);
				m_Submitted = m_Host.Submitted(frame, submission);
				return rendered;
			}

			[[nodiscard]] EditorHostViewports& GetHost() { return m_Host; }
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
	}

}
