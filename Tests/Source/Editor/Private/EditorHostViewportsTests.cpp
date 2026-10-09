#include "TestsPCH.h"

#include "Editor/Private/EditorHostViewports.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
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
			CHECK(game->Flags == RenderViewFlags::Picking);
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
