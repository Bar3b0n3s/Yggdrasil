#include "TestsPCH.h"

#include "Editor/EditorLayer.h"
#include "Editor/EditorPanelContext.h"
#include "Editor/Private/EditorHostThumbnails.h"
#include "Editor/Private/EditorHostViewports.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EngineAssetGenerators.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/EngineAssetBaker.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/ImGui/ImGuiScreenshot.h"
#include "Engine/Platform/Window.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/GoldenImage.h"
#include "Support/HeadlessGpuFixture.h"

#include <doctest/doctest.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace {

		// These services are the production EditorApp composition over the fixture's single borrowed device.
		// Member order keeps the views and thumbnail registrations inside the ImGui layer's lifetime.
		struct EditorDesignHarness
		{
			GraphicsDevice& Device; // The test's GPU fixture outlives every service below.
			Test::EditorTestFixture Environment{ "EditorDesign", {}, nullptr, std::nullopt, true };
			std::optional<Window> NativeWindow{};
			Scope<ImGuiLayer> Ui{};
			GpuResourceCache Cache;
			Scope<SceneRendererPipelines> Pipelines{};
			Scope<ViewportCapture> Capture{};
			Test::AutomationTestClient Client{ Environment.GetEditor() };
			EditorActions Actions{ Environment.GetEditor(), Client.GetServer() };
			EditorAutomationControls Controls{ Environment.GetEditor(), Client.GetServer() };
			ReflectedEditController Edits{ Environment.GetEditor() };
			GizmoController Gizmos{ Environment.GetEditor() };
			EditorHostViewports Views{ Environment.GetEditor(), Gizmos };
			EditorHostThumbnails Thumbnails{ Environment.GetEditor(), [this](const RenderSnapshot& snapshot, uint32_t size)
			{
				return Capture->Capture({ .Width = size, .Height = size }, snapshot);
			} };
			EditorPanelContext Panels{ Environment.GetEditor(), Client.GetServer(), Actions, Controls, Views, Edits, Thumbnails.GetCache(), Gizmos };
			EditorLayer Layer{ Panels };
			std::optional<OffscreenTarget> Target{};
			nvrhi::CommandListHandle Command{};
			uint64_t FrameIndex = 0;
			float Scale = 1.0f;
			UUID Selected{};

			explicit EditorDesignHarness(Test::HeadlessGpuFixture& gpu)
				: Device(gpu.GetDevice()), Cache(Device, Environment.GetEditor().GetAssets())
			{
			}

			~EditorDesignHarness()
			{
				Device.WaitForIdle();
				Thumbnails.Reset();
				Views.Retire();
			}

			[[nodiscard]] Status Initialize(Test::HeadlessGpuFixture& gpu, uint32_t width, uint32_t height, float scale)
			{
				Scale = scale;
				ENGINE_TRY_ASSIGN(NativeWindow, Window::Create({ .Title = "Editor design acceptance", .Width = width, .Height = height }));
				ENGINE_TRY_ASSIGN(Ui, ImGuiLayer::Create(*NativeWindow, Device, gpu.GetPipelines(), {}));
				ENGINE_TRY(Utils::LoadEditorFont(Environment.GetEditor().GetVfs()));
				// Null-platform windows have unit framebuffer scale. Applying the editor's absolute DPI style gives
				// 2560x1440 at 2x the same logical space and 15 px Inter type as 1280x720 at 1x.
				Utils::ApplyEditorStyle(scale);
				ENGINE_TRY_ASSIGN(Pipelines, SceneRendererPipelines::Create(Device, gpu.GetPipelines()));
				ENGINE_TRY_ASSIGN(Capture, ViewportCapture::CreateForScenes(Device, *Pipelines, Cache, Environment.GetEditor().GetAssets()));
				ENGINE_TRY(Views.Initialize(Device, *Pipelines, Cache, Ui->GetRenderer()));
				Thumbnails.SetGraphics(Device, Ui->GetRenderer());
				Panels.FindThumbnailTexture = [this](const ThumbnailRequest& request)
				{
					return Thumbnails.FindTexture(request);
				};
				ENGINE_TRY_ASSIGN(Command, Device.CreateCommandList());
				return Resize(width, height);
			}

			[[nodiscard]] Status Resize(uint32_t width, uint32_t height)
			{
				Device.WaitForIdle();
				NativeWindow->SetSize(width, height);
				NativeWindow->PollEvents();
				ENGINE_TRY_ASSIGN(Target, OffscreenTarget::Create(Device, { .Width = width, .Height = height, .DebugName = "EditorDesign" }));
				return {};
			}

			[[nodiscard]] Status Populate(Test::HeadlessGpuFixture& gpu)
			{
				auto& engine = Environment.GetEngine();
				auto& editor = Environment.GetEditor();
				// The CPU editor fixture has no baker. Bake the real template environment into its private cache,
				// through the same first-use path as the host, before an open scene command list could need it.
				ENGINE_TRY_ASSIGN(const auto baker, EnvironmentBaker::Create(Device, gpu.GetPipelines()));
				ImporterRegistry importers;
				RegisterBuiltinImporters(importers);
				ENGINE_TRY_ASSIGN(const auto catalog, BuiltinAssetCatalog::Load(engine.GetVfs()));
				const BuiltinAssetEntry* studio = catalog.Find(BuiltinAssetHandles::StudioEnvironment);
				if (studio == nullptr)
					return MakeError(ErrorCode::NotFound, "The editor design scene needs the built-in Studio environment");
				ENGINE_TRY(GetOrBakeEngineAsset({ .Vfs = &engine.GetVfs(), .Importers = &importers, .Registry = &engine.GetTypeRegistry(), .Jobs = &engine.GetJobSystem(), .EnvironmentBaker = baker.get(), .Generators = GetEngineAssetGenerators() }, *studio));
				ENGINE_TRY(Client.Call("project.create", Json{ { "path", FileSystem::PathToUtf8(Environment.GetProjectRoot("DesignStudio")) }, { "name", "Design Studio" }, { "template", "Basic3D" } }));
				ENGINE_TRY(Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
				const Json ocean{ { "BaseColor", { 0.12f, 0.42f, 0.70f, 1.0f } }, { "Roughness", 0.32f } };
				const Json stone{ { "BaseColor", { 0.70f, 0.53f, 0.35f, 1.0f } }, { "Roughness", 0.75f } };
				ENGINE_TRY(Client.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Ocean.material" }, { "values", ocean } }));
				ENGINE_TRY(Client.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/WarmStone.material" }, { "values", stone } }));
				const Json components{
					{ "Transform", Json{ { "Translation", { 0.0f, 0.75f, 0.0f } }, { "EulerAngles", { 0.0f, 25.0f, 0.0f } }, { "Scale", { 1.5f, 1.5f, 1.5f } } } },
					{ "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Cube" }, { "Materials", { "project://Assets/Materials/Ocean.material" } } } },
					{ "RigidBody", Json{ { "Type", "Dynamic" }, { "Mass", 2.5f } } },
					{ "BoxCollider", Json::object() },
				};
				ENGINE_TRY_ASSIGN(const auto cube, Client.Call("entity.create", Json{ { "name", "Ceramic Cube" }, { "components", components } }));
				ENGINE_TRY_ASSIGN(Selected, JsonReader(cube["entity"]["id"]).ReadUUID());
				ENGINE_TRY(Client.Call("edit.select", Json{ { "entities", { Selected.ToString() } } }));
				ENGINE_TRY(Client.Call("viewport.camera", Json{ { "position", { 5.0f, 3.7f, 6.5f } }, { "target", { 0.0f, 0.5f, 0.0f } } }));
				ENGINE_TRY(Client.Call("scene.save", Json::object()));
				editor.GetAssets().WaitIdle();
				// Warm the environment before the renderer records its first commands. No fallback lighting or placeholders.
				ENGINE_TRY(editor.GetAssets().Load(BuiltinAssetHandles::StudioEnvironment));
				ENGINE_TRY(Thumbnails.GetCache().BindProject(1));
				editor.GetUiState().ResetLayout(true);
				return {};
			}

			[[nodiscard]] Status Frame(bool logText = false)
			{
				// Fixed frames settle docking, dynamic font textures and queued thumbnails. GPU completion is a fence,
				// never a timing assertion; no wall clock or sleeping influences the UI or scene.
				Device.WaitForIdle();
				ENGINE_TRY(Layer.OnSafePoint(static_cast<double>(FrameIndex) / 60.0));
				ENGINE_TRY(Thumbnails.Pump());
				NativeWindow->PollEvents();
				const uint32_t slot = static_cast<uint32_t>(FrameIndex % 2);
				Ui->BeginFrame(1.0 / 60.0, slot);
				Command->open();
				Target->Clear(*Command);
				RenderContext context{ .Device = &Device, .CommandList = Command, .Framebuffer = Target->GetFramebuffer(), .Target = Target->GetColorTexture(), .Width = NativeWindow->GetFramebufferWidth(), .Height = NativeWindow->GetFramebufferHeight(), .FrameSlot = slot, .FrameIndex = FrameIndex };
				const Status scene = Views.Render(context);
				if (logText)
					ImGui::LogToFile(0, FileSystem::PathToUtf8(Environment.GetDirectory() / std::format("EditorDesignFrame{}.txt", FrameIndex)).c_str());
				const Status drawn = Layer.OnImGuiRender();
				if (logText)
					ImGui::LogFinish();
				Ui->EndFrame();
				const Status rendered = Ui->Render(*Command, *Target->GetFramebuffer());
				Command->close();
				const uint64_t submission = Device.ExecuteCommandList(*Command);
				const Status submitted = Views.Submitted(FrameIndex, submission);
				Environment.GetEditor().GetUiState().CompleteFrame();
				++FrameIndex;
				Cache.CollectStale();
				Pipelines->CollectStale(Environment.GetEditor().GetAssets());
				Device.RunGarbageCollection();
				ENGINE_TRY(scene);
				ENGINE_TRY(drawn);
				ENGINE_TRY(rendered);
				return submitted;
			}

			void Settle()
			{
				// More than the two layout frames and the one-item-per-frame thumbnail queue, with a fixed endpoint.
				for (uint32_t frame = 0; frame < 12; ++frame)
				{
					const Status status = Frame();
					REQUIRE_MESSAGE(status.has_value(), status.error().ToString());
				}
				REQUIRE(Views.IsLayoutReady());
			}

			void ClickTab(EditorPanel panel)
			{
				// Read geometry only; tab selection goes through the real input path.
				const ImGuiWindow* window = ImGui::FindWindowByName(Utils::EditorWindowTitle(panel));
				REQUIRE(window != nullptr);
				REQUIRE(window->DockNode != nullptr);
				ImGuiTabBar* tabs = window->DockNode->TabBar;
				REQUIRE(tabs != nullptr);
				const ImGuiTabItem* tab = ImGui::TabBarFindTabByID(tabs, window->TabId);
				REQUIRE(tab != nullptr);
				ImGui::GetIO().AddMousePosEvent(tabs->BarRect.Min.x + tab->Offset + tab->Width * 0.5f, tabs->BarRect.GetCenter().y);
				REQUIRE(Frame());
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				REQUIRE(Frame());
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				REQUIRE(Frame());
				ImGui::GetIO().AddMousePosEvent(-1000.0f, -1000.0f);
				Settle();
			}

			[[nodiscard]] ImGuiWindow* AssetTiles()
			{
				ImGuiWindow* browser = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::ContentBrowser));
				REQUIRE(browser != nullptr);
				const std::string project = FileSystem::PathToUtf8(Environment.GetEditor().GetProject().GetProjectFile());
				const ImGuiID columns = ImHashStr("ContentColumns", 0, browser->GetID(static_cast<int>(FNV1a32(project))));
				const ImGuiID child = ImHashStr("AssetTiles", 0, columns);
				for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
					if (window->Active && window->ParentWindow == browser && window->ChildId == child)
						return window;
				FAIL("The production asset grid is not visible");
				return nullptr;
			}

			void OpenMaterials()
			{
				// Locate the root folder's column from the same public, sorted directory listing as the browser.
				// Read submitted table geometry, then navigate with real mouse events rather than panel state.
				const auto root = VfsPath::Create("project", "Assets");
				REQUIRE(root);
				const auto folders = Environment.GetEditor().GetVfs().List(*root, false);
				REQUIRE(folders);
				int folderIndex = 0;
				bool found = false;
				for (const auto& entry : *folders)
				{
					if (!entry.Info.IsDirectory)
						continue;
					if (entry.Path.GetPath() == "Assets/Materials")
					{
						found = true;
						break;
					}
					++folderIndex;
				}
				REQUIRE(found);
				ImGuiWindow* tiles = AssetTiles();
				const ImGuiTable* grid = ImGui::GetCurrentContext()->Tables.GetByKey(tiles->GetID("AssetGrid"));
				REQUIRE(grid != nullptr);
				REQUIRE(folderIndex < grid->ColumnsCount);
				const auto& column = grid->Columns[folderIndex];
				const ImVec2 position((column.WorkMinX + column.WorkMaxX) * 0.5f, grid->OuterRect.Min.y + grid->RowCellPaddingY + tiles->FontRefSize);
				REQUIRE(tiles->InnerClipRect.Contains(position));
				ImGui::GetIO().AddMousePosEvent(position.x, position.y);
				REQUIRE(Frame());
				for (int click = 0; click < 2; ++click)
				{
					ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
					REQUIRE(Frame());
					ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
					REQUIRE(Frame());
				}
				ImGui::GetIO().AddMousePosEvent(-1000.0f, -1000.0f);
				Settle();
				REQUIRE(Frame(true));
				const auto text = FileSystem::ReadText(Environment.GetDirectory() / std::format("EditorDesignFrame{}.txt", FrameIndex - 1));
				REQUIRE(text);
				CHECK(text->contains("Materials"));
				CHECK(text->contains("2 assets"));
				const auto selection = Environment.GetEditor().GetSelection();
				REQUIRE(selection.size() == 1);
				CHECK(selection.front() == Selected);
			}
		};

	}

	static void CheckEditorDesignWindow(const char* title, bool noHorizontalOverflow)
	{
		const ImGuiWindow* window = ImGui::FindWindowByName(title);
		INFO("window: ", title);
		REQUIRE(window != nullptr);
		REQUIRE(window->Active);
		CHECK_FALSE(window->Hidden);
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		constexpr float Tolerance = 1.0f;
		CHECK(window->Pos.x >= viewport->Pos.x - Tolerance);
		CHECK(window->Pos.y >= viewport->Pos.y - Tolerance);
		CHECK(window->Pos.x + window->Size.x <= viewport->Pos.x + viewport->Size.x + Tolerance);
		CHECK(window->Pos.y + window->Size.y <= viewport->Pos.y + viewport->Size.y + Tolerance);
		if (noHorizontalOverflow)
		{
			CHECK_FALSE(window->ScrollbarX);
			CHECK(window->ScrollMax.x <= Tolerance);
			// CursorMaxPos includes every submitted toolbar item, even when NoScrollbar hides overflow.
			// This detects clipped controls that a scrollbar-only assertion would miss.
			CHECK(window->DC.CursorMaxPos.x <= window->InnerRect.Max.x - window->WindowPadding.x + Tolerance);
		}
	}

	static void CheckEditorDesignLayout()
	{
		// Dock hosts and child windows count too; hidden tabs have no visible rectangle to accept.
		for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
			if (window->Active && !window->Hidden && !window->IsFallbackWindow)
				CheckEditorDesignWindow(window->Name, false);
		for (const EditorPanel panel : { EditorPanel::SceneHierarchy, EditorPanel::Inspector, EditorPanel::SceneViewport, EditorPanel::ContentBrowser })
			CheckEditorDesignWindow(Utils::EditorWindowTitle(panel), true);
		CheckEditorDesignWindow("##EditorToolbar", true);
		CheckEditorDesignWindow("##EditorStatus", true);
		const ImGuiWindow* scene = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
		REQUIRE(scene != nullptr);
		CHECK(scene->DC.CursorMaxPos.y <= scene->InnerRect.Max.y - scene->WindowPadding.y + 1.0f);
	}

	static void CheckEditorDesignGameHint(EditorDesignHarness& test, bool expectOmitted = false)
	{
		REQUIRE(test.Frame(true));
		CheckEditorDesignWindow(Utils::EditorWindowTitle(EditorPanel::GameViewport), true);
		const ImGuiWindow* game = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::GameViewport));
		REQUIRE(game != nullptr);
		const float available = game->WorkRect.GetWidth();
		// EndFrame restores the pre-frame font stack. Measure using the size the Game window actually submitted.
		const float hintWidth = std::ceil(ImGui::GetIO().FontDefault->CalcTextSizeA(game->FontRefSize, std::numeric_limits<float>::max(), 0.0f, "Camera preview - press Play to interact").x);
		const float required = std::min(180.0f * test.Scale, available) + ImGui::GetStyle().ItemSpacing.x
			+ hintWidth;
		const auto text = FileSystem::ReadText(test.Environment.GetDirectory() / std::format("EditorDesignFrame{}.txt", test.FrameIndex - 1));
		REQUIRE(text);
		if (expectOmitted)
			CHECK(required > available);
		CHECK(text->contains("Camera preview - press Play to interact") == (required <= available));
	}

	static void CheckEditorDesignThumbnails(EditorDesignHarness& test)
	{
		const auto& editor = test.Environment.GetEditor();
		const ImGuiWindow* tiles = test.AssetTiles();
		REQUIRE_FALSE(tiles->Hidden);
		for (const char* path : { "Assets/Materials/Ocean.material", "Assets/Materials/WarmStone.material" })
		{
			INFO("material tile: ", std::string_view(path));
			const auto handles = editor.GetAssets().GetRegistry().GetHandles();
			const auto found = std::find_if(handles.begin(), handles.end(), [&editor, path](AssetHandle handle)
			{
				return editor.GetAssets().GetReferencePath(handle) == path;
			});
			REQUIRE(found != handles.end());
			const AssetHandle material = *found;
			const ThumbnailRequest request{ test.Thumbnails.GetCache().GetProjectGeneration(), material, editor.GetAssets().GetVersion(material), 128 };
			const auto thumbnail = test.Thumbnails.GetCache().Find(request);
			REQUIRE(thumbnail);
			REQUIRE(thumbnail->has_value());
			CHECK_FALSE((**thumbnail).TypeIcon);
			const uint64_t texture = test.Thumbnails.FindTexture(request);
			REQUIRE(texture != 0);
			bool visible = false;
			for (const ImDrawCmd& command : tiles->DrawList->CmdBuffer)
			{
				if (command.GetTexID() != texture || command.ElemCount == 0)
					continue;
				const auto firstVertex = command.VtxOffset + tiles->DrawList->IdxBuffer[static_cast<int>(command.IdxOffset)];
				const ImVec2 firstPosition = tiles->DrawList->VtxBuffer[static_cast<int>(firstVertex)].pos;
				ImRect bounds(firstPosition, firstPosition);
				for (unsigned int index = 1; index < command.ElemCount; ++index)
				{
					const auto vertex = command.VtxOffset + tiles->DrawList->IdxBuffer[static_cast<int>(command.IdxOffset + index)];
					bounds.Add(tiles->DrawList->VtxBuffer[static_cast<int>(vertex)].pos);
				}
				const ImRect clip(command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w);
				CHECK(bounds.GetWidth() > 0.0f);
				CHECK(bounds.GetHeight() > 0.0f);
				CHECK(clip.Contains(bounds));
				CHECK(tiles->InnerClipRect.Contains(bounds));
				visible = true;
			}
			CHECK(visible);
		}
	}

	static void CaptureEditorDesign(uint32_t width, uint32_t height, float scale, const char* goldenName, bool launcher = false)
	{
		Test::HeadlessGpuFixture gpu;
		ENGINE_REQUIRE_GPU(gpu);
		EditorDesignHarness test(gpu);
		const Status initialized = test.Initialize(gpu, width, height, scale);
		REQUIRE_MESSAGE(initialized.has_value(), initialized.error().ToString());
		if (!launcher)
		{
			const Status populated = test.Populate(gpu);
			REQUIRE_MESSAGE(populated.has_value(), populated.error().ToString());
			const Entity selected = test.Environment.GetEditor().GetScene().FindEntityByID(test.Selected);
			REQUIRE(selected);
			CHECK(selected.HasComponent<TransformComponent>());
			CHECK(selected.HasComponent<MeshRendererComponent>());
			CHECK(selected.HasComponent<RigidBodyComponent>());
			CHECK(selected.HasComponent<BoxColliderComponent>());
			const auto selection = test.Environment.GetEditor().GetSelection();
			REQUIRE(selection.size() == 1);
			CHECK(selection.front() == test.Selected);
		}
		test.Settle();
		if (!launcher)
			test.OpenMaterials();
		// FontRefSize records GetFontSize() inside this production window's Begin(). EndFrame calls PopFont()
		// and restores the unscaled pre-frame size, which says nothing about the font used in the screenshot.
		const ImGuiWindow* fontWindow = ImGui::FindWindowByName(Utils::EditorWindowTitle(launcher ? EditorPanel::ProjectLauncher : EditorPanel::SceneViewport));
		REQUIRE(fontWindow != nullptr);
		CHECK(fontWindow->FontRefSize == doctest::Approx(15.0f * scale));
		REQUIRE(ImGui::GetIO().FontDefault != nullptr);
		CHECK(std::string_view(ImGui::GetIO().FontDefault->GetDebugName()) == "Inter Regular");
		if (launcher)
			CheckEditorDesignWindow(Utils::EditorWindowTitle(EditorPanel::ProjectLauncher), true);
		else
		{
			CheckEditorDesignLayout();
			const auto& editor = test.Environment.GetEditor();
			const ConstEntity selected = editor.GetScene().FindEntityByID(test.Selected);
			REQUIRE(selected);
			const auto& materials = selected.GetComponent<MeshRendererComponent>().Materials;
			REQUIRE(materials.size() == 1);
			const AssetHandle material = materials.front().GetHandle();
			CHECK(editor.GetAssets().GetReferencePath(material).find("Ocean.material") != std::string::npos);
			CheckEditorDesignThumbnails(test);
			const EditorViewportImage scene = test.Views.GetImage(ViewportView::Scene);
			CHECK(scene.Texture != 0);
			CHECK(scene.Width > 0);
			CHECK(scene.Height > 0);
			CHECK(scene.SceneRevision == test.Environment.GetEditor().GetScene().GetRevision());
		}
		const auto image = CaptureImGuiScreenshot(test.Device, *test.Ui, {});
		REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
		REQUIRE(image->IsValid());
		CHECK(image->Width == width);
		CHECK(image->Height == height);
		ENGINE_CHECK_GOLDEN(goldenName, *image, test.Device.GetInfo().DeviceClass);
		if (!launcher)
		{
			test.ClickTab(EditorPanel::GameViewport);
			CheckEditorDesignGameHint(test);
			if (width == 1280 && scale == 1.0f)
			{
				// Exercise the omitted-hint branch through a real window resize after capturing the default layout.
				REQUIRE(test.Resize(800, height));
				test.Settle();
				CheckEditorDesignGameHint(test, true);
			}
			for (const auto& diagnostic : test.Environment.GetEditor().GetAssets().GetDiagnostics())
				FAIL_CHECK("Asset diagnostic: ", diagnostic.Code, ": ", diagnostic.Message);
		}
	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: EditorDesign selected object fits 1280x720")
		{
			CaptureEditorDesign(1280, 720, 1.0f, "EditorDesignSelected1280x720");
		}

		TEST_CASE("Golden: EditorDesign selected object fits 1600x900")
		{
			CaptureEditorDesign(1600, 900, 1.0f, "EditorDesignSelected1600x900");
		}

		TEST_CASE("Golden: EditorDesign selected object fits 2560x1440 at 2x")
		{
			CaptureEditorDesign(2560, 1440, 2.0f, "EditorDesignSelected2560x1440-2x");
		}

		TEST_CASE("Golden: EditorDesign launcher fits 1280x720")
		{
			CaptureEditorDesign(1280, 720, 1.0f, "EditorDesignLauncher1280x720", true);
		}

		TEST_CASE("Golden: EditorDesign supporting panels fit compact columns at 1600x900")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			EditorDesignHarness test(gpu);
			const Status initialized = test.Initialize(gpu, 1600, 900, 1.0f);
			REQUIRE_MESSAGE(initialized.has_value(), initialized.error().ToString());
			const Status populated = test.Populate(gpu);
			REQUIRE_MESSAGE(populated.has_value(), populated.error().ToString());
			const auto settings = test.Client.Call("project.setSettings", Json{ { "patch", { { "StartScene", "Assets/Scenes/Opening.scene" }, { "Export", { { "BuildScenes", { "Assets/Scenes/Finale.scene" } } } } } } });
			REQUIRE_MESSAGE(settings.has_value(), settings.error().ToString());

			constexpr std::array Panels{ EditorPanel::ProjectSettings, EditorPanel::Console, EditorPanel::Diagnostics, EditorPanel::Stats };
			auto& ui = test.Environment.GetEditor().GetUiState();
			for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
			{
				const auto panel = static_cast<EditorPanel>(index);
				REQUIRE(ui.SetPanelOpen(panel, std::find(Panels.begin(), Panels.end(), panel) != Panels.end()));
			}
			// Load an ordinary saved layout before the first frame. All four production panels remain visible at
			// compact widths, with their real filters, settings drawers, validation results and statistics sample.
			std::string layout;
			for (size_t index = 0; index < Panels.size(); ++index)
				layout += std::format("[Window][{}]\nPos={},85\nSize=390,780\nCollapsed=0\n\n", EditorPanelToString(Panels[index]), 8 + index * 398);
			ImGui::LoadIniSettingsFromMemory(layout.c_str());
			test.Settle();

			// Filter through the real Console input so unrelated process logs cannot change the golden image.
			const ImGuiWindow* console = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::Console));
			REQUIRE(console != nullptr);
			const ImGuiStyle& style = ImGui::GetStyle();
			const float frameHeight = console->FontRefSize + style.FramePadding.y * 2.0f;
			const float filterRows = console->WorkRect.GetWidth() >= console->FontRefSize * 25.0f ? 1.0f : 2.0f;
			const auto click = [&test](ImVec2 position)
			{
				ImGui::GetIO().AddMousePosEvent(position.x, position.y);
				REQUIRE(test.Frame());
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				REQUIRE(test.Frame());
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				REQUIRE(test.Frame());
			};
			click(ImVec2(console->WorkRect.GetCenter().x, console->WorkRect.Min.y + filterRows * (frameHeight + style.ItemSpacing.y) + frameHeight * 0.5f));
			ImGui::GetIO().AddInputCharactersUTF8("Design review:");
			REQUIRE(test.Frame());
			click(ImVec2(console->Pos.x + 100.0f, console->Pos.y + console->TitleBarHeight * 0.5f));
			Log::GetRingBuffer().Append({ .Tick = {}, .Level = LogLevel::Info, .Logger = LogChannel::App, .Message = "Design review: Design Studio contains six scene objects", .File = {}, .EntityId = {}, .ScriptFile = {} });
			Log::GetRingBuffer().Append({ .Tick = {}, .Level = LogLevel::Warn, .Logger = LogChannel::Engine, .Message = "Design review: Two build-scene paths need attention before export", .File = {}, .EntityId = {}, .ScriptFile = {} });
			Log::GetRingBuffer().Append({ .Tick = {}, .Level = LogLevel::Info, .Logger = LogChannel::Script, .Message = "Design review: Select Ceramic Cube to inspect its mesh, material and physics components", .File = {}, .EntityId = test.Selected, .ScriptFile = {} });
			ImGui::GetIO().AddMousePosEvent(-1000.0f, -1000.0f);
			test.Settle();
			REQUIRE(test.Frame(true));
			const auto text = FileSystem::ReadText(test.Environment.GetDirectory() / std::format("EditorDesignFrame{}.txt", test.FrameIndex - 1));
			REQUIRE(text);
			for (const std::string_view expected : { "Changes save automatically", "Start Scene", "3 messages", "Design Studio contains six scene objects", "Two build-scene paths need attention", "Select Ceramic Cube", "Project validation: 2 errors", "Opening.scene", "Finale.scene", "Live sample", "CPU frame", "Physics bodies" })
				CHECK_MESSAGE(text->contains(expected), "Missing supporting-panel content: ", expected);
			for (const EditorPanel panel : Panels)
				CheckEditorDesignWindow(Utils::EditorWindowTitle(panel), true);
			for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
				if (window->Active && !window->Hidden && !window->IsFallbackWindow)
					CheckEditorDesignWindow(window->Name, false);
			const auto image = CaptureImGuiScreenshot(test.Device, *test.Ui, {});
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 1600);
			CHECK(image->Height == 900);
			ENGINE_CHECK_GOLDEN("EditorDesignSupporting1600x900", *image, test.Device.GetInfo().DeviceClass);
		}
	}

}
