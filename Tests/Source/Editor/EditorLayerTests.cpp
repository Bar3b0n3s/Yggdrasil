#include "TestsPCH.h"
#include "Editor/EditorLayer.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/Private/EditorHostRecovery.h"
#include "Editor/Private/EditorHostThumbnails.h"
#include "Editor/Private/EditorHostViewports.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/App/Application.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/ImGui/ImGuiScreenshot.h"
#include "Engine/Platform/ProjectLock.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
#include "Support/WindowedChild.h"

#include <doctest/doctest.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>

#include <algorithm>
#include <functional>
#include <optional>

namespace Engine {

	static Result<Json> CompleteLayerTestAction(EditorActions& actions, std::string_view method, const Json& params)
	{
		ENGINE_TRY_ASSIGN(uint64_t ticket, actions.Submit(method, params));
		actions.Pump();
		ENGINE_TRY_ASSIGN(auto result, actions.TakeResult(ticket));
		REQUIRE(result.has_value());
		return std::move(*result);
	}

	// Concrete production services; only the frame clock and user input are driven by the test.
	struct LayerUiHarness
	{
		Test::PanelInteractionUi Ui{ false };
		Test::EditorTestFixture Environment{ "LayerInteraction" };
		Test::AutomationTestClient Client{ Environment.GetEditor() };
		EditorActions Actions{ Environment.GetEditor(), Client.GetServer() };
		EditorAutomationControls Controls{ Environment.GetEditor(), Client.GetServer() };
		ReflectedEditController Edits{ Environment.GetEditor() };
		GizmoController Gizmos{ Environment.GetEditor() };
		EditorHostViewports Views{ Environment.GetEditor(), Gizmos };
		EditorHostThumbnails Thumbnails{ Environment.GetEditor(), {} };
		EditorPanelContext Panels{ Environment.GetEditor(), Client.GetServer(), Actions, Controls, Views, Edits, Thumbnails.GetCache(), Gizmos };
		EditorLayer Layer{ Panels };
		uint64_t Frame = 0;
		EditorAutomationPreferenceState Preference{};
		std::optional<bool> PendingPreference{};

		LayerUiHarness()
		{
			Environment.CreateAndOpenProject();
			Environment.CreateAndOpenScene();
			REQUIRE(SafePoint());
			Environment.GetEditor().GetUiState().ResetLayout(true);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			ImGui::GetIO().DisplaySize = ImVec2(1200, 900);
			const auto preferences = ReadEditorPreferences(Environment.GetEditor().GetVfs());
			REQUIRE(preferences);
			Preference.Allowed = preferences->AllowAiAutomation;
			Panels.AutomationPreferences.GetState = [this]()
			{
				return Preference;
			};
			Panels.AutomationPreferences.QueueChange = [this](bool allowed) -> Status
			{
				if (PendingPreference)
					return MakeError(ErrorCode::InvalidState, "preference is pending");
				PendingPreference = allowed;
				Preference.Pending = true;
				return {};
			};
		}

		void OnlyPanel(EditorPanel panel)
		{
			for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
				REQUIRE(Environment.GetEditor().GetUiState().SetPanelOpen(static_cast<EditorPanel>(index), static_cast<EditorPanel>(index) == panel));
		}

		Status SafePoint()
		{
			if (PendingPreference)
			{
				const bool allowed = *PendingPreference;
				const auto changed = Controls.SetAllowAiAutomation(allowed);
				PendingPreference.reset();
				Preference.Pending = false;
				if (!changed)
				{
					Preference.Failure = changed.error();
					return changed;
				}
				Preference.Allowed = allowed;
				Preference.Failure.reset();
			}
			return Layer.OnSafePoint(0);
		}

		std::string Draw()
		{
			const auto file = Environment.GetDirectory() / std::format("LayerFrame{}.txt", Frame++);
			const auto status = Ui.Frame([this, &file]() -> Status
			{
				ImGui::LogToFile(64, FileSystem::PathToUtf8(file).c_str());
				const auto drawn = Layer.OnImGuiRender();
				ImGui::LogFinish();
				return drawn;
			});
			REQUIRE_MESSAGE(status.has_value(), status.error().ToString());
			const auto text = FileSystem::ReadText(file);
			REQUIRE(text);
			return *text;
		}

		void Click(ImVec2 position)
		{
			ImGui::GetIO().AddMousePosEvent(position.x, position.y);
			Draw();
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			Draw();
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			Draw();
		}

		bool IsVisible(const char* name)
		{
			bool visible = false;
			REQUIRE(Ui.Frame([this, name, &visible]() -> Status
			{
				ENGINE_TRY(Layer.OnImGuiRender());
				visible = ImGui::Begin(name);
				ImGui::End();
				return {};
			}));
			return visible;
		}

		void ClickTab(const char* name)
		{
			// Read widget geometry only; selection still goes through real ImGui mouse events.
			const ImGuiWindow* window = ImGui::FindWindowByName(name);
			REQUIRE(window != nullptr);
			REQUIRE(window->DockNode != nullptr);
			ImGuiTabBar* tabs = window->DockNode->TabBar;
			REQUIRE(tabs != nullptr);
			const ImGuiTabItem* tab = ImGui::TabBarFindTabByID(tabs, window->TabId);
			REQUIRE(tab != nullptr);
			Click({ tabs->BarRect.Min.x + tab->Offset + tab->Width / 2, tabs->BarRect.GetCenter().y });
		}
	};

	struct LayerRecoveryFile
	{
		std::filesystem::path Path{};
		Buffer Bytes{};
		uint64_t ModificationTime = 0;
		bool operator==(const LayerRecoveryFile&) const = default;
	};

	struct LayerRecoveryHarness
	{
		LayerUiHarness Ui{};
		Autosave Saves{ Ui.Environment.GetEditor() };
		EditorHostRecovery Host{ Ui.Environment.GetEditor(), Saves };
		std::filesystem::path ProjectFile{};
		UUID SourceEntity{};
		UUID RecoveredEntity{};
		uint32_t QueuedDecisions = 0;
		uint32_t CloseCalls = 0;

		LayerRecoveryHarness()
		{
			auto& editor = Ui.Environment.GetEditor();
			ProjectFile = editor.GetProject().GetProjectFile();
			const auto source = Ui.Client.Call("entity.create", Json{ { "name", "Saved source entity" } });
			REQUIRE(source);
			const auto sourceId = JsonReader((*source)["entity"]["id"]).ReadUUID();
			REQUIRE(sourceId);
			SourceEntity = *sourceId;
			REQUIRE(Ui.Client.Call("scene.save", Json::object()));
			REQUIRE(Saves.Publish());
			const auto created = Ui.Client.Call("entity.create", Json{ { "name", "Recovered from disk" } });
			REQUIRE(created);
			const auto id = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(id);
			RecoveredEntity = *id;
			const auto saved = Saves.Save(AutosaveReason::Periodic);
			REQUIRE(saved);
			REQUIRE(saved->Written);
			REQUIRE(Saves.Reset());
			REQUIRE(editor.CloseProject());
			REQUIRE(Ui.SafePoint());
			Ui.OnlyPanel(EditorPanel::ProjectLauncher);
			Ui.Draw();
			CHECK(Ui.Draw().contains("Recent projects"));
			OpenOriginalProject();
			Ui.Panels.Recovery.GetOffer = [this]()
			{
				return Host.GetOffer();
			};
			Ui.Panels.Recovery.QueueDecision = [this](const EditorRecoveryOffer& offer, EditorRecoveryDecision decision) -> Status
			{
				++QueuedDecisions; // observe the real UI boundary without replacing its decision implementation
				return Host.QueueDecision(offer, decision);
			};
			editor.SetLifecycleCallbacks({ .BeforeProjectClose = [this]() -> Status
			{
				++CloseCalls;
				return {};
			} });
		}

