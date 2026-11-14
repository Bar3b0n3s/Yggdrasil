#include "TestsPCH.h"

#include "Editor/Private/EditorHostViewports.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/EditorCamera.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/TemporalHistory.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
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
#include <variant>
#include <vector>

namespace Engine {

	namespace {

		// The host's GPU services over a headless device: its pipelines, the ImGui texture registry and one command list,
		// with frame-by-frame recording and submission like EditorApp's frame loop. Destroyed before the fixture's checks.
		class HostGpuHarness
		{
		public:
			HostGpuHarness(Test::HeadlessGpuFixture& gpu, EditorContext& editor)
				: m_Device(gpu.GetDevice()), m_Editor(&editor), m_Cache(m_Device, editor.GetAssets()), m_Gizmos(editor), m_Host(editor, m_Gizmos)
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
				m_Capture.reset();
				m_Command = nullptr;
				m_Host.Retire();
				m_Renderer->DestroyTextures();
			}
			HostGpuHarness(const HostGpuHarness&) = delete;
			HostGpuHarness& operator=(const HostGpuHarness&) = delete;

			// One frame: records both views, submits them and notifies the host. Returns Render's status. `beforeSubmission`,
			// when set, runs after the recording and before the submission. `capture`: the frame is a capture of the editor UI,
			// as editor.screenshot's fresh frame is (EditorHostViewports::Render), through the harness's viewport capture.
			Status Frame(uint64_t frame, std::optional<EditorViewportClick> click = {}, const std::function<void()>& beforeSubmission = {}, bool capture = false)
			{
				m_Device.WaitForIdle();
				m_Renderer->BeginFrame(static_cast<uint32_t>(frame % 2));
				m_Command->open();
				RenderContext context{ .Device = &m_Device, .CommandList = m_Command, .FrameIndex = frame };
				const Status rendered = m_Host.Render(context, capture ? &GetCapture() : nullptr);
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
			// The viewport capture over the views' pipelines and cache, as EditorApp's, created at its first use.
			[[nodiscard]] ViewportCapture& GetCapture()
			{
				if (m_Capture == nullptr)
				{
					auto capture = ViewportCapture::CreateForScenes(m_Device, *m_Pipelines, m_Cache, m_Editor->GetAssets());
					REQUIRE_MESSAGE(capture.has_value(), (capture ? std::string() : capture.error().ToString()));
					m_Capture = std::move(*capture);
				}
				return *m_Capture;
			}
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
			EditorContext* m_Editor = nullptr; // the test's, which outlives the harness
			GpuResourceCache m_Cache;
			Scope<SceneRendererPipelines> m_Pipelines{};
			Scope<ImGuiRenderer> m_Renderer{};
			nvrhi::CommandListHandle m_Command{};
			Scope<ViewportCapture> m_Capture{}; // GetCapture's, released after the device idles
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

		TEST_CASE("EditorHostViewports: a window's cursor position maps to the game image through the main viewport's desktop position")
		{
			// M24 (Docs/Decisions/0035-m24-contract.md decision 18, the coordinate audit): with platform windows on, Dear ImGui's
			// coordinates, the Game panel's rectangle among them, are the desktop's, while a native window event's cursor
			// position is relative to the main window's client area, which starts at the main viewport's position.
			Test::EditorTestFixture fixture("HostGameDesktopCoordinates");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			struct UiContext
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~UiContext() { ImGui::DestroyContext(Context); }
			} ui;
			ImGui::GetMainViewport()->Pos = ImVec2(300.0f, 200.0f);
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			// The Game panel at (700, 280) on the desktop, (400, 80) in the main window, 800x450 logical units showing a
			// 1920x1080 image.
			host.SetRectangle(ViewportView::Game, { .Min = { 700.0f, 280.0f }, .Size = { 800.0f, 450.0f }, .ImageSize = { 1920, 1080 } });
			host.SetGameInputFocused(true);
			const auto corner = host.ToGameEvent(MouseMoveEvent{ .Position = { 400.0f, 80.0f } });
			REQUIRE(corner.has_value());
			CHECK(std::get<MouseMoveEvent>(*corner).Position == glm::vec2(0.0f, 0.0f));
			const auto center = host.ToGameEvent(MouseMoveEvent{ .Position = { 800.0f, 305.0f } });
			REQUIRE(center.has_value());
			CHECK(std::get<MouseMoveEvent>(*center).Position == glm::vec2(960.0f, 540.0f));
			// A press over the image is game input. The window's (1300, 600) is the desktop's (1600, 800), below and right of the
			// image, so a press there is not.
			CHECK(host.ToGameEvent(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Pressed }).has_value());
			CHECK(host.ToGameEvent(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Released }).has_value());
			REQUIRE(host.ToGameEvent(MouseMoveEvent{ .Position = { 1300.0f, 600.0f } }).has_value());
			CHECK_FALSE(host.ToGameEvent(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Pressed }).has_value());
			host.SetGameInputFocused(false);
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

		// M21 (Docs/Decisions/0032-m21-contract.md decision 4, stream D): the editor's scene and game views copy the project's
		// SortingLayers.Layers into their extraction request, in edit and in play (PlaySession::ExtractView takes the request
		// as it is), as they copy the rendering quality (EditorHostViewports::Extract, over stream A's section and sprite
		// extraction; decision 18's integration case).
		TEST_CASE("EditorHostViewports: scene and game extraction carry the project's sorting layers")
		{
			Test::EditorTestFixture fixture("HostSortingLayers");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Test::AutomationTestClient client(editor);
			REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "SortingLayers", Json{ { "Layers", Json::array({ "Default", "Background", "Foreground" }) } } } } } }).has_value());
			// Front comes first in canonical order and has the lower OrderInLayer: only its sorting layer draws it last.
			const Result<Json> front = client.Call("entity.create", Json{ { "name", "Front" }, { "components", Json{ { "SpriteRenderer", Json{ { "SortingLayer", "Foreground" }, { "OrderInLayer", 0 } } } } } });
			const Result<Json> back = client.Call("entity.create", Json{ { "name", "Back" }, { "components", Json{ { "SpriteRenderer", Json{ { "SortingLayer", "Background" }, { "OrderInLayer", 5 } } } } } });
			REQUIRE(front.has_value());
			REQUIRE(back.has_value());
			const std::vector<std::string> expected = { "Back", "Front" };
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			for (const bool playing : { false, true })
			{
				CAPTURE(playing);
				if (playing)
					REQUIRE(client.Call("play.start", Json{ { "paused", true } }).has_value());
				const Scene& shown = playing ? editor.GetPlay().GetSession()->GetScene() : editor.GetScene();
				for (const ViewportView view : { ViewportView::Scene, ViewportView::Game })
				{
					const auto snapshot = host.Extract(view, 160, 90);
					REQUIRE(snapshot.has_value());
					std::vector<std::string> order;
					for (const SpriteItem& sprite : snapshot->Sprites)
						order.push_back(shown.FindEntityByID(sprite.Entity).GetName());
					CHECK(order == expected);
					REQUIRE(snapshot->Sprites.size() == 2);
					CHECK(snapshot->Sprites[0].SortingLayer == 1);
					CHECK(snapshot->Sprites[1].SortingLayer == 2);
				}
			}
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
		}

		// M20 (Docs/Decisions/0031-m20-contract.md decisions 13 and 23): while a session plays one open scene, another tab's
		// scene view shows that tab's edit scene, as its hierarchy does, and the game view keeps showing the session; the
		// played scene's tab shows the play scene in both views.
		TEST_CASE("EditorHostViewports: the scene view shows the active scene's edit scene while another open scene plays")
		{
			Test::EditorTestFixture fixture("HostOpenScenes");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			const auto addMesh = [&editor](std::string_view name)
			{
				editor.GetScene().CreateEntity(std::string(name)).AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			};
			// Main holds one mesh and Second two, so each snapshot tells which scene it shows.
			const OpenSceneId main = editor.GetActiveSceneId();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			addMesh("Played");
			REQUIRE(client.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }).has_value());
			const OpenSceneId second = editor.GetActiveSceneId();
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			addMesh("EditedA");
			addMesh("EditedB");
			REQUIRE(client.Call("scene.activate", Json{ { "scene", main } }).has_value());
			REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			const auto meshes = [&host](ViewportView view) -> size_t
			{
				const Result<RenderSnapshot> snapshot = host.Extract(view, 160, 90);
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				return snapshot->Meshes.size();
			};
			CHECK(meshes(ViewportView::Scene) == 1);
			CHECK(meshes(ViewportView::Game) == 1);

			REQUIRE(client.Call("scene.activate", Json{ { "scene", second } }).has_value());
			CHECK(meshes(ViewportView::Scene) == 2);
			const Result<RenderSnapshot> shown = host.Extract(ViewportView::Scene, 160, 90);
			REQUIRE(shown.has_value());
			CHECK(shown->SceneRevision == editor.GetScene().GetRevision());
			CHECK(meshes(ViewportView::Game) == 1);

			// The played scene's tab shows the play scene again.
			REQUIRE(client.Call("scene.activate", Json{ { "scene", main } }).has_value());
			CHECK(meshes(ViewportView::Scene) == 1);
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
			REQUIRE(client.Call("scene.activate", Json{ { "scene", second } }).has_value());
			CHECK(meshes(ViewportView::Scene) == 2);
			CHECK(meshes(ViewportView::Game) == 2);
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

		// M26 (Docs/Decisions/0037-m26-contract.md decision 10; W26.2's review): each displayed view applies its history to
		// every render, so its renders count up from its first (RenderStats::HistoryFrames), and starts it afresh when the shown
		// scene changes (here play starting and stopping, whose scene keeps the camera's entity, so the history alone would
		// continue) and when the view gets a new renderer (a view hidden while the scene changed, shown again).
		TEST_CASE("EditorHostViewports: each view continues its temporal history between renders and starts it afresh when the shown scene changes"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostViewHistory");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			const Entity mesh = editor.GetScene().CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			Entity camera = editor.GetScene().CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 0.0f, 5.0f);
			});
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			{
				HostGpuHarness harness(gpu, editor);
				EditorHostViewports& host = harness.GetHost();
				host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				host.SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
				const auto historyFrames = [&host](ViewportView view) -> uint32_t
				{
					const SceneRenderer* renderer = host.GetRenderer(view);
					REQUIRE(renderer != nullptr);
					const RenderStats stats = renderer->GetRenderStats();
					REQUIRE(stats.Available);
					return stats.HistoryFrames;
				};
				uint64_t frame = 0;
				for (uint32_t expected = 0; expected < 3; ++expected)
				{
					REQUIRE(harness.Frame(frame++).has_value());
					CHECK(historyFrames(ViewportView::Scene) == expected);
					CHECK(historyFrames(ViewportView::Game) == expected);
				}

				// Play starting replaces the shown scene: both views start afresh, then continue.
				REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
				for (uint32_t expected = 0; expected < 2; ++expected)
				{
					REQUIRE(harness.Frame(frame++).has_value());
					CHECK(historyFrames(ViewportView::Scene) == expected);
					CHECK(historyFrames(ViewportView::Game) == expected);
				}

				// The game view hidden while play stops releases its renderer; shown again, its new renderer starts afresh.
				host.SetRectangle(ViewportView::Game, {});
				REQUIRE(client.Call("play.stop", Json::object()).has_value());
				REQUIRE(harness.Frame(frame++).has_value());
				CHECK(host.GetRenderer(ViewportView::Game) == nullptr);
				CHECK(historyFrames(ViewportView::Scene) == 0);
				host.SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
				REQUIRE(harness.Frame(frame++).has_value());
				CHECK(historyFrames(ViewportView::Game) == 0);
				CHECK(historyFrames(ViewportView::Scene) == 1);
				REQUIRE(harness.Frame(frame++).has_value());
				CHECK(historyFrames(ViewportView::Game) == 1);
			}
		}

		// M26 (Docs/Decisions/0037-m26-contract.md decision 10; the milestone review): a capture's frame, as editor.screenshot's
		// fresh UI frame is, renders each displayed view as every capture renders: the capture's warm-up from a reset history at
		// delta time 0 (ViewportCapture::RenderWarmup), the frame's own render its last. So two captures of an unchanged TAA and
		// auto-exposure scene show byte-identical views, however many live frames (each a step of the jitter sequence and of
		// the steady clock's adaptation) ran before each, and the live frames after a capture go on from its history.
		TEST_CASE("EditorHostViewports: a capture's frame renders each view's warm-up from a reset history, so two captures of a TAA scene are byte-identical"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostCaptureWarmup");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			Entity mesh = scene.CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			mesh.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::quat(glm::radians(glm::vec3(20.0f, 35.0f, 0.0f)));
			});
			scene.CreateEntity("Sun").AddComponent<DirectionalLightComponent>();
			scene.CreateEntity("Post").AddComponent<PostProcessComponent>(PostProcessComponent{ .ExposureMode = ExposureMode::Auto, .AntiAliasing = AntiAliasing::Taa });
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 0.0f, 4.0f);
			});
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true).has_value());
			{
				HostGpuHarness harness(gpu, editor);
				EditorHostViewports& host = harness.GetHost();
				host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				host.SetRectangle(ViewportView::Game, { .Size = glm::vec2(80, 80) });
				// Both views' final images, read back after their frame.
				const auto images = [&gpu, &host]()
				{
					gpu.GetDevice().WaitForIdle();
					Readback readback(gpu.GetDevice());
					std::vector<std::byte> pixels;
					for (const ViewportView view : { ViewportView::Scene, ViewportView::Game })
					{
						const SceneRenderer* renderer = host.GetRenderer(view);
						REQUIRE(renderer != nullptr);
						REQUIRE(renderer->GetFinalTexture() != nullptr);
						const Result<Image> image = readback.ReadTexture(*renderer->GetFinalTexture());
						REQUIRE_MESSAGE(image.has_value(), (image ? std::string() : image.error().ToString()));
						pixels.insert(pixels.end(), image->Pixels.begin(), image->Pixels.end());
					}
					return pixels;
				};
				const auto historyFrames = [&host](ViewportView view) -> uint32_t
				{
					const SceneRenderer* renderer = host.GetRenderer(view);
					REQUIRE(renderer != nullptr);
					const RenderStats stats = renderer->GetRenderStats();
					REQUIRE(stats.Available);
					return stats.HistoryFrames;
				};
				uint64_t frame = 0;
				for (int live = 0; live < 3; ++live)
					REQUIRE(harness.Frame(frame++).has_value());
				REQUIRE(harness.Frame(frame++, {}, {}, true).has_value());
				const std::vector<std::byte> captured = images();
				// The frame's render was the warm-up's last, the history's eighth from its reset.
				CHECK(historyFrames(ViewportView::Scene) == CaptureWarmupFrames - 1);
				CHECK(historyFrames(ViewportView::Game) == CaptureWarmupFrames - 1);
				CHECK(host.GetImage(ViewportView::Scene).FrameIndex == frame - 1);
				// The live frames go on from the capture's history.
				REQUIRE(harness.Frame(frame++).has_value());
				CHECK(historyFrames(ViewportView::Scene) == CaptureWarmupFrames);
				CHECK(historyFrames(ViewportView::Game) == CaptureWarmupFrames);
				for (int live = 0; live < 4; ++live)
					REQUIRE(harness.Frame(frame++).has_value());
				// Five live frames later, another phase of the jitter sequence: the next capture shows the same views.
				REQUIRE(harness.Frame(frame++, {}, {}, true).has_value());
				CHECK(historyFrames(ViewportView::Scene) == CaptureWarmupFrames - 1);
				const bool identical = images() == captured;
				CHECK(identical);
			}
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

		// M20 (decisions 13 and 23): a click in the scene view of an open scene that is not the played one selects in that
		// scene's edit scene, which the view shows, never in the play scene.
		TEST_CASE("EditorHostViewports: a pick in another open scene's tab while a session plays selects in that tab's edit scene"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostOpenScenePick");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			const auto addCube = [&editor](std::string_view name)
			{
				const Entity cube = editor.GetScene().CreateEntity(std::string(name));
				cube.AddComponent<MeshRendererComponent>(
					MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
				return cube.GetUUID();
			};
			const OpenSceneId main = editor.GetActiveSceneId();
			const UUID played = addCube("Played");
			REQUIRE(client.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }).has_value());
			const OpenSceneId second = editor.GetActiveSceneId();
			const UUID edited = addCube("Edited");
			REQUIRE(played != edited);
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(client.Call("scene.activate", Json{ { "scene", main } }).has_value());
			REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
			REQUIRE(client.Call("scene.activate", Json{ { "scene", second } }).has_value());
			{
				HostGpuHarness harness(gpu, editor);
				EditorHostViewports& host = harness.GetHost();
				host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				const auto click = [&host](uint64_t frame, uint64_t sequence)
				{
					const EditorViewportImage image = host.GetImage(ViewportView::Scene);
					return EditorViewportClick{ .View = ViewportView::Scene,
						.Pixel = { image.Width / 2, image.Height / 2 },
						.FrameIndex = frame,
						.SceneRevision = image.SceneRevision,
						.Sequence = sequence,
						.ViewGeneration = image.Generation };
				};
				REQUIRE(harness.Frame(0).has_value());
				REQUIRE(harness.Frame(1, click(1, 1)).has_value());
				// The pick's result is read back at its fixed frame latency (the milestone review, T6): not one frame early.
				REQUIRE(harness.Frame(2).has_value());
				CHECK(editor.GetSelection().empty());
				REQUIRE(harness.Frame(3).has_value());
				CHECK(editor.GetActiveSceneId() == second);
				CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
				CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ edited });
			}
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
		}

		// M21 (Docs/Decisions/0032-m21-contract.md decision 14, stream D): sprites write no EntityId, so the click selection asks
		// PickSprite first, in the project's sorting layers, and takes the GPU pick's mesh only where no sprite shows (over
		// stream A's PickSprite and sorting-layers section; decision 18's integration case).
		TEST_CASE("EditorHostViewports: a click selects the sprite its pixel shows before the mesh the GPU pick reads"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostSpritePick");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "SortingLayers", Json{ { "Layers", Json::array({ "Default", "Background", "Foreground" }) } } } } } }).has_value());
			// A cube between the camera and the sprites, which the GPU pick reads at the centre pixel.
			const Entity cube = editor.GetScene().CreateEntity("Cube");
			cube.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			cube.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = { 0.0f, 0.0f, 2.0f };
			});
			// Front comes first in canonical order and has the lower OrderInLayer: only its sorting layer draws it last.
			REQUIRE(client.Call("entity.create", Json{ { "name", "Front" }, { "components", Json{ { "SpriteRenderer", Json{ { "SortingLayer", "Foreground" }, { "OrderInLayer", 0 } } } } } }).has_value());
			REQUIRE(client.Call("entity.create", Json{ { "name", "Back" }, { "components", Json{ { "SpriteRenderer", Json{ { "SortingLayer", "Background" }, { "OrderInLayer", 5 } } } } } }).has_value());
			REQUIRE(client.Call("viewport.setOptions", Json{ { "grid", false }, { "icons", false }, { "mode2D", true } }).has_value());
			REQUIRE(client.Call("viewport.camera", Json{ { "position", { 0, 0, 500 } }, { "target", { 0, 0, 0 } }, { "orthographicSize", 2 } }).has_value());
			const UUID front = editor.GetScene().FindEntityByPath("/Front").GetUUID();
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			{
				HostGpuHarness harness(gpu, editor);
				EditorHostViewports& host = harness.GetHost();
				host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				REQUIRE(harness.Frame(0).has_value());
				const EditorViewportImage image = host.GetImage(ViewportView::Scene);
				const EditorViewportClick click{ .View = ViewportView::Scene,
					.Pixel = { image.Width / 2, image.Height / 2 },
					.FrameIndex = 1,
					.SceneRevision = image.SceneRevision,
					.Sequence = 1,
					.ViewGeneration = image.Generation };
				REQUIRE(harness.Frame(1, click).has_value());
				REQUIRE(harness.Frame(2).has_value());
				REQUIRE(harness.Frame(3).has_value());
				CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
				CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ front });
			}
		}

		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24, stream T1): tiles write no EntityId either, so the click
		// selection picks a tile among the clicked image's tilemaps as it picks a sprite, selecting its tilemap's entity.
		TEST_CASE("EditorHostViewports: a click selects the tilemap whose tile its pixel shows" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostTilemapPick");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			// A 16-texel frame at the default 16 pixels per unit is one unit: the four tiles of chunk (0, 0) cover [0, 2]^2, and the
			// view's centre shows the tile of cell (0, 0).
			REQUIRE(client.Call("asset.create", Json{ { "type", "SpriteSheet" }, { "path", "Assets/Sprites/Tiles.spritesheet" }, { "values", Json{ { "Frames", Json::array({ Json{ { "Rect", Json{ { "Width", 16 }, { "Height", 16 } } } } }) } } } })
					.has_value());
			REQUIRE(client.Call("asset.create", Json{ { "type", "Tilemap" }, { "path", "Assets/Tilemaps/Ground.tilemap" }, { "values", Json{ { "ChunkSize", 2 }, { "Palette", Json::array({ Json{ { "Sheet", "Assets/Sprites/Tiles.spritesheet" } } }) }, { "Chunks", Json::array({ Json{ { "X", 0 }, { "Y", 0 }, { "Tiles", Json::array({ 1, 1, 1, 1 }) } } }) } } } })
					.has_value());
			REQUIRE(client.Call("entity.create", Json{ { "name", "Ground" }, { "components", Json{ { "TilemapRenderer", Json{ { "Tilemap", "Assets/Tilemaps/Ground.tilemap" } } } } } })
					.has_value());
			const UUID ground = editor.GetScene().FindEntityByPath("/Ground").GetUUID();
			REQUIRE(client.Call("viewport.setOptions", Json{ { "grid", false }, { "icons", false }, { "mode2D", true } }).has_value());
			REQUIRE(client.Call("viewport.camera", Json{ { "position", { 0.5, 0.5, 500 } }, { "target", { 0.5, 0.5, 0 } }, { "orthographicSize", 2 } }).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			{
				HostGpuHarness harness(gpu, editor);
				EditorHostViewports& host = harness.GetHost();
				host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				REQUIRE(harness.Frame(0).has_value());
				const EditorViewportImage image = host.GetImage(ViewportView::Scene);
				const EditorViewportClick click{ .View = ViewportView::Scene,
					.Pixel = { image.Width / 2, image.Height / 2 },
					.FrameIndex = 1,
					.SceneRevision = image.SceneRevision,
					.Sequence = 1,
					.ViewGeneration = image.Generation };
				REQUIRE(harness.Frame(1, click).has_value());
				REQUIRE(harness.Frame(2).has_value());
				REQUIRE(harness.Frame(3).has_value());
				CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
				CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ ground });
			}
		}

		// M21 (decision 14; W21.1's review): the sprite is picked in the clicked image, among the sprites that image drew, so a
		// sprite that a playing session moves on before the GPU pick completes (two frames later) is still the one selected.
		TEST_CASE("EditorHostViewports: a click selects the sprite the clicked image showed, though it moves on before the pick completes"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostMovingSpritePick");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			// A sprite at the view's centre whose 2D body falls under the project's gravity.
			REQUIRE(client.Call("entity.create", Json{ { "name", "Faller" }, { "components", Json{ { "SpriteRenderer", Json::object() }, { "RigidBody2D", Json::object() }, { "CircleCollider2D", Json{ { "Radius", 0.5 } } } } } }).has_value());
			const UUID faller = editor.GetScene().FindEntityByPath("/Faller").GetUUID();
			REQUIRE(client.Call("viewport.setOptions", Json{ { "grid", false }, { "icons", false }, { "mode2D", true } }).has_value());
			REQUIRE(client.Call("viewport.camera", Json{ { "position", { 0, 0, 500 } }, { "target", { 0, 0, 0 } }, { "orthographicSize", 2 } }).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
			{
				HostGpuHarness harness(gpu, editor);
				EditorHostViewports& host = harness.GetHost();
				host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				REQUIRE(harness.Frame(0).has_value());
				const EditorViewportImage image = host.GetImage(ViewportView::Scene);
				const EditorViewportClick click{ .View = ViewportView::Scene,
					.Pixel = { image.Width / 2, image.Height / 2 },
					.FrameIndex = 1,
					.SceneRevision = image.SceneRevision,
					.Sequence = 1,
					.ViewGeneration = image.Generation };
				REQUIRE(harness.Frame(1, click).has_value());
				// The session steps on: the sprite falls well below the clicked pixel before the pick completes.
				REQUIRE(client.Call("play.step", Json{ { "ticks", 40 } }).has_value());
				REQUIRE(harness.Frame(2).has_value());
				REQUIRE(harness.Frame(3).has_value());
				CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
				CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ faller });
			}
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
		}

		// M21's milestone review: a playing SpriteAnimator writes its sprite's frame through the scene every tick, which moves the
		// play scene's revision, while the click is still resolved against the clicked image: ticks between the click and the
		// GPU pick's completion make no pick stale (decision 14), so the animated sprite is selected.
		TEST_CASE("EditorHostViewports: a click selects a sprite whose animator plays on before the pick completes"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostAnimatedSpritePick");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			// Two 16-texel frames, the clip changing frame every tick: one unit at the default 16 pixels per unit.
			const Json frames = Json::array({ Json{ { "Rect", Json{ { "X", 0 }, { "Y", 0 }, { "Width", 16 }, { "Height", 16 } } } },
				Json{ { "Rect", Json{ { "X", 16 }, { "Y", 0 }, { "Width", 16 }, { "Height", 16 } } } } });
			const Json clips = Json::array({ Json{ { "Name", "Walk" }, { "Frames", Json::array({ Json{ { "Frame", 0 }, { "Ticks", 1 } }, Json{ { "Frame", 1 }, { "Ticks", 1 } } }) }, { "Loop", true } } });
			REQUIRE(client.Call("asset.create", Json{ { "type", "SpriteSheet" }, { "path", "Assets/Sprites/Walk.spritesheet" }, { "values", Json{ { "Frames", frames }, { "Clips", clips } } } })
					.has_value());
			REQUIRE(client.Call("entity.create", Json{ { "name", "Walker" }, { "components", Json{ { "SpriteRenderer", Json{ { "Sheet", "Assets/Sprites/Walk.spritesheet" } } }, { "SpriteAnimator", Json{ { "Clip", "Walk" }, { "Playing", true } } } } } })
					.has_value());
			const UUID walker = editor.GetScene().FindEntityByPath("/Walker").GetUUID();
			REQUIRE(client.Call("viewport.setOptions", Json{ { "grid", false }, { "icons", false }, { "mode2D", true } }).has_value());
			REQUIRE(client.Call("viewport.camera", Json{ { "position", { 0, 0, 500 } }, { "target", { 0, 0, 0 } }, { "orthographicSize", 2 } }).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
			{
				HostGpuHarness harness(gpu, editor);
				EditorHostViewports& host = harness.GetHost();
				host.SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
				REQUIRE(harness.Frame(0).has_value());
				const EditorViewportImage image = host.GetImage(ViewportView::Scene);
				const EditorViewportClick click{ .View = ViewportView::Scene,
					.Pixel = { image.Width / 2, image.Height / 2 },
					.FrameIndex = 1,
					.SceneRevision = image.SceneRevision,
					.Sequence = 1,
					.ViewGeneration = image.Generation };
				REQUIRE(harness.Frame(1, click).has_value());
				// Two ticks: the animator writes the sprite's frame and its own step through the scene in each.
				const uint64_t before = editor.GetPlay().GetSession()->GetScene().GetRevision();
				REQUIRE(client.Call("play.step", Json{ { "ticks", 2 } }).has_value());
				REQUIRE(editor.GetPlay().GetSession()->GetScene().GetRevision() != before);
				REQUIRE(harness.Frame(2).has_value());
				REQUIRE(harness.Frame(3).has_value());
				CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
				CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ walker });
			}
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
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

		// M21 (Docs/Decisions/0032-m21-contract.md decision 14, stream D): the 2D mode's grid lies in the plane z = 0.
		TEST_CASE("EditorHostViewports: the 2D mode draws the scene view's grid in the plane z = 0 instead of the ground grid")
		{
			Test::EditorTestFixture fixture("HostGrid2D");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			EditorViewportState& state = editor.GetViewportState();
			state.SetOptions({ .Grid = true, .Icons = false });
			const auto ground = host.Extract(ViewportView::Scene, 320, 180);
			REQUIRE(ground.has_value());
			CHECK(HasFlag(ground->Flags, RenderViewFlags::Grid));
			CHECK(ground->DebugDraw.GetCommands().empty());

			state.SetOptions({ .Grid = true, .Icons = false, .Mode2D = true });
			const Result<ExplicitRenderCamera> flat = EditorCamera::Make2D(state.GetCamera());
			REQUIRE(flat.has_value());
			REQUIRE(state.SetCamera(*flat).has_value());
			const auto plane = host.Extract(ViewportView::Scene, 320, 180);
			REQUIRE(plane.has_value());
			CHECK_FALSE(HasFlag(plane->Flags, RenderViewFlags::Grid));
			REQUIRE_FALSE(plane->DebugDraw.GetCommands().empty());
			for (const DebugDrawCommand& command : plane->DebugDraw.GetCommands())
			{
				const auto* line = std::get_if<DebugLine>(&command.Shape);
				REQUIRE(line != nullptr);
				CHECK(line->From.z == 0.0f);
				CHECK(line->To.z == 0.0f);
			}
			// The game view never inherits the scene view's overlays, its grid included.
			const auto game = host.Extract(ViewportView::Game, 320, 180);
			REQUIRE(game.has_value());
			CHECK(game->DebugDraw.GetCommands().empty());
			// Without the Grid option the 2D mode draws no grid either.
			state.SetOptions({ .Grid = false, .Icons = false, .Mode2D = true });
			const auto hidden = host.Extract(ViewportView::Scene, 320, 180);
			REQUIRE(hidden.has_value());
			CHECK(hidden->DebugDraw.GetCommands().empty());
		}

		// M22 (Docs/Decisions/0033-m22-contract.md decision 33): with the UI preview the scene view extracts what the game view
		// does, through the primary camera with its UI, without the editor overlays, the selection or the debug view.
		TEST_CASE("EditorHostViewports: the UI preview extracts the scene view through the primary camera with its UI and no editor overlays")
		{
			Test::EditorTestFixture fixture("HostUiPreview");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const Entity mesh = editor.GetScene().CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			const Entity canvas = editor.GetScene().CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>();
			const Entity image = editor.GetScene().CreateEntity("Image", canvas);
			image.AddComponent<UiRectComponent>();
			image.AddComponent<UiImageComponent>();
			REQUIRE(editor.SetSelection({ mesh.GetUUID() }, SceneTarget::Edit).has_value());
			REQUIRE(editor.GetViewportState().SetDebugView(RenderDebugView::Normals).has_value());
			GizmoController gizmos(editor);
			EditorHostViewports host(editor, gizmos);
			const auto edited = host.Extract(ViewportView::Scene, 320, 240);
			REQUIRE(edited.has_value());
			CHECK(HasFlag(edited->Flags, RenderViewFlags::EditorOverlays));
			CHECK(edited->DebugView == RenderDebugView::Normals);
			// The scene view's own camera draws no Screen canvas (ADR 0033 decisions 7 and 51; it would draw a World canvas).
			CHECK(edited->Ui.Items.empty());

			EditorViewportOptions options = editor.GetViewportState().GetOptions();
			options.UiPreview = true;
			editor.GetViewportState().SetOptions(options);
			const auto preview = host.Extract(ViewportView::Scene, 320, 240);
			const auto game = host.Extract(ViewportView::Game, 320, 240);
			REQUIRE(preview.has_value());
			REQUIRE(game.has_value());
			CHECK(preview->Flags == RenderViewFlags::None);
			CHECK(preview->SelectedEntities.empty());
			CHECK(preview->DebugView == RenderDebugView::Lit);
			CHECK(preview->Camera.View == game->Camera.View);
			CHECK(preview->Camera.Projection == game->Camera.Projection);
			CHECK(preview->Ui.Items.size() == 1);
			CHECK(preview->Ui.Items.size() == game->Ui.Items.size());
		}

		TEST_CASE("EditorHostViewports: the UI preview renders the scene view at the game frame without a picking image" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::EditorTestFixture fixture("HostUiPreviewFrame");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, true).has_value());
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(160, 90)).has_value());
			HostGpuHarness harness(gpu, editor);
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 64) });
			REQUIRE(harness.Frame(0).has_value());
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).Width == 96);
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).Picking);

			EditorViewportOptions options = editor.GetViewportState().GetOptions();
			options.UiPreview = true;
			editor.GetViewportState().SetOptions(options);
			// The panel hands the letterboxed frame; the host renders the game frame whatever its size on screen.
			harness.GetHost().SetRectangle(ViewportView::Scene, { .Size = glm::vec2(96, 54) });
			REQUIRE(harness.Frame(1).has_value());
			const EditorViewportImage image = harness.GetHost().GetImage(ViewportView::Scene);
			CHECK(image.Width == 160);
			CHECK(image.Height == 90);
			CHECK_FALSE(image.Picking);
			CHECK(editor.GetViewportState().GetPixelSize(ViewportView::Scene).value() == glm::uvec2(160, 90));
			// A hidden view stays hidden: no area, no render.
			harness.GetHost().SetRectangle(ViewportView::Scene, {});
			REQUIRE(harness.Frame(2).has_value());
			CHECK(harness.GetHost().GetImage(ViewportView::Scene).Texture == 0);
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