		~LayerRecoveryHarness()
		{
			Ui.Panels.Recovery = {};
			Ui.Environment.GetEditor().SetLifecycleCallbacks({});
			Host.Reset();
			CHECK(Saves.Reset());
		}

		void OpenOriginalProject()
		{
			auto& editor = Ui.Environment.GetEditor();
			auto project = ProjectManager::OpenProject(ProjectFile, {}, editor.GetTypeRegistry());
			REQUIRE(project);
			REQUIRE(editor.OpenProject(std::move(*project)));
			REQUIRE(Ui.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			REQUIRE(Ui.SafePoint());
			CHECK(std::ranges::find(editor.GetUiState().GetOpenPanels(), EditorPanel::ProjectLauncher) == editor.GetUiState().GetOpenPanels().end());
			Ui.OnlyPanel(EditorPanel::SceneHierarchy);
			REQUIRE(Saves.Publish());
		}

		void CheckLock()
		{
			const auto lock = ProjectLock::Acquire(Ui.Environment.GetEditor().GetProject().GetRoot() / ProjectManager::LockFilePath);
			REQUIRE_FALSE(lock);
			CHECK(lock.error().GetCode() == ErrorCode::AlreadyExists);
		}

		std::string SceneText()
		{
			const auto scene = SceneSerializer::SaveToString(Ui.Environment.GetEditor().GetScene());
			REQUIRE(scene);
			return *scene;
		}

		Json History()
		{
			const auto history = Ui.Client.Call("edit.history", Json::object());
			REQUIRE(history);
			return *history;
		}

		std::vector<LayerRecoveryFile> DurableFiles(const std::filesystem::path& projectFile = {})
		{
			std::vector<LayerRecoveryFile> result;
			const auto& source = projectFile.empty() ? ProjectFile : projectFile;
			std::vector<std::filesystem::path> paths{ source };
			for (const char* relative : { "Assets", "Library/Autosave" })
			{
				const auto entries = FileSystem::ListDirectory(source.parent_path() / relative, true);
				if (!entries && entries.error().GetCode() == ErrorCode::NotFound)
					continue;
				REQUIRE(entries);
				paths.insert(paths.end(), entries->begin(), entries->end());
			}
			std::ranges::sort(paths);
			for (const auto& path : paths)
			{
				const auto info = FileSystem::GetInfo(path);
				REQUIRE(info);
				if (info->IsDirectory)
					continue;
				const auto bytes = FileSystem::ReadFile(path);
				REQUIRE(bytes);
				result.push_back({ path, *bytes, info->ModificationTime });
			}
			return result;
		}

		void ClickDecision(EditorRecoveryDecision decision)
		{
			const std::string drawn = Ui.Draw();
			CHECK(drawn.contains("An unsaved version of Main is available"));
			Ui.Draw();
			const auto* modal = ImGui::FindWindowByName("Recover unsaved scene");
			REQUIRE(modal != nullptr);
			REQUIRE(modal->Active);
			const float recoverWidth = ImGui::CalcTextSize("Recover").x + 2 * ImGui::GetStyle().FramePadding.x;
			const float keepWidth = ImGui::CalcTextSize("Keep current scene").x + 2 * ImGui::GetStyle().FramePadding.x;
			const float x = modal->WorkRect.Min.x + (decision == EditorRecoveryDecision::Accept ? recoverWidth / 2 : recoverWidth + ImGui::GetStyle().ItemSpacing.x + keepWidth / 2);
			Ui.Click({ x, modal->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() / 2 });
		}

		Status SafePoint()
		{
			ENGINE_TRY(Ui.SafePoint());
			return Host.Pump();
		}

		void CheckModalClosed()
		{
			Ui.Draw();
			Ui.Draw();
			const auto* modal = ImGui::FindWindowByName("Recover unsaved scene");
			REQUIRE(modal != nullptr);
			CHECK_FALSE(modal->Active);
		}
	};

	struct LayerGpuHarness
	{
		LayerUiHarness Ui{};
		GraphicsDevice& Device;
		GpuResourceCache Cache;
		Scope<SceneRendererPipelines> Pipelines{};
		Scope<ImGuiRenderer> Renderer{};
		nvrhi::CommandListHandle Command{};
		UUID Left{};
		UUID Right{};
		uint64_t Frame = 0;
		bool Recording = false;

		explicit LayerGpuHarness(Test::HeadlessGpuFixture& gpu)
			: Device(gpu.GetDevice()), Cache(Device, Ui.Environment.GetEditor().GetAssets())
		{
			auto& editor = Ui.Environment.GetEditor();
			for (const float x : { -1.0f, 1.0f })
			{
				const Entity entity = editor.GetScene().CreateEntity(x < 0 ? "Left" : "Right");
				entity.GetComponent<TransformComponent>().Translation.x = x;
				entity.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
				(x < 0 ? Left : Right) = entity.GetUUID();
			}
			editor.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(editor.GetViewportState().SetCamera({ .Position = { 0, 0, 5 }, .Target = { 0, 0, 0 }, .Projection = RenderProjection::Orthographic, .OrthographicSize = 2 }));
			editor.GetViewportState().SetOptions({ .Grid = false, .Icons = false });
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, true));
			auto pipelines = SceneRendererPipelines::Create(Device, gpu.GetPipelines());
			REQUIRE(pipelines);
			Pipelines = std::move(*pipelines);
			auto renderer = ImGuiRenderer::Create(Device, gpu.GetPipelines(), {});
			REQUIRE(renderer);
			Renderer = std::move(*renderer);
			auto command = Device.CreateCommandList();
			REQUIRE(command);
			Command = std::move(*command);
			REQUIRE(Ui.Views.Initialize(Device, *Pipelines, Cache, *Renderer));
			Ui.Views.SetRectangle(ViewportView::Scene, { .Size = { 96, 96 } });
			Ui.Views.SetRectangle(ViewportView::Game, { .Size = { 64, 64 } });
		}

		~LayerGpuHarness()
		{
			if (Recording)
			{
				Command->close();
				static_cast<void>(Device.ExecuteCommandList(*Command));
			}
			Device.WaitForIdle();
			Command = nullptr;
			Ui.Views.Retire();
			Renderer->DestroyTextures();
		}

		void Begin(uint64_t frame)
		{
			Device.WaitForIdle();
			Frame = frame;
			Renderer->BeginFrame(static_cast<uint32_t>(frame % 2));
			Command->open();
			Recording = true;
			RenderContext context{ .Device = &Device, .CommandList = Command, .FrameIndex = frame };
			const auto rendered = Ui.Views.Render(context);
			REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
		}

		uint64_t Submit()
		{
			Command->close();
			Recording = false;
			const uint64_t submitted = Device.ExecuteCommandList(*Command);
			REQUIRE(Ui.Views.Submitted(Frame, submitted));
			return submitted;
		}

		EditorViewportClick Click(uint64_t sequence, uint32_t x = 24, uint32_t y = 48) const
		{
			const auto image = Ui.Views.GetImage(ViewportView::Scene);
			return { .Pixel = { x, y }, .FrameIndex = image.FrameIndex, .SceneRevision = image.SceneRevision, .Sequence = sequence, .ViewGeneration = image.Generation };
		}

		void Advance(uint64_t frame, std::optional<EditorViewportClick> click = {})
		{
			Begin(frame);
			if (click)
				REQUIRE(Ui.Views.RequestPick(*click));
			Submit();
		}

		void CheckSelection(std::initializer_list<UUID> ids)
		{
			const auto selected = Ui.Environment.GetEditor().GetSelection();
			CHECK(std::ranges::equal(selected, ids));
		}
	};

	struct LayerCaptureServices
	{
		EditorActions Actions;
		EditorAutomationControls Controls;
		ReflectedEditController Edits;
		GizmoController Gizmos;
		EditorHostViewports Views;
		EditorHostThumbnails Thumbnails;
		EditorPanelContext Panels;
		EditorLayer Layer;

		LayerCaptureServices(EditorContext& editor, AutomationServer& server)
			: Actions(editor, server), Controls(editor, server), Edits(editor), Gizmos(editor), Views(editor, Gizmos), Thumbnails(editor, {}), Panels{ editor, server, Actions, Controls, Views, Edits, Thumbnails.GetCache(), Gizmos }, Layer(Panels)
		{
			for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
				REQUIRE(editor.GetUiState().SetPanelOpen(static_cast<EditorPanel>(index), static_cast<EditorPanel>(index) == EditorPanel::SceneHierarchy));
		}
	};

	static ApplicationSpecification LayerCaptureSpecification(bool windowed)
	{
		ApplicationSpecification specification;
		specification.Name = "LayerScreenshotTests";
		specification.Window = windowed ? WindowMode::Windowed : WindowMode::Headless;
		specification.WindowSettings = { .Title = "Editor layer capture", .Width = 900, .Height = 600 };
		specification.Clock = ClockKind::Manual;
		specification.MaxFrames = 12;
		specification.WorkerCount = 0;
		specification.ThrottleHeadless = false;
		specification.EnableImGui = true;
		specification.Audio = AudioEngineSpecification{};
		specification.Graphics.Validation = true;
		specification.Graphics.SynchronizationValidation = true;
		specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
		specification.ExpectNoGpuErrors = true;
		return specification;
	}

	// Drives the real application frame loop, ImGui layer, screenshot method and readback. No cached image provider.
	class LayerCaptureApplication final : public Application
	{
	public:
		LayerCaptureApplication(Test::EditorTestFixture& fixture, bool windowed)
			: Application(LayerCaptureSpecification(windowed)), m_Fixture(fixture), m_Windowed(windowed)
		{
		}
		uint32_t Completed = 0;
		bool CapturedWhileMinimized = false;
	protected:
		Status OnInitialize() override
		{
			auto specification = Test::MakeTestServerSpecification();
			specification.RendererName = "vulkan";
			specification.Screenshots.CompletedUiFrame = [this]()
			{
				return m_Fixture.GetEditor().GetUiState().GetCompletedFrame();
			};
			specification.Screenshots.RequestUiFrame = [this]()
			{
				m_Ui->Layer.RequestFrame();
				REQUIRE(RequestOffscreenUiFrame());
			};
			specification.Screenshots.EditorUi = [this]() -> Result<Image>
			{
				CHECK(m_Fixture.GetEditor().GetUiState().GetCompletedFrame() > m_AdmittedFrame);
				CHECK(m_SubmittedFrame > m_AdmittedFrame);
				if (m_Windowed && Completed == 1)
				{
					CapturedWhileMinimized = GetContext().GetWindow()->IsMinimized();
					CHECK(CapturedWhileMinimized);
				}
				return CaptureImGuiScreenshot(*GetContext().GetGraphicsDevice(), *GetImGuiLayer(), {});
			};
			m_Client = CreateScope<Test::AutomationTestClient>(m_Fixture.GetEditor(), specification);
			m_Ui = CreateScope<LayerCaptureServices>(m_Fixture.GetEditor(), m_Client->GetServer());
			return {};
		}

		void OnSafePoint() override
		{
			REQUIRE(m_Ui->Layer.OnSafePoint(0));
			m_Client->GetServer().Pump();
			const auto responses = m_Client->GetServer().TakeInProcessResponses(m_Client->GetClient());
			for (const Json& response : responses)
			{
				if (!response.contains("id"))
					continue;
				REQUIRE(response["id"] == m_Request);
				REQUIRE_FALSE_MESSAGE(response.contains("error"), response.dump());
				const auto path = JsonReader(response["result"]["path"]).ReadString();
				REQUIRE(path);
				const auto image = ReadPng(FileSystem::PathFromUtf8(*path));
				REQUIRE(image);
				CHECK(image->Width == 900);
				CHECK(image->Height == 600);
				if (Completed == 0)
					m_Baseline = *image;
				else
					CHECK(image->Pixels != m_Baseline.Pixels);
				++Completed;
				m_Request = 0;
			}
			if (Completed == 2)
				RequestExit();
		}

		void OnUpdate(const FrameTime& frame) override
		{
			if (frame.FrameIndex < 2 || m_Request != 0 || Completed >= 2)
				return;
			if (Completed == 1)
			{
				if (m_Windowed)
				{
					GetContext().GetWindow()->Minimize();
					REQUIRE(Test::WaitUntilMinimized(*GetContext().GetWindow(), true));
				}
				REQUIRE(m_Client->Call("entity.create", Json{ { "name", "Fresh hierarchy entry" } }));
			}
			m_AdmittedFrame = m_Fixture.GetEditor().GetUiState().GetCompletedFrame();
			m_Request = m_Client->Submit("editor.screenshot", Json::object());
			m_Client->GetServer().Pump();
			CHECK(m_Ui->Layer.IsFrameRequested());
			const auto responses = m_Client->GetServer().TakeInProcessResponses(m_Client->GetClient());
			for (const Json& response : responses)
				CHECK_FALSE_MESSAGE(response.contains("id"), response.dump()); // a cached synchronous capture fails here
		}

		void OnImGuiRender() override
		{
			REQUIRE(m_Ui->Layer.OnImGuiRender());
		}

		void OnRenderSubmitted(uint64_t /*frameIndex*/, uint64_t submissionId) override
		{
			CHECK(GetContext().GetGraphicsDevice()->GetLastSubmissionID() == submissionId);
			m_Fixture.GetEditor().GetUiState().CompleteFrame();
			m_SubmittedFrame = m_Fixture.GetEditor().GetUiState().GetCompletedFrame();
		}

		void OnShutdown() override
		{
			CHECK(Completed == 2);
			m_Ui.reset();
			m_Client.reset();
		}
	private:
		Test::EditorTestFixture& m_Fixture; // borrowed for Run
		bool m_Windowed = false;
		Scope<Test::AutomationTestClient> m_Client{};
		Scope<LayerCaptureServices> m_Ui{};
		uint64_t m_AdmittedFrame = 0;
		uint64_t m_SubmittedFrame = 0;
		int64_t m_Request = 0;
		Image m_Baseline{};
	};

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorLayer: initial and reset layouts select scene and content tabs and preserve user selections")
		{
			std::string saved;
			{
				LayerUiHarness ui;
				ui.Draw();
				ui.Draw();
				CHECK(ui.IsVisible("SceneViewport"));
				CHECK(ui.IsVisible("ContentBrowser"));
				CHECK_FALSE(ui.IsVisible("GameViewport"));
				CHECK_FALSE(ui.IsVisible("Console"));
				ui.ClickTab("GameViewport");
				ui.ClickTab("Console");
				CHECK(ui.IsVisible("GameViewport"));
				CHECK(ui.IsVisible("Console"));
				ui.Draw();
				CHECK_FALSE(ui.IsVisible("SceneViewport"));
				CHECK_FALSE(ui.IsVisible("ContentBrowser"));
				saved = ImGui::SaveIniSettingsToMemory();
			}
			{
				LayerUiHarness ui;
				ImGui::LoadIniSettingsFromMemory(saved.data(), saved.size());
				ui.Draw();
				ui.Draw();
				CHECK(ui.IsVisible("GameViewport"));
				CHECK(ui.IsVisible("Console"));
				// Open the real Window menu and click its final item, Reset layout.
				const auto& style = ImGui::GetStyle();
				const float x = style.DisplaySafeAreaPadding.x + ImGui::CalcTextSize("File").x + ImGui::CalcTextSize("Edit").x + 2 * style.ItemSpacing.x + ImGui::CalcTextSize("Window").x / 2;
				ui.Click({ x, ImGui::GetFrameHeight() / 2 });
				auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
				REQUIRE_FALSE(popups.empty());
				const ImGuiWindow* menu = popups.back().Window;
				REQUIRE(menu != nullptr);
				ui.Click({ menu->Pos.x + menu->Size.x / 2, menu->DC.CursorPosPrevLine.y + ImGui::GetTextLineHeight() / 2 });
				ui.Draw();
				ui.Draw();
				CHECK(ui.IsVisible("SceneViewport"));
				CHECK(ui.IsVisible("ContentBrowser"));
				CHECK_FALSE(ui.IsVisible("GameViewport"));
				CHECK_FALSE(ui.IsVisible("Console"));
			}
		}

		TEST_CASE("EditorLayer: first thumbnail load invalidates version zero and the next request succeeds")
		{
			LayerUiHarness ui;
			auto& editor = ui.Environment.GetEditor();
			auto& cache = ui.Thumbnails.GetCache();
			REQUIRE(cache.BindProject(1));
			const AssetHandle asset = BuiltinAssetHandles::CheckerTexture;
			REQUIRE(editor.GetAssets().GetVersion(asset) == 0);
			REQUIRE(cache.Queue({ 1, asset, 0, 32 }));
			REQUIRE(ui.Layer.OnSafePoint(0));
			CHECK(editor.GetAssets().GetVersion(asset) == 1);
			const auto stale = cache.Find({ 1, asset, 0, 32 });
			REQUIRE_FALSE(stale);
			CHECK(stale.error().GetCode() == ErrorCode::Conflict);
			const ThumbnailRequest current{ 1, asset, 1, 32 };
			CHECK_FALSE(cache.Find(current)->has_value());
			REQUIRE(cache.Queue(current));
			REQUIRE(ui.Layer.OnSafePoint(1));
			const auto ready = cache.Find(current);
			REQUIRE(ready);
			REQUIRE(ready->has_value());
			const auto image = ReadPng(FileSystem::PathFromUtf8((**ready).Path));
			REQUIRE(image);
			CHECK(image->Width == 32);
			CHECK(image->Pixels[0] == std::byte{ 192 });
			CHECK(image->Pixels[16] == std::byte{ 128 });
		}

		TEST_CASE("EditorLayer: every architecture panel is available in the dockspace")
		{
			LayerUiHarness ui;
			for (uint8_t index = 0; index < static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
			{
				const auto panel = static_cast<EditorPanel>(index);
				ui.OnlyPanel(panel);
				ui.Draw();
				ui.Draw();
				const std::string settings = ImGui::SaveIniSettingsToMemory();
				CHECK(settings.contains("[Window][" + std::string(EditorPanelToString(panel)) + "]"));
			}
			CHECK(std::string(ImGui::SaveIniSettingsToMemory()).contains("[Docking][Data]"));
			REQUIRE(ui.Environment.GetEditor().CloseProject());
			REQUIRE(ui.Layer.OnSafePoint(0));
			ui.Draw(); // the launcher's first Begin measures its contents
			CHECK(ui.Draw().contains("Recent projects"));
		}
		TEST_CASE("EditorLayer: scene changes on disk offer reload without silently discarding edits")
		{
			LayerUiHarness ui;
			ui.OnlyPanel(EditorPanel::SceneHierarchy);
			auto& editor = ui.Environment.GetEditor();
			REQUIRE(ui.Client.Call("entity.create", Json{ { "name", "Unsaved" } }));
			const uint64_t revision = editor.GetRevision();
			auto external = editor.CreateScene("External");
			const UUID externalId = external->CreateEntity("FromDisk").GetUUID();
			const auto serialized = SceneSerializer::SaveToString(*external);
			REQUIRE(serialized);
			const auto file = editor.GetProject().GetRoot() / "Assets/Scenes/Main.scene";
			REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(serialized->data(), serialized->size()))));
			REQUIRE(ui.Client.Call("project.refreshAssets", Json::object()));
			REQUIRE(editor.IsSceneChangedOnDisk());
			CHECK(ui.Draw().contains("Reload discards unsaved scene edits"));
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.IsSceneDirty());
			CHECK_FALSE(editor.GetScene().FindEntityByID(externalId));
			ImVec2 reload;
			REQUIRE(ui.Ui.Frame([&ui, &reload]() -> Status
			{
				ENGINE_TRY(ui.Layer.OnImGuiRender());
				ImGui::Begin("Notifications");
				const auto cursor = ImGui::GetCursorScreenPos();
				reload = ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x + 35,
					cursor.y - ImGui::GetStyle().ItemSpacing.y - ImGui::GetFrameHeight() / 2);
				ImGui::End();
				return {};
			}));
			ui.Click(reload);
			CHECK(editor.GetRevision() == revision);
			REQUIRE(ui.Layer.OnSafePoint(0));
			CHECK(editor.GetScene().FindEntityByID(externalId).IsValid());
			CHECK_FALSE(editor.IsSceneDirty());
			CHECK_FALSE(editor.IsSceneChangedOnDisk());
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}
		TEST_CASE("EditorLayer: the game panel shows the lockstep owner and releases input on blur" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Ui.OnlyPanel(EditorPanel::GameViewport);
			REQUIRE(test.Ui.Client.Call("play.start", Json{ { "lockstep", true } }));
			auto& session = *test.Ui.Environment.GetEditor().GetPlay().GetSession();
			test.Advance(0);
			test.Ui.Draw();
			test.Ui.Draw();
			ImGui::SetWindowFocus("GameViewport");
			const auto* window = ImGui::FindWindowByName("GameViewport");
			REQUIRE(window != nullptr);
			const ImVec2 inside = window->InnerRect.GetCenter();
			ImGui::GetIO().AddMousePosEvent(inside.x, inside.y);
			test.Ui.Draw();
			const std::string drawn = test.Ui.Draw();
			CHECK(drawn.contains(std::format("Agent controls time (owner {})", session.GetLockstepOwner())));
			REQUIRE(test.Ui.Views.IsGameInputFocused());
			REQUIRE(session.GetInput().Queue(session.GetTick(), { .KeyCode = Key::Space }));
			REQUIRE(test.Ui.Client.Call("play.step", Json{ { "ticks", 1 } }));
			CHECK(session.GetInput().GetSummary(InputPhase::Step).Down == std::vector<std::string>{ "Key.Space" });
			ImGui::SetWindowFocus(nullptr);
			ImGui::GetIO().AddMousePosEvent(-100, -100);
			test.Ui.Draw();
			CHECK_FALSE(test.Ui.Views.IsGameInputFocused());
			// Blur queues releases; it must not advance an agent-owned session itself.
			CHECK(session.GetTick() == 1);
			CHECK(session.GetInput().GetSummary(InputPhase::Step).Down == std::vector<std::string>{ "Key.Space" });
			REQUIRE(test.Ui.Client.Call("play.step", Json{ { "ticks", 1 } }));
			CHECK(session.GetInput().GetSummary(InputPhase::Step).Down.empty());
			CHECK(session.GetInput().GetSummary(InputPhase::Step).Released == std::vector<std::string>{ "Key.Space" });
		}

		TEST_CASE("EditorLayer: editor.screenshot waits for a frame constructed after the request" * doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::EditorTestFixture fixture("FreshLayerCapture");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			LayerCaptureApplication application(fixture, false);
			CHECK(application.Run() == ExitCode::Success);
			CHECK(application.Completed == 2);
		}

		TEST_CASE("EditorLayer: minimized screenshot requests produce a fresh offscreen UI frame" * doctest::test_suite(Test::GpuSuite))
		{
			std::vector<std::string> arguments{ "--windowed-child=EditorLayer: native minimized screenshot child" };
			const auto gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const auto child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE(child);
			INFO(child->StandardOutput, child->StandardError);
			CHECK(child->ExitCode == 0);
		}

		TEST_CASE("EditorLayer: scene and game targets retire through ImGui texture registration" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Advance(0);
			const auto scene = test.Ui.Views.GetImage(ViewportView::Scene);
			const auto game = test.Ui.Views.GetImage(ViewportView::Game);
			REQUIRE(scene.Texture != 0);
			REQUIRE(game.Texture != 0);
			CHECK(scene.Texture != game.Texture);
			CHECK(test.Renderer->GetTextureCount() == 2);
			test.Ui.Views.SetRectangle(ViewportView::Scene, { .Size = { 128, 80 } });
			test.Advance(1);
			const auto resized = test.Ui.Views.GetImage(ViewportView::Scene);
			CHECK(resized.Texture != scene.Texture);
			CHECK(resized.Generation != scene.Generation);
			CHECK(test.Ui.Views.GetImage(ViewportView::Game).Texture == game.Texture);
			CHECK(test.Renderer->GetTextureCount() == 3); // replaced image is retained through this slot's submission
			test.Device.WaitForIdle();
			test.Renderer->BeginFrame(0);
			CHECK(test.Renderer->GetTextureCount() == 3);
			test.Renderer->BeginFrame(1);
			CHECK(test.Renderer->GetTextureCount() == 2);
			test.Ui.Views.Retire();
			CHECK(test.Ui.Views.GetImage(ViewportView::Scene).Texture == 0);
			CHECK(test.Ui.Views.GetImage(ViewportView::Game).Texture == 0);
			CHECK(test.Renderer->GetTextureCount() == 2);
			test.Renderer->BeginFrame(0);
			CHECK(test.Renderer->GetTextureCount() == 2);
			test.Renderer->BeginFrame(1);
			CHECK(test.Renderer->GetTextureCount() == 0);
		}
		TEST_CASE("EditorLayer: hierarchy and inspector actions share automation undo behavior")
		{
			// Exercise the exact CPU action/controller boundary used by Draw and OnSafePoint; no Editor.exe symbols.
			Test::AutomationFixture fixture("LayerSharedCommands");
			auto& editor = fixture.GetEditor();
			EditorActions actions(editor, fixture.GetClient().GetServer());
			auto created = CompleteLayerTestAction(actions, "entity.create", Json{ { "name", "Child" } });
			REQUIRE(created);
			auto id = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(id);
			auto parent = fixture.Call("entity.create", Json{ { "name", "Parent" } });
			REQUIRE(parent);
			auto parentId = JsonReader((*parent)["entity"]["id"]).ReadUUID();
			REQUIRE(parentId);
			REQUIRE(CompleteLayerTestAction(actions, "entity.reparent", Json{ { "entity", id->ToString() }, { "parent", parentId->ToString() }, { "keepWorld", true } }));
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { *id }, .Component = "Transform", .FieldPath = "Translation" }));
			REQUIRE(edits.Preview(Value::FromVec3({ 2.0f, 3.0f, 4.0f })));
			REQUIRE(edits.Commit());
			const auto history = editor.GetHistory().GetEntries(10);
			REQUIRE(history.size() == 4);
			CHECK(history[0].Origin == CommandOrigin::User);
			CHECK(history[1].Origin == CommandOrigin::Agent);
			CHECK(history[2].Origin == CommandOrigin::User);
			CHECK(history[3].Origin == CommandOrigin::User);
			REQUIRE(fixture.Call("edit.undo", Json{ { "steps", 2 } }));
			CHECK_FALSE(editor.GetScene().FindEntityByID(*id).GetParent().IsValid());
			auto value = ComponentAccess::GetFieldValue(editor.GetScene().FindEntityByID(*id), "Transform", "Translation");
			REQUIRE(value);
			CHECK(*value == Value::FromVec3({ 0.0f, 0.0f, 0.0f }));
			REQUIRE(CompleteLayerTestAction(actions, "edit.redo", Json{ { "steps", 2 } }));
			CHECK(editor.GetScene().FindEntityByID(*id).GetParent().GetUUID() == *parentId);
			value = ComponentAccess::GetFieldValue(editor.GetScene().FindEntityByID(*id), "Transform", "Translation");
			REQUIRE(value);
			CHECK(*value == Value::FromVec3({ 2.0f, 3.0f, 4.0f }));
		}

		TEST_CASE("EditorLayer: content deletion uses trash and preserves asset handles")
		{
			Test::AutomationFixture fixture("LayerContentTrash");
			auto& editor = fixture.GetEditor();
			EditorActions actions(editor, fixture.GetClient().GetServer());
			auto created = CompleteLayerTestAction(actions, "asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Trash.material" } });
			REQUIRE(created);
			auto id = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(id);
			const VfsPath source = VfsPath::Create("project", "Assets/Materials/Trash.material").value();
			const VfsPath meta = VfsPath::Create("project", "Assets/Materials/Trash.material.meta").value();
			auto bytes = editor.GetVfs().ReadText(source);
			auto metadata = editor.GetVfs().ReadText(meta);
			REQUIRE(bytes);
			REQUIRE(metadata);
			auto ticket = actions.Submit("asset.delete", Json{ { "asset", id->ToString() } });
			REQUIRE(ticket);
			CHECK(editor.GetVfs().Exists(source));
			actions.Pump();
			auto deleted = actions.TakeResult(*ticket);
			REQUIRE(deleted);
			REQUIRE(deleted->has_value());
			REQUIRE(**deleted);
			CHECK_FALSE(editor.GetVfs().Exists(source));
			const std::string trash = "Library/Trash/" + id->ToString() + "/Assets/Materials/Trash.material";
			CHECK(editor.GetVfs().ReadText(VfsPath::Create("project", trash).value()) == *bytes);
			CHECK(editor.GetVfs().ReadText(VfsPath::Create("project", trash + ".meta").value()) == *metadata);
			REQUIRE(fixture.Call("edit.undo", Json::object()));
			auto info = fixture.Call("asset.info", Json{ { "asset", "Assets/Materials/Trash.material" } });
			REQUIRE(info);
			CHECK((*info)["asset"]["id"] == Json(id->ToString()));
			CHECK(editor.GetVfs().ReadText(source) == *bytes);
			CHECK(editor.GetVfs().ReadText(meta) == *metadata);
		}

		TEST_CASE("EditorLayer: accepting recovery uses the injected already-open project service")
		{
			LayerRecoveryHarness test;
			auto& editor = test.Ui.Environment.GetEditor();
			REQUIRE(CompleteLayerTestAction(test.Ui.Actions, "asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Current.material" } }));
			REQUIRE_FALSE(editor.IsSceneDirty());
			const LoadedProject* project = &editor.GetProject();
			const uint64_t revision = editor.GetRevision();
			const auto durable = test.DurableFiles();
			const auto before = test.SceneText();
			REQUIRE(test.Host.Inspect(1));
			const auto offered = test.Host.GetOffer();
			REQUIRE(offered);
			REQUIRE(offered->Id != 0);
			test.CheckLock();
			test.ClickDecision(EditorRecoveryDecision::Accept);
			REQUIRE(test.Host.GetOffer());
			CHECK(test.Host.GetOffer()->DecisionPending);
			CHECK(test.QueuedDecisions == 1);
			CHECK(editor.GetRevision() == revision);
			CHECK(test.SceneText() == before);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			// Both modal buttons stay disabled until the production helper executes at the next safe point.
			test.ClickDecision(EditorRecoveryDecision::Accept);
			test.ClickDecision(EditorRecoveryDecision::Decline);
			CHECK(test.QueuedDecisions == 1);
			test.CheckLock();
			REQUIRE(test.SafePoint());
			CHECK_FALSE(test.Host.GetOffer());
			CHECK(&editor.GetProject() == project);
			CHECK(test.CloseCalls == 0);
			CHECK(editor.GetRevision() > revision);
			CHECK(editor.IsSceneDirty());
			CHECK(editor.GetScene().GetEntityCount() == 2);
			REQUIRE(editor.GetScene().FindEntityByID(test.RecoveredEntity).IsValid());
			CHECK(editor.GetScene().FindEntityByID(test.RecoveredEntity).GetName() == "Recovered from disk");
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			CHECK(editor.GetHistory().GetRedoCount() == 0);
			CHECK(test.DurableFiles() == durable);
			test.CheckLock();
			const uint64_t adopted = editor.GetRevision();
			REQUIRE(test.SafePoint());
			REQUIRE(test.Host.Inspect(1));
			CHECK_FALSE(test.Host.GetOffer());
			CHECK(editor.GetRevision() == adopted); // never installs twice or offers again within this open epoch
			test.CheckModalClosed();
		}

		TEST_CASE("EditorLayer: declining recovery preserves the scene history and durable generation")
		{
			LayerRecoveryHarness test;
			auto& editor = test.Ui.Environment.GetEditor();
			REQUIRE(CompleteLayerTestAction(test.Ui.Actions, "asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Keep.material" } }));
			REQUIRE(editor.SetSelection({ test.SourceEntity }, SceneTarget::Edit));
			REQUIRE(CompleteLayerTestAction(test.Ui.Actions, "asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Redo.material" } }));
			REQUIRE(CompleteLayerTestAction(test.Ui.Actions, "edit.undo", Json::object()));
			// Native asset commands provide real undo/redo branches without dirtying the source scene being offered.
			REQUIRE_FALSE(editor.IsSceneDirty());
			const LoadedProject* project = &editor.GetProject();
			const uint64_t revision = editor.GetRevision();
			const auto scene = test.SceneText();
			const auto history = test.History();
			const auto durable = test.DurableFiles();
			REQUIRE(test.Host.Inspect(1));
			const auto offered = test.Host.GetOffer();
			REQUIRE(offered);
			test.CheckLock();
			test.ClickDecision(EditorRecoveryDecision::Decline);
			REQUIRE(test.Host.GetOffer());
			CHECK(test.Host.GetOffer()->DecisionPending);
			CHECK(test.QueuedDecisions == 1);
			CHECK(editor.GetRevision() == revision);
			REQUIRE(test.SafePoint());
			CHECK_FALSE(test.Host.GetOffer());
			CHECK(&editor.GetProject() == project);
			CHECK(test.CloseCalls == 0);
			CHECK(editor.GetRevision() == revision);
			CHECK(test.SceneText() == scene);
			CHECK(test.History() == history);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK(editor.GetHistory().GetRedoCount() == 1);
			CHECK_FALSE(editor.IsSceneDirty());
			CHECK(std::ranges::equal(editor.GetSelection(), std::vector<UUID>{ test.SourceEntity }));
			CHECK(test.DurableFiles() == durable);
			test.CheckLock();
			REQUIRE(test.Host.Inspect(1));
			CHECK_FALSE(test.Host.GetOffer());
			const auto stillDurable = test.Saves.FindRecovery();
			REQUIRE(stillDurable);
			REQUIRE(stillDurable->has_value());
			CHECK((**stillDurable).Generation == offered->Recovery.Generation);
			test.CheckModalClosed();
		}

		TEST_CASE("EditorLayer: a queued recovery decision cannot overwrite intervening edits or a new project")
		{
			LayerRecoveryHarness test;
			auto& editor = test.Ui.Environment.GetEditor();
			REQUIRE(test.Host.Inspect(1));
			const auto old = test.Host.GetOffer();
			REQUIRE(old);
			const auto durable = test.DurableFiles();
			test.ClickDecision(EditorRecoveryDecision::Accept);
			REQUIRE(test.Host.GetOffer());
			REQUIRE(test.Host.GetOffer()->DecisionPending);
			bool cancelledByReset = false;
			SUBCASE("an intervening human edit preserves its undo history")
			{
				REQUIRE(CompleteLayerTestAction(test.Ui.Actions, "entity.create", Json{ { "name", "Newer user edit" } }));
			}
			SUBCASE("a different project is open before the decision executes")
			{
				REQUIRE(test.Saves.Reset());
				REQUIRE(editor.CloseProject());
				test.Ui.Environment.CreateAndOpenProject("SecondProject");
				test.Ui.Environment.CreateAndOpenScene();
				REQUIRE(CompleteLayerTestAction(test.Ui.Actions, "entity.create", Json{ { "name", "Second project work" } }));
				// Even before the normal host Reset/Inspect synchronization, Pump must reject the old project.
			}
			SUBCASE("the same project path reopens into a new epoch")
			{
				cancelledByReset = true;
				test.Host.Reset();
				REQUIRE(test.Saves.Reset());
				REQUIRE(editor.CloseProject());
				test.OpenOriginalProject();
				REQUIRE(test.Host.Inspect(2));
				const auto current = test.Host.GetOffer();
				REQUIRE(current);
				CHECK(current->Id > old->Id);
				CHECK_FALSE(current->DecisionPending);
				const auto stale = test.Host.QueueDecision(*old, EditorRecoveryDecision::Accept);
				REQUIRE_FALSE(stale);
				CHECK(stale.error().GetCode() == ErrorCode::NotFound);
			}
			const auto scene = test.SceneText();
			const auto history = test.History();
			const uint64_t revision = editor.GetRevision();
			const auto currentDurable = test.DurableFiles(editor.GetProject().GetProjectFile());
			const auto applied = test.SafePoint();
			if (cancelledByReset)
				REQUIRE(applied);
			else
			{
				REQUIRE_FALSE(applied);
				CHECK(applied.error().GetCode() == ErrorCode::Conflict);
				const auto failed = test.Host.GetOffer();
				REQUIRE(failed);
				REQUIRE(failed->Failure);
				CHECK(failed->Failure->GetCode() == ErrorCode::Conflict);
				CHECK_FALSE(failed->DecisionPending);
				CHECK(test.Ui.Draw().contains(failed->Failure->GetMessageText()));
				test.ClickDecision(EditorRecoveryDecision::Decline);
				REQUIRE(test.Host.GetOffer());
				REQUIRE(test.Host.GetOffer()->DecisionPending);
				CHECK_FALSE(test.Host.GetOffer()->Failure);
				CHECK(test.SceneText() == scene);
				CHECK(test.History() == history);
				REQUIRE(test.SafePoint());
				CHECK_FALSE(test.Host.GetOffer());
				test.CheckModalClosed();
			}
			CHECK(editor.GetRevision() == revision);
			CHECK(test.SceneText() == scene);
			CHECK(test.History() == history);
			CHECK(test.DurableFiles() == durable);
			CHECK(test.DurableFiles(editor.GetProject().GetProjectFile()) == currentDurable);
			CHECK(test.QueuedDecisions == (cancelledByReset ? 1 : 2));
			test.CheckLock();
		}

		TEST_CASE("EditorViewportHost: a superseded click cannot change selection" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Begin(0);
			REQUIRE(test.Ui.Views.RequestPick(test.Click(1)));
			test.Submit();
			test.Begin(1);
			REQUIRE(test.Ui.Views.RequestPick(test.Click(2, 72)));
			test.Submit();
			test.Advance(2);
			test.CheckSelection({});
			test.Advance(3);
			test.CheckSelection({ test.Right });
			test.Advance(4);
			test.CheckSelection({ test.Right });
		}
		TEST_CASE("EditorViewportHost: a camera change rejects queued and pending pick results" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Begin(0);
			REQUIRE(test.Ui.Views.RequestPick(test.Click(1)));
			auto camera = test.Ui.Environment.GetEditor().GetViewportState().GetCamera();
			camera.Position.x += 0.2f;
			camera.Target.x += 0.2f;
			SUBCASE("queued before submission")
			{
				REQUIRE(test.Ui.Environment.GetEditor().GetViewportState().SetCamera(camera));
			}
			SUBCASE("pending after submission")
			{
			}
			test.Submit();
			REQUIRE(test.Ui.Environment.GetEditor().GetViewportState().SetCamera(camera));
			test.Advance(1);
			test.Advance(2);
			test.CheckSelection({});
		}
		TEST_CASE("EditorViewportHost: resize rejects an old image while preserving source frame identity" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Begin(30);
			const auto old = test.Click(1);
			bool measuredBeforeClick = false;
			glm::vec2 size{ 192, 192 };
			SUBCASE("changed aspect ratio")
			{
				size = { 128, 64 };
			}
			SUBCASE("proportional resize at pending completion")
			{
			}
			SUBCASE("proportional layout measured before clicking the displayed old image")
			{
				// SceneViewportPanel publishes the new rectangle before queueing a click on its displayed image.
				measuredBeforeClick = true;
				test.Ui.Views.SetRectangle(ViewportView::Scene, { .Size = size });
			}
			if (measuredBeforeClick)
			{
				const auto rejected = test.Ui.Views.RequestPick(old);
				REQUIRE_FALSE(rejected);
				CHECK(rejected.error().GetCode() == ErrorCode::Conflict);
			}
			else
				REQUIRE(test.Ui.Views.RequestPick(old));
			test.Submit();
			if (!measuredBeforeClick)
			{
				test.Advance(31);
				test.CheckSelection({});
				test.Ui.Views.SetRectangle(ViewportView::Scene, { .Size = size });
			}
			// Frame 32 is the first eligible completion. Invalidation must precede Poll even if the camera is unchanged.
			test.Advance(32);
			test.CheckSelection({});
			const auto rejected = test.Ui.Views.RequestPick(old);
			REQUIRE_FALSE(rejected);
			CHECK(rejected.error().GetCode() == ErrorCode::Conflict);
			CHECK(old.FrameIndex == 30);
			CHECK(old.SceneRevision == test.Ui.Environment.GetEditor().GetScene().GetRevision());
			const auto current = test.Ui.Views.GetImage(ViewportView::Scene);
			CHECK(current.FrameIndex == 32);
			CHECK(current.Generation != old.ViewGeneration);
			test.Advance(33);
			test.CheckSelection({});
		}
		TEST_CASE("EditorViewportHost: scene replacement and deleted UUIDs discard pick results" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Begin(0);
			REQUIRE(test.Ui.Views.RequestPick(test.Click(1)));
			test.Submit();
			auto& editor = test.Ui.Environment.GetEditor();
			SUBCASE("the source scene is replaced")
			{
				auto replacement = editor.CreateScene("Replacement");
				const auto reused = replacement->CreateEntity("Different table entry");
				reused.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Materials = {} });
				editor.SetScene(std::move(replacement), {});
			}
			SUBCASE("the hit UUID is deleted")
			{
				editor.GetScene().DestroyEntity(editor.GetScene().FindEntityByID(test.Left));
			}
			test.Advance(1);
			test.Advance(2);
			test.CheckSelection({});
		}
		TEST_CASE("EditorViewportHost: a stale miss never clears a newer selection" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			REQUIRE(test.Ui.Environment.GetEditor().SetSelection({ test.Left }, SceneTarget::Edit));
			test.Begin(0);
			REQUIRE(test.Ui.Views.RequestPick(test.Click(1, 0, 0)));
			test.Submit();
			test.Begin(1);
			REQUIRE(test.Ui.Views.RequestPick(test.Click(2, 72)));
			test.Submit();
			test.Advance(2);
			test.CheckSelection({ test.Left });
			test.Advance(3);
			test.CheckSelection({ test.Right });
			test.Advance(4);
			test.CheckSelection({ test.Right });
		}
		TEST_CASE("EditorViewportHost: ImGuizmo hover and active manipulation suppress picking" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Ui.OnlyPanel(EditorPanel::SceneViewport);
			REQUIRE(test.Ui.Environment.GetEditor().SetSelection({ test.Left, test.Right }, SceneTarget::Edit));
			test.Advance(0);
			test.Ui.Draw();
			ImGui::SetWindowFocus("SceneViewport");
			test.Ui.Draw();
			test.Advance(1);
			test.Ui.Draw();
			const auto image = test.Ui.Views.GetImage(ViewportView::Scene);
			const auto* window = ImGui::FindWindowByName("SceneViewport");
			REQUIRE(window != nullptr);
			const ImVec2 center{ window->DC.CursorPos.x + static_cast<float>(image.Width) / 2,
				window->DC.CursorPos.y - ImGui::GetStyle().ItemSpacing.y - static_cast<float>(image.Height) / 2 };
			ImGui::GetIO().AddMousePosEvent(center.x, center.y);
			test.Ui.Draw(); // ImGuizmo::IsOver uses the previous complete frame's accumulated hotspots
			test.Begin(2);
			test.Ui.Draw();
			REQUIRE(ImGuizmo::IsOver());
			CHECK_FALSE(ImGuizmo::IsUsingAny());
			const auto hovered = test.Submit();
			CHECK(test.Device.GetLastSubmissionID() == hovered);
			// ImGuizmo carries hover across a complete frame. Click away from the hotspot before that clears:
			// IsOver alone must suppress the background pick, even though no manipulation starts.
			ImGui::GetIO().AddMousePosEvent(center.x, center.y + static_cast<float>(image.Height) / 4);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			test.Begin(3);
			test.Ui.Draw();
			REQUIRE(ImGuizmo::IsOver());
			CHECK_FALSE(ImGuizmo::IsUsingAny());
			CHECK_FALSE(test.Ui.Gizmos.IsDragging());
			const auto hoverClick = test.Submit();
			CHECK(test.Device.GetLastSubmissionID() == hoverClick);
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			test.Ui.Draw();
			ImGui::GetIO().AddMousePosEvent(center.x, center.y);
			test.Ui.Draw();
			test.Ui.Draw();
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			test.Begin(4);
			test.Ui.Draw();
			REQUIRE(ImGuizmo::IsUsingAny());
			REQUIRE(test.Ui.Gizmos.IsDragging());
			const auto submitted = test.Submit();
			CHECK(test.Device.GetLastSubmissionID() == submitted); // no picking readback command list
			ImGui::GetIO().AddMousePosEvent(center.x + 12, center.y);
			test.Begin(5);
			test.Ui.Draw();
			CHECK(ImGuizmo::IsUsingAny());
			CHECK(test.Ui.Gizmos.IsDragging());
			const auto dragged = test.Submit();
			CHECK(test.Device.GetLastSubmissionID() == dragged);
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			test.Ui.Draw();
			CHECK_FALSE(test.Ui.Gizmos.IsDragging());
			test.Advance(6);
			test.Advance(7);
			test.CheckSelection({ test.Left, test.Right });
		}

		TEST_CASE("EditorViewportHost: queued clicks drain only against the exact submitted image and table" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Begin(7);
			const auto click = test.Click(1);
			const uint64_t before = test.Device.GetLastSubmissionID();
			REQUIRE(test.Ui.Views.RequestPick(click));
			CHECK(test.Device.GetLastSubmissionID() == before);
			test.CheckSelection({});
			const auto submitted = test.Submit();
			CHECK(test.Device.GetLastSubmissionID() > submitted); // actual one-pixel readback is submitted after the image
			test.Advance(8);
			test.CheckSelection({});
			test.Advance(9);
			test.CheckSelection({ test.Left });
			CHECK(click.FrameIndex == 7);
			CHECK(click.SceneRevision == test.Ui.Environment.GetEditor().GetScene().GetRevision());
			const auto stale = test.Ui.Views.RequestPick(click);
			REQUIRE_FALSE(stale);
			CHECK(stale.error().GetCode() == ErrorCode::Conflict);
		}
		TEST_CASE("EditorViewportHost: forced and skipped UI frames never relabel the rendered picking frame" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Begin(80);
			const auto click = test.Click(1);
			for (uint32_t frame = 0; frame < 3; ++frame)
			{
				REQUIRE(test.Ui.Ui.Frame([]() -> Status
				{
					ImGui::Begin("UI without a viewport render");
					ImGui::TextUnformatted("No new source image");
					ImGui::End();
					return {};
				}));
				test.Ui.Environment.GetEditor().GetUiState().CompleteFrame();
			}
			REQUIRE(test.Ui.Views.RequestPick(click));
			test.Submit();
			test.Ui.Layer.RequestFrame();
			CHECK(test.Ui.Layer.IsFrameRequested());
			REQUIRE(test.Ui.Ui.Frame([]() -> Status
			{
				return {};
			}));
			test.Ui.Environment.GetEditor().GetUiState().CompleteFrame();
			CHECK_FALSE(test.Ui.Layer.IsFrameRequested());
			CHECK(test.Ui.Views.GetImage(ViewportView::Scene).FrameIndex == 80);
			test.Advance(81);
			test.CheckSelection({});
			test.Advance(82);
			test.CheckSelection({ test.Left });
			CHECK(click.FrameIndex == 80);
			CHECK(test.Ui.Environment.GetEditor().GetUiState().GetCompletedFrame() == 4);
		}
	}

	TEST_CASE("EditorLayer: native minimized screenshot child" * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
	{
		if (!Test::ProbeGpuForProcess())
			return;
		Test::EditorTestFixture fixture("MinimizedLayerCapture");
		fixture.CreateAndOpenProject();
		fixture.CreateAndOpenScene();
		LayerCaptureApplication application(fixture, true);
		CHECK(application.Run() == ExitCode::Success);
		CHECK(application.Completed == 2);
		CHECK(application.CapturedWhileMinimized);
	}

}
