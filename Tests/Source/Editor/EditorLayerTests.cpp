#include "TestsPCH.h"
#include "Editor/EditorLayer.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/Private/EditorHostPreferences.h"
#include "Editor/Private/EditorHostRecovery.h"
#include "Editor/Private/EditorHostThumbnails.h"
#include "Editor/Private/EditorHostViewports.h"
#include "Editor/Ui/EditorStyle.h"
#include "Editor/WorkspaceTestUi.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/EditorActions.h"
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
#include <imgui_internal.h>
#include <ImGuizmo.h>

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

	// The text of the frame this call draws: WorkspaceTestUi::Text logs every frame it draws into one file, so this returns
	// what follows `seen`, the length the previous call read.
	static std::string NextFrameText(Test::WorkspaceTestUi& test, size_t& seen)
	{
		const std::string text = test.Text();
		REQUIRE(text.size() >= seen);
		std::string frame = text.substr(seen);
		seen = text.size();
		return frame;
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
		EditorHostPreferences Preferences{}; // the production host preference queue, applied by SafePoint

		LayerUiHarness()
		{
			Environment.CreateAndOpenProject();
			Environment.CreateAndOpenScene();
			REQUIRE(SafePoint());
			Environment.GetEditor().GetUiState().ResetLayout(true);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			ImGui::GetIO().DisplaySize = ImVec2(1200, 900);
			REQUIRE(Preferences.Load(Environment.GetEditor().GetVfs()));
			Preferences.RefreshRecentProjects(Environment.GetEditor().GetVfs());
			Panels.AutomationPreferences.GetState = [this]()
			{
				return Preferences.GetAutomationState();
			};
			Panels.AutomationPreferences.QueueChange = [this](bool allowed)
			{
				return Preferences.QueueAutomationChange(allowed);
			};
			Panels.ExternalEditor.GetState = [this]()
			{
				return Preferences.GetExternalEditorState();
			};
			Panels.ExternalEditor.QueueChange = [this](std::string executable, std::vector<std::string> arguments)
			{
				return Preferences.QueueExternalEditorChange(std::move(executable), std::move(arguments));
			};
			Panels.GetRecentProjects = [this]()
			{
				return Preferences.GetRecentProjects();
			};
		}

		void OnlyPanel(EditorPanel panel)
		{
			for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
				REQUIRE(Environment.GetEditor().GetUiState().SetPanelOpen(static_cast<EditorPanel>(index), static_cast<EditorPanel>(index) == panel));
		}

		Status SafePoint()
		{
			ENGINE_TRY(Preferences.Pump([this](bool allowed)
			{
				return Controls.SetAllowAiAutomation(allowed);
			}));
			ENGINE_TRY(Preferences.PumpExternalEditor(Environment.GetEditor().GetVfs()));
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
			CHECK(Ui.Draw().contains("Build your next world."));
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
			specification.Screenshots.RequestUiFrame = [this]() -> Status
			{
				m_Ui->Layer.RequestFrame();
				return RequestOffscreenUiFrame();
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
				CHECK(ui.IsVisible("Scene###SceneViewport"));
				CHECK(ui.IsVisible("Assets###ContentBrowser"));
				CHECK_FALSE(ui.IsVisible("Game###GameViewport"));
				CHECK_FALSE(ui.IsVisible("Console###Console"));
				ui.ClickTab("Game###GameViewport");
				ui.ClickTab("Console###Console");
				CHECK(ui.IsVisible("Game###GameViewport"));
				CHECK(ui.IsVisible("Console###Console"));
				ui.Draw();
				CHECK_FALSE(ui.IsVisible("Scene###SceneViewport"));
				CHECK_FALSE(ui.IsVisible("Assets###ContentBrowser"));
				saved = ImGui::SaveIniSettingsToMemory();
			}
			{
				LayerUiHarness ui;
				ImGui::LoadIniSettingsFromMemory(saved.data(), saved.size());
				ui.Draw();
				ui.Draw();
				CHECK(ui.IsVisible("Game###GameViewport"));
				CHECK(ui.IsVisible("Console###Console"));
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
				CHECK(ui.IsVisible("Scene###SceneViewport"));
				CHECK(ui.IsVisible("Assets###ContentBrowser"));
				CHECK_FALSE(ui.IsVisible("Game###GameViewport"));
				CHECK_FALSE(ui.IsVisible("Console###Console"));
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
				CHECK(settings.contains("[Window][" + std::string(ImHashSkipUncontributingPrefix(Utils::EditorWindowTitle(panel))) + "]"));
			}
			CHECK(std::string(ImGui::SaveIniSettingsToMemory()).contains("[Docking][Data]"));
			REQUIRE(ui.Environment.GetEditor().CloseProject());
			REQUIRE(ui.Layer.OnSafePoint(0));
			ui.Draw(); // the launcher's first Begin measures its contents
			CHECK(ui.Draw().contains("Build your next world."));
		}

		TEST_CASE("EditorLayer: the viewport windows use their panels' flags and never scroll")
		{
			LayerUiHarness ui;
			ui.Draw();
			ui.Draw();
			// Scene is the selected tab and Game a hidden one: Begin applies the flags either way.
			for (const char* name : { "Scene###SceneViewport", "Game###GameViewport" })
			{
				CAPTURE(name);
				const ImGuiWindow* window = ImGui::FindWindowByName(name);
				REQUIRE(window != nullptr);
				CHECK((window->Flags & ImGuiWindowFlags_NoScrollWithMouse) != 0);
				CHECK((window->Flags & ImGuiWindowFlags_NoScrollbar) != 0);
			}
			const ImGuiWindow* content = ImGui::FindWindowByName("Assets###ContentBrowser");
			REQUIRE(content != nullptr);
			CHECK((content->Flags & (ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) == 0);
		}

		TEST_CASE("EditorLayer: revealing a panel opens it and gives its window focus")
		{
			LayerUiHarness ui;
			ui.Draw();
			ui.Draw();
			EditorUiState& state = ui.Environment.GetEditor().GetUiState();
			// Hide the Content panel behind the Console tab, then close it, as before files are dropped on the window.
			ui.ClickTab("Console###Console");
			REQUIRE(state.SetPanelOpen(EditorPanel::ContentBrowser, false));
			ui.Draw();
			const ImGuiWindow* closed = ImGui::FindWindowByName("Assets###ContentBrowser");
			CHECK((closed == nullptr || !closed->Active)); // the layer no longer submits it
			REQUIRE(ui.Layer.RevealPanel(EditorPanel::ContentBrowser));
			CHECK(std::ranges::find(state.GetOpenPanels(), EditorPanel::ContentBrowser) != state.GetOpenPanels().end());
			ui.Draw();
			ui.Draw();
			CHECK(ui.IsVisible("Assets###ContentBrowser"));
			CHECK_FALSE(ui.IsVisible("Console###Console"));
			const ImGuiWindow* content = ImGui::FindWindowByName("Assets###ContentBrowser");
			REQUIRE(content != nullptr);
			const ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow;
			REQUIRE(focused != nullptr);
			CHECK(focused->RootWindow == content);
			const Status unknown = ui.Layer.RevealPanel(static_cast<EditorPanel>(200));
			REQUIRE_FALSE(unknown);
			CHECK(unknown.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditorLayer: the default workspace reserves chrome and usable dock widths at supported sizes")
		{
			Test::WorkspaceTestUi test;
			float scale = 1.0f;
			SUBCASE("1280 by 720")
			{
			}
			SUBCASE("1600 by 900")
			{
				ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
			}
			SUBCASE("scaled 1920 by 1080")
			{
				scale = 1.5f;
				Utils::ApplyEditorStyle(scale);
				ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
			}
			float layoutScale = 0.0f;
			test.Draw([&layoutScale]()
			{
				// Capture at the same point as EditorLayer: Render() restores the unscaled font afterward.
				layoutScale = Utils::EditorUiScale();
			});
			test.Draw();
			REQUIRE(layoutScale > 0.0f);
			auto* hierarchy = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneHierarchy));
			auto* inspector = test.Window(Utils::EditorWindowTitle(EditorPanel::Inspector));
			auto* scene = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
			auto* assets = test.Window(Utils::EditorWindowTitle(EditorPanel::ContentBrowser));
			auto* toolbar = test.Window("##EditorToolbar");
			auto* tabs = test.Window("##EditorSceneTabs");
			auto* menu = test.Window("##MainMenuBar");
			auto* status = test.Window("##EditorStatus");
			for (const auto panel : test.Environment.GetEditor().GetUiState().GetOpenPanels())
			{
				auto* window = test.Window(Utils::EditorWindowTitle(panel));
				REQUIRE(window->DockNode != nullptr);
				CHECK(window->DockIsActive);
			}
			CHECK(hierarchy->Size.x / layoutScale >= 220.0f);
			CHECK(hierarchy->Size.x / layoutScale <= 235.0f);
			CHECK(inspector->Size.x / layoutScale >= 350.0f);
			CHECK(inspector->Size.x / layoutScale <= 365.0f);
			CHECK(scene->Size.x / scale >= 660.0f);
			CHECK(scene->Size.y / scale >= 340.0f);
			if (ImGui::GetIO().DisplaySize.y / scale >= 900.0f)
				CHECK(assets->Size.y / scale >= 250.0f);
			CHECK(assets->Size.y <= (status->Pos.y - toolbar->Pos.y - toolbar->Size.y) * 0.32f + 2.0f);
			CHECK(scene->Pos.x == doctest::Approx(assets->Pos.x));
			CHECK(scene->Size.x == doctest::Approx(assets->Size.x));
			CHECK(scene->Pos.y >= toolbar->Pos.y + toolbar->Size.y);
			// The scene tabs' row (Docs/Decisions/0031-m20-contract.md decision 20) sits between the toolbar and the dock space.
			CHECK(tabs->Pos.y >= toolbar->Pos.y + toolbar->Size.y);
			CHECK(scene->Pos.y >= tabs->Pos.y + tabs->Size.y);
			CHECK(tabs->Size.x == doctest::Approx(toolbar->Size.x));
			CHECK(toolbar->Pos.y >= menu->Pos.y + menu->Size.y);
			CHECK(assets->Pos.y + assets->Size.y <= status->Pos.y);
			CHECK(assets->DockNode->SelectedTabId == assets->TabId);
			CHECK(scene->DockNode->SelectedTabId == scene->TabId);
			CHECK(toolbar->ScrollMax.x == 0.0f);
			for (const char* label : { "Play", "Simulate", "Pause###PauseResume", "Step", "Stop" })
			{
				const auto rectangle = test.Locate("##EditorToolbar", label);
				CHECK(rectangle.Min.x >= toolbar->InnerRect.Min.x);
				CHECK(rectangle.Max.x <= toolbar->InnerRect.Max.x);
				CHECK(rectangle.Max.y <= toolbar->InnerRect.Max.y);
			}
		}

		TEST_CASE("EditorLayer: serialized legacy layouts preserve window geometry and selected tabs")
		{
			std::string saved;
			ImGuiID sceneDock = 0;
			ImGuiID assetsDock = 0;
			{
				Test::PanelInteractionUi legacy{ false };
				ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
				ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
				const auto draw = [&sceneDock, &assetsDock]() -> Status
				{
					const ImGuiID dock = ImGui::DockSpaceOverViewport();
					if (sceneDock == 0)
					{
						ImGui::DockBuilderRemoveNode(dock);
						ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
						ImGui::DockBuilderSetNodeSize(dock, ImGui::GetIO().DisplaySize);
						assetsDock = ImGui::DockBuilderSplitNode(dock, ImGuiDir_Down, 0.40f, nullptr, &sceneDock);
						ImGui::DockBuilderDockWindow("SceneViewport", sceneDock);
						ImGui::DockBuilderDockWindow("GameViewport", sceneDock);
						ImGui::DockBuilderDockWindow("ContentBrowser", assetsDock);
						ImGui::DockBuilderDockWindow("Console", assetsDock);
						ImGui::DockBuilderFinish(dock);
					}
					for (const char* name : { "SceneViewport", "GameViewport", "ContentBrowser", "Console" })
					{
						ImGui::Begin(name);
						ImGui::End();
					}
					ImGui::SetNextWindowPos(ImVec2(830, 130), ImGuiCond_Once);
					ImGui::SetNextWindowSize(ImVec2(330, 450), ImGuiCond_Once);
					ImGui::Begin("Inspector");
					ImGui::End();
					return {};
				};
				REQUIRE(legacy.Frame(draw));
				REQUIRE(legacy.Frame(draw));
				ImGui::SetWindowFocus("GameViewport");
				REQUIRE(legacy.Frame(draw));
				ImGui::SetWindowFocus("Console");
				REQUIRE(legacy.Frame(draw));
				saved = ImGui::SaveIniSettingsToMemory();
				REQUIRE(saved.contains("[Window][SceneViewport]"));
			}
			std::string resaved;
			{
				Test::WorkspaceTestUi test;
				REQUIRE(test.Environment.GetEditor().GetUiState().SetPanelOpen(EditorPanel::SceneHierarchy, false));
				ImGui::LoadIniSettingsFromMemory(saved.data(), saved.size());
				test.Draw();
				test.Draw();
				auto* scene = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
				auto* game = test.Window(Utils::EditorWindowTitle(EditorPanel::GameViewport));
				auto* console = test.Window(Utils::EditorWindowTitle(EditorPanel::Console));
				auto* inspector = test.Window(Utils::EditorWindowTitle(EditorPanel::Inspector));
				CHECK(scene->ID == ImHashStr(Utils::EditorWindowTitle(EditorPanel::SceneViewport)));
				CHECK(scene->ID == ImHashStr("SceneViewport")); // Pinned ImGui 1.92.6+ skips the ### bytes.
				CHECK(scene->DockId == sceneDock);
				CHECK(console->DockId == assetsDock);
				CHECK(game->DockNode->SelectedTabId == game->TabId);
				CHECK(console->DockNode->SelectedTabId == console->TabId);
				CHECK(inspector->DockId == 0);
				CHECK(inspector->Pos.x == 830);
				CHECK(inspector->Pos.y == 130);
				CHECK(inspector->Size.x == 330);
				CHECK(inspector->Size.y == 450);
				auto* tabs = scene->DockNode->TabBar;
				const auto* tab = ImGui::TabBarFindTabByID(tabs, scene->TabId);
				REQUIRE(tab != nullptr);
				test.Click(ImVec2(tabs->BarRect.Min.x + tab->Offset + tab->Width * 0.5f, tabs->BarRect.GetCenter().y));
				test.Draw();
				CHECK(scene->DockNode->SelectedTabId == scene->TabId);
				resaved = ImGui::SaveIniSettingsToMemory();
				CHECK(resaved.contains("[Window][" + std::string(ImHashSkipUncontributingPrefix(Utils::EditorWindowTitle(EditorPanel::SceneViewport))) + "]"));
			}
			{
				Test::WorkspaceTestUi test;
				ImGui::LoadIniSettingsFromMemory(resaved.data(), resaved.size());
				test.Draw();
				test.Draw();
				auto* scene = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
				CHECK(scene->DockId == sceneDock);
				CHECK(scene->DockNode->SelectedTabId == scene->TabId);
				CHECK(test.Window(Utils::EditorWindowTitle(EditorPanel::Inspector))->DockId == 0);
			}
		}

		TEST_CASE("EditorLayer: switching from the launcher to a fresh project restores every default dock")
		{
			Test::WorkspaceTestUi test(false);
			test.Draw();
			test.Draw();
			CHECK(test.Text().contains("Build your next world."));
			// Without a project there are no scenes, so the launcher has no scene tabs' row.
			const ImGuiWindow* launcherTabs = ImGui::FindWindowByName("##EditorSceneTabs");
			CHECK((launcherTabs == nullptr || !launcherTabs->Active));
			test.Environment.CreateAndOpenProject();
			test.Environment.CreateAndOpenScene();
			REQUIRE(test.Layer.OnSafePoint(0));
			// ImGuiLayer::SetIniFilePath clears the launcher's ini when the project is bound, even when its file is new.
			test.IniFile = FileSystem::PathToUtf8(test.Environment.GetDirectory() / "FreshProjectLayout.ini");
			ImGui::GetIO().IniFilename = test.IniFile.c_str();
			ImGui::ClearIniSettings();
			test.Draw();
			test.Draw();
			auto* scene = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
			auto* game = test.Window(Utils::EditorWindowTitle(EditorPanel::GameViewport));
			auto* assets = test.Window(Utils::EditorWindowTitle(EditorPanel::ContentBrowser));
			auto* console = test.Window(Utils::EditorWindowTitle(EditorPanel::Console));
			auto* hierarchy = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneHierarchy));
			auto* inspector = test.Window(Utils::EditorWindowTitle(EditorPanel::Inspector));
			for (auto* window : { scene, game, assets, console, hierarchy, inspector })
			{
				REQUIRE(window->DockNode != nullptr);
				CHECK(window->DockIsActive);
			}
			CHECK(scene->DockNode == game->DockNode);
			CHECK(assets->DockNode == console->DockNode);
			CHECK(scene->DockNode->SelectedTabId == scene->TabId);
			CHECK(assets->DockNode->SelectedTabId == assets->TabId);
			CHECK(scene->DockTabIsVisible);
			CHECK_FALSE(game->DockTabIsVisible);
			CHECK(assets->DockTabIsVisible);
			CHECK_FALSE(console->DockTabIsVisible);
			CHECK(test.Window("##EditorSceneTabs")->Active);
			CHECK(hierarchy->Pos.x + hierarchy->Size.x <= scene->Pos.x);
			CHECK(scene->Pos.x + scene->Size.x <= inspector->Pos.x);
			CHECK(scene->Pos.y + scene->Size.y <= assets->Pos.y);
			CHECK(scene->Size.x >= 660.0f);
		}

		TEST_CASE("EditorLayer: the scene browser selects project scenes and requires an explicit unsaved-edit choice")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			test.Environment.CreateAndOpenScene("Assets/Scenes/Another level.scene");
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Keep this edit" } }));
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(false);
			test.Click("Open scene", "##SceneSearch");
			ImGui::GetIO().AddInputCharactersUTF8("ANOTHER");
			test.Draw();
			test.SelectScene("Assets/Scenes/Another level.scene");
			test.Click("Open scene", "Open scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScenePath()->GetFileName() == "Main.scene");
			CHECK(editor.IsSceneDirty());
			test.Click("Open scene", "Save changes first");
			test.Click("Open scene", "Open scene");
			CHECK(editor.GetScenePath()->GetFileName() == "Main.scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScenePath()->GetFileName() == "Another level.scene");
			CHECK_FALSE(editor.IsSceneDirty());
			test.Draw();
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			CHECK(editor.GetScene().GetEntityCount() == 1);
		}

		TEST_CASE("EditorLayer: the scene browser opens scenes outside Assets with distinct literal path identities")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			for (const char* path : { "Levels/Main.scene", "Levels###Shared/Main.scene", "Other###Shared/Main.scene", "Main.scene" })
				test.Environment.CreateAndOpenScene(path);
			std::string path;
			SUBCASE("project level folder")
			{
				path = "Levels/Main.scene";
			}
			SUBCASE("first authored hash folder")
			{
				path = "Levels###Shared/Main.scene";
			}
			SUBCASE("second authored hash folder")
			{
				path = "Other###Shared/Main.scene";
			}
			SUBCASE("project root")
			{
				path = "Main.scene";
			}
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(false);
			if (path.contains("###"))
			{
				test.ReplaceText("Open scene", "##SceneSearch", "###Shared");
				CHECK(test.FindText("Levels###Shared").has_value());
				CHECK(test.FindText("Other###Shared").has_value());
			}
			test.SelectScene(path);
			test.Click("Open scene", "Open scene");
			CHECK(editor.GetScenePath()->GetPath() == "Assets/Scenes/Main.scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScenePath()->GetPath() == path);
			CHECK_FALSE(editor.IsSceneDirty());
		}

		TEST_CASE("EditorLayer: new scene folders display authored hash text and keep distinct selection identities")
		{
			Test::WorkspaceTestUi test;
			test.Environment.CreateAndOpenScene("A###Shared/Main.scene");
			test.Environment.CreateAndOpenScene("AA###Shared/Main.scene");
			std::string folder;
			SUBCASE("first folder")
			{
				folder = "A###Shared";
			}
			SUBCASE("second folder")
			{
				folder = "AA###Shared";
			}
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(true);
			test.ReplaceText("New scene", "##SceneName", "New level");
			test.Click("New scene", "##SceneFolder");
			const auto choice = test.FindText(folder);
			REQUIRE(choice.has_value());
			test.Click(*choice);
			CHECK(test.FindText(folder).has_value());
			test.Click("New scene", "Create scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(test.Environment.GetEditor().GetScenePath()->GetPath() == folder + "/New level.scene");
		}

		TEST_CASE("EditorLayer: new scenes use a name and folder and cannot escape the project")
		{
			Test::WorkspaceTestUi test;
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(true);
			test.Click("New scene", "##SceneName");
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
			test.Draw();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			ImGui::GetIO().AddInputCharactersUTF8("../Outside");
			test.Draw();
			test.Click("New scene", "Create scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(test.Environment.GetEditor().GetScenePath()->GetFileName() == "Main.scene");
			test.Click("New scene", "##SceneName");
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
			test.Draw();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			ImGui::GetIO().AddInputCharactersUTF8("First level");
			test.Draw();
			test.Click("New scene", "Create scene");
			CHECK(test.Environment.GetEditor().GetScenePath()->GetFileName() == "Main.scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(test.Environment.GetEditor().GetScenePath()->GetPath() == "Assets/Scenes/First level.scene");
			CHECK(test.Environment.GetEditor().GetScene().GetName() == "First level");
			CHECK_FALSE(test.Environment.GetEditor().IsSceneDirty());
		}

		TEST_CASE("EditorLayer: toolbar playback queues human commands and respects lockstep ownership")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			test.Draw();
			test.Draw();
			test.Click("##EditorToolbar", "Simulate");
			CHECK(editor.GetPlay().GetSession() == nullptr);
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(editor.GetPlay().GetSession() != nullptr);
			CHECK(editor.GetPlay().GetSession()->GetMode() == PlayMode::Simulate);
			test.Draw();
			test.Click("##EditorToolbar", "Pause###PauseResume");
			const ImGuiID pauseId = ImGui::GetCurrentContext()->NavId;
			CHECK(pauseId == test.Window("##EditorToolbar")->GetID("Pause###PauseResume"));
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetPlay().GetSession()->IsPaused());
			test.Draw();
			CHECK(ImGui::GetCurrentContext()->NavId == pauseId);
			CHECK(ImGui::GetCurrentContext()->NavIdIsAlive);
			test.Click("##EditorToolbar", "Resume###PauseResume");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK_FALSE(editor.GetPlay().GetSession()->IsPaused());
			test.Draw();
			CHECK(ImGui::GetCurrentContext()->NavId == pauseId);
			CHECK(ImGui::GetCurrentContext()->NavIdIsAlive);
			test.Click("##EditorToolbar", "Stop");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetPlay().GetSession() == nullptr);
			REQUIRE(test.Client.Call("play.start", Json{ { "mode", "Simulate" }, { "lockstep", true } }));
			test.Draw();
			test.Click("##EditorToolbar", "Stop");
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(editor.GetPlay().GetSession() != nullptr);
			CHECK(editor.GetPlay().GetSession()->IsLockstep());
			CHECK(test.Text().contains("Agent controls time"));
			REQUIRE(test.Client.Call("play.stop", Json::object()));
		}
		TEST_CASE("EditorLayer: Ctrl+Z, Ctrl+Y and Ctrl+S work from any focused panel and leave text undo to text fields")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Shortcut target" } }));
			REQUIRE(editor.GetScene().GetEntityCount() == 1);
			test.Draw();
			test.Draw();
			const auto press = [&test](const char* panelTitle, ImGuiKey key)
			{
				ImGui::SetWindowFocus(panelTitle);
				test.Draw();
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
				ImGui::GetIO().AddKeyEvent(key, true);
				test.Draw();
				ImGui::GetIO().AddKeyEvent(key, false);
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
				test.Draw();
				REQUIRE(test.Layer.OnSafePoint(0));
			};
			press(Utils::EditorWindowTitle(EditorPanel::Inspector), ImGuiKey_Z);
			CHECK(editor.GetScene().GetEntityCount() == 0);
			press(Utils::EditorWindowTitle(EditorPanel::SceneHierarchy), ImGuiKey_Y);
			CHECK(editor.GetScene().GetEntityCount() == 1);
			REQUIRE(editor.IsSceneDirty());
			press(Utils::EditorWindowTitle(EditorPanel::ContentBrowser), ImGuiKey_S);
			CHECK_FALSE(editor.IsSceneDirty());
			// While a text field is active, Ctrl+Z belongs to that field.
			const char* hierarchy = Utils::EditorWindowTitle(EditorPanel::SceneHierarchy);
			test.Click(hierarchy, "##Search");
			ImGui::GetIO().AddInputCharactersUTF8("abc");
			test.Draw();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Z, true);
			test.Draw();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Z, false);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			test.Draw();
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScene().GetEntityCount() == 1);
		}

		TEST_CASE("EditorLayer: open panels persist in the workspace layout and a layout without them uses the defaults")
		{
			Test::WorkspaceTestUi test;
			auto& ui = test.Environment.GetEditor().GetUiState();
			test.IniFile = FileSystem::PathToUtf8(test.Environment.GetDirectory() / "WorkspaceLayout.ini");
			ImGui::GetIO().IniFilename = test.IniFile.c_str();
			ImGui::GetIO().IniSavingRate = 0.01f;
			for (int frame = 0; frame < 3; ++frame)
				test.Draw();
			// Toggling panels marks the layout dirty, so Dear ImGui's delayed save writes the new set.
			REQUIRE(ui.SetPanelOpen(EditorPanel::UndoHistory, true));
			REQUIRE(ui.SetPanelOpen(EditorPanel::GameViewport, false));
			test.Draw();
			test.Draw();
			const auto saved = FileSystem::ReadText(FileSystem::PathFromUtf8(test.IniFile));
			REQUIRE(saved);
			REQUIRE(saved->contains("[EnginePanels][Open]"));
			const std::string section = saved->substr(saved->find("[EnginePanels][Open]"));
			const std::string panels = section.substr(0, section.find("\n\n"));
			CHECK(panels.contains("UndoHistory"));
			CHECK_FALSE(panels.contains("GameViewport"));
			const auto open = [&ui](EditorPanel panel)
			{
				return std::find(ui.GetOpenPanels().begin(), ui.GetOpenPanels().end(), panel) != ui.GetOpenPanels().end();
			};
			// The next session starts from the defaults; loading the saved layout restores the user's panels.
			ImGui::GetIO().IniSavingRate = 1000.0f;
			ui.ResetLayout(true);
			ImGui::LoadIniSettingsFromMemory(saved->data(), saved->size());
			test.Draw();
			test.Draw();
			CHECK(open(EditorPanel::UndoHistory));
			CHECK_FALSE(open(EditorPanel::GameViewport));
			CHECK(open(EditorPanel::Inspector));
			CHECK_FALSE(open(EditorPanel::ProjectLauncher));
			// A layout that never saved panels (a fresh project) opens the defaults.
			ImGui::ClearIniSettings();
			test.Draw();
			CHECK_FALSE(open(EditorPanel::UndoHistory));
			CHECK(open(EditorPanel::GameViewport));
			// A layer that goes away while Dear ImGui keeps an ini writes its panels first. Its owner then stops the ini, as
			// EditorApp's shutdown does (EditorAppTests), because a save without the handler would drop them.
			REQUIRE(ui.SetPanelOpen(EditorPanel::Console, false));
			test.Draw();
			{
				EditorLayer replacement(test.Panels);
				REQUIRE(test.Ui.Frame([&replacement]()
				{
					return replacement.OnImGuiRender();
				}));
			}
			ImGui::GetIO().IniFilename = nullptr;
			const auto written = FileSystem::ReadText(FileSystem::PathFromUtf8(test.IniFile));
			REQUIRE(written);
			REQUIRE(written->contains("[EnginePanels][Open]"));
			const std::string finalSection = written->substr(written->find("[EnginePanels][Open]"));
			const std::string finalPanels = finalSection.substr(0, finalSection.find("\n\n"));
			CHECK(finalPanels.contains("GameViewport"));
			CHECK_FALSE(finalPanels.contains("Console"));
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

		// Docs/Decisions/0031-m20-contract.md decision 20 (E-SCENES): the scene tabs, over the open scenes (decision 13) and
		// the methods they submit (scene.new and scene.open keepOpen, scene.activate, scene.close and the scene target).

		// The tabs list the open scenes in tab order with a dirty marker, a click activates one through scene.activate at the
		// next safe point, and the panels show the active scene.
		TEST_CASE("EditorLayer: the scene tabs list the open scenes in tab order and a click activates one")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Only in Main" } }));
			REQUIRE(test.Client.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }));
			const OpenSceneId second = editor.GetActiveSceneId();
			const Result<VfsPath> mainPath = VfsPath::Parse("project://Assets/Scenes/Main.scene");
			REQUIRE(mainPath);
			const OpenSceneId main = editor.FindOpenScene(*mainPath);
			REQUIRE(main != NoOpenScene);
			REQUIRE(editor.GetOpenScenes() == std::vector<OpenSceneId>{ main, second });
			REQUIRE(test.Layer.OnSafePoint(0));
			test.Draw();
			test.Draw();
			// The toolbar names the active scene too, so the active tab's name shows twice.
			size_t seen = 0;
			const auto occurrences = [](std::string_view text, std::string_view part)
			{
				size_t count = 0;
				for (size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size()))
					++count;
				return count;
			};
			const std::string before = NextFrameText(test, seen);
			CHECK(occurrences(before, "Main *") == 1);
			CHECK(occurrences(before, "Second") == 2);
			CHECK_FALSE(before.contains("Second *"));
			CHECK_FALSE(before.contains("Only in Main")); // the hierarchy shows the active scene, Second
			const ImRect mainTab = test.Locate("##EditorSceneTabs", std::format("##SceneTab{}", main).c_str());
			const ImRect secondTab = test.Locate("##EditorSceneTabs", std::format("##SceneTab{}", second).c_str());
			CHECK(mainTab.Max.x <= secondTab.Min.x);
			const ImGuiWindow* toolbar = test.Window("##EditorToolbar");
			CHECK(mainTab.Min.y >= toolbar->Pos.y + toolbar->Size.y);

			// The click queues scene.activate; the safe point runs it, and the panels follow.
			test.Click(mainTab.GetCenter());
			CHECK(editor.GetActiveSceneId() == second);
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetActiveSceneId() == main);
			CHECK(editor.GetScene().GetName() == "Main");
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ main, second });
			CHECK(editor.IsSceneDirty());
			test.Draw();
			CHECK(NextFrameText(test, seen).contains("Only in Main"));

			// An agent's activation shows in the next frame too, and without an open scene the row says so.
			REQUIRE(test.Client.Call("scene.activate", Json{ { "scene", second } }));
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK_FALSE(NextFrameText(test, seen).contains("Only in Main"));
			REQUIRE(test.Client.Call("scene.close", Json{ { "scene", main }, { "discardChanges", true } }));
			REQUIRE(test.Client.Call("scene.close", Json::object()));
			REQUIRE(test.Layer.OnSafePoint(0));
			const std::string none = NextFrameText(test, seen);
			CHECK(occurrences(none, "No scene open") == 2);
			CHECK(occurrences(none, "Second") == 0);
		}

		// A tab's close button closes a scene without unsaved changes at once and asks first for one with them: Cancel keeps
		// it, Save and close writes its file, Discard changes drops them. Each is scene.close with the tab's scene target.
		TEST_CASE("EditorLayer: closing a scene tab asks to save or discard its unsaved changes first")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(test.Client.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }));
			const OpenSceneId second = editor.GetActiveSceneId();
			REQUIRE(test.Client.Call("scene.new", Json{ { "path", "Assets/Scenes/Third.scene" }, { "keepOpen", true } }));
			const OpenSceneId third = editor.GetActiveSceneId();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Kept" } }));
			REQUIRE(test.Client.Call("scene.activate", Json{ { "scene", main } }));
			REQUIRE(test.Layer.OnSafePoint(0));
			test.Draw();
			test.Draw();
			const auto closeTab = [&test](OpenSceneId id)
			{
				test.Click("##EditorSceneTabs", std::format("##CloseSceneTab{}", id).c_str());
				test.Draw();
			};

			// A scene without unsaved changes closes at once; the active scene stays active.
			closeTab(second);
			CHECK(editor.FindScene(second) != nullptr);
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.FindScene(second) == nullptr);
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ main, third });
			CHECK(editor.GetActiveSceneId() == main);

			// A dirty scene asks first, and Cancel keeps it with its changes.
			closeTab(third);
			const ImGuiWindow* prompt = test.Window("Close scene");
			CHECK(prompt->Active);
			CHECK(test.Text().contains("Third has unsaved changes."));
			test.Click("Close scene", "Cancel");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.FindScene(third) != nullptr);
			CHECK(editor.DescribeOpenScene(third)->Dirty);

			// Save and close writes its file first.
			closeTab(third);
			test.Click("Close scene", "Save and close");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.FindScene(third) == nullptr);
			const auto saved = FileSystem::ReadText(editor.GetProject().GetRoot() / "Assets/Scenes/Third.scene");
			REQUIRE(saved);
			CHECK(saved->contains("Kept"));

			// Discard changes drops them; closing the last scene leaves none open.
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Dropped" } }));
			REQUIRE(test.Layer.OnSafePoint(0));
			test.Draw();
			closeTab(main);
			test.Click("Close scene", "Discard changes");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK_FALSE(editor.HasScene());
			const auto discarded = FileSystem::ReadText(editor.GetProject().GetRoot() / "Assets/Scenes/Main.scene");
			REQUIRE(discarded);
			CHECK_FALSE(discarded->contains("Dropped"));
			test.Draw();
			CHECK_FALSE(test.Window("Close scene")->Active);
		}

		// The New and Open scene dialogs open a scene in a new tab (keepOpen), which needs no save-or-discard choice for the
		// active scene's unsaved changes and remembers itself between dialogs; choosing a scene that is open switches to it.
		TEST_CASE("EditorLayer: the scene dialogs open a scene in a new tab and switch to one already open")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			test.Environment.CreateAndOpenScene("Assets/Scenes/Another level.scene");
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Keep this edit" } }));
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(editor.GetOpenScenes() == std::vector<OpenSceneId>{ main });
			REQUIRE(test.Layer.OnSafePoint(0));
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(false);
			test.SelectScene("Assets/Scenes/Another level.scene");
			size_t seen = 0;
			CHECK(NextFrameText(test, seen).contains("Save changes first"));
			test.Click("Open scene", "Open in a new tab");
			CHECK_FALSE(NextFrameText(test, seen).contains("Save changes first"));
			test.Click("Open scene", "Open scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(editor.GetOpenScenes().size() == 2);
			CHECK(editor.GetOpenScenes().front() == main);
			CHECK(editor.GetScenePath()->GetFileName() == "Another level.scene");
			CHECK(editor.DescribeOpenScene(main)->Dirty);
			test.Draw();

			// The choice stays for the New scene dialog.
			test.OpenSceneDialog(true);
			test.ReplaceText("New scene", "##SceneName", "Third");
			test.Click("New scene", "Create scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(editor.GetOpenScenes().size() == 3);
			CHECK(editor.GetScenePath()->GetPath() == "Assets/Scenes/Third.scene");
			test.Draw();

			// A scene that is already open switches to its tab, keeping its changes.
			test.OpenSceneDialog(false);
			test.SelectScene("Assets/Scenes/Main.scene");
			CHECK(NextFrameText(test, seen).contains("This scene is already open in a tab."));
			test.Click("Open scene", "Switch to scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetActiveSceneId() == main);
			CHECK(editor.GetOpenScenes().size() == 3);
			CHECK(editor.IsSceneDirty());
			CHECK(editor.GetScene().FindEntityByPath("/Keep this edit").IsValid());
		}

		// The panels follow the active scene: an inspector gesture begun in one scene ends unwritten when another becomes
		// active, whoever activates it, before the gesture could commit into the other scene.
		TEST_CASE("EditorLayer: activating another scene ends the inspector's gesture unwritten")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			const Result<Json> created = test.Client.Call("entity.create", Json{ { "name", "Edited" } });
			REQUIRE(created);
			const auto edited = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(edited);
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(test.Edits.Begin({ .Kind = InspectorTargetKind::Component, .Entities = { *edited }, .Component = "Transform", .FieldPath = "Translation" }));
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(test.Edits.IsEditing());
			REQUIRE(test.Client.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }));
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK_FALSE(test.Edits.IsEditing());
			CHECK(editor.GetScene().GetName() == "Second");
		}

		// Decision 13: while another scene plays, a tab that is not the played one shows its edit scene in the hierarchy, and a
		// click on a row selects in that scene; the played scene's tab shows the play scene.
		TEST_CASE("EditorLayer: the hierarchy shows the active scene's edit scene while another open scene plays")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Played entity" } }));
			REQUIRE(test.Client.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }));
			const OpenSceneId second = editor.GetActiveSceneId();
			const Result<Json> created = test.Client.Call("entity.create", Json{ { "name", "Edited entity" } });
			REQUIRE(created);
			const auto edited = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(edited);
			REQUIRE(test.Client.Call("scene.activate", Json{ { "scene", main } }));
			REQUIRE(test.Client.Call("play.start", Json{ { "lockstep", true } }));
			REQUIRE(test.Client.Call("scene.activate", Json{ { "scene", second } }));
			REQUIRE(test.Layer.OnSafePoint(0));
			test.Draw();
			test.Draw();
			size_t seen = 0;
			const std::string shown = NextFrameText(test, seen);
			CHECK(shown.contains("Edited entity"));
			CHECK_FALSE(shown.contains("Played entity"));
			CHECK_FALSE(shown.contains("Preview scene"));

			auto* hierarchy = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneHierarchy));
			const ImGuiID row = ImHashStr("##Entity", 0, ImHashStr(edited->ToString().c_str(), 0, hierarchy->ID));
			test.Click(test.Locate(hierarchy, row).GetCenter());
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetActiveSceneId() == second);
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ *edited });

			// The played scene's tab shows the play scene again.
			REQUIRE(test.Client.Call("scene.activate", Json{ { "scene", main } }));
			REQUIRE(test.Layer.OnSafePoint(0));
			test.Draw();
			const std::string played = NextFrameText(test, seen);
			CHECK(played.contains("Played entity"));
			CHECK(played.contains("Preview scene"));
			REQUIRE(test.Client.Call("play.stop", Json::object()));
		}

		TEST_CASE("EditorLayer: the game panel shows the lockstep owner and its blur leaves agent input alone" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			LayerGpuHarness test(gpu);
			test.Ui.OnlyPanel(EditorPanel::GameViewport);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			REQUIRE(test.Ui.Client.Call("play.start", Json{ { "lockstep", true } }));
			auto& session = *test.Ui.Environment.GetEditor().GetPlay().GetSession();
			test.Advance(0);
			test.Ui.Draw();
			test.Ui.Draw();
			ImGui::SetWindowFocus("Game###GameViewport");
			const auto* window = ImGui::FindWindowByName("Game###GameViewport");
			REQUIRE(window != nullptr);
			const ImVec2 inside = window->InnerRect.GetCenter();
			ImGui::GetIO().AddMousePosEvent(inside.x, inside.y);
			test.Ui.Draw();
			const std::string drawn = test.Ui.Draw();
			CHECK(drawn.contains(std::format("Agent controls time (owner {})", session.GetLockstepOwner())));
			// Focus and hover alone do not capture game input; a click on the image does.
			CHECK_FALSE(test.Ui.Views.IsGameInputFocused());
			test.Ui.Click(inside);
			REQUIRE(test.Ui.Views.IsGameInputFocused());
			REQUIRE(session.GetInput().Queue(session.GetTick(), { .KeyCode = Key::Space }));
			REQUIRE(test.Ui.Client.Call("play.step", Json{ { "ticks", 1 } }));
			CHECK(session.GetInput().GetSummary(InputPhase::Step).Down == std::vector<std::string>{ "Key.Space" });
			// Route the same held arrow to ImGui and game input. Navigation must not steal focus from the game
			// over subsequent frames or enqueue a release while the physical key is still held.
			ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, true);
			REQUIRE(session.GetInput().Queue(session.GetTick(), { .KeyCode = Key::Down }));
			constexpr uint32_t HeldFrames = 30;
			for (uint32_t frame = 0; frame < HeldFrames; ++frame)
			{
				test.Ui.Draw();
				CHECK(ImGui::GetCurrentContext()->NavWindow == window);
				CHECK(test.Ui.Views.IsGameInputFocused());
				CHECK(ImGui::IsKeyDown(ImGuiKey_DownArrow));
				REQUIRE(test.Ui.Client.Call("play.step", Json{ { "ticks", 1 } }));
				CHECK(session.GetInput().GetSummary(InputPhase::Step).Down == std::vector<std::string>{ "Key.Down", "Key.Space" });
				CHECK(session.GetInput().GetSummary(InputPhase::Step).Released.empty());
			}
			ImGui::SetWindowFocus(nullptr);
			ImGui::GetIO().AddMousePosEvent(-100, -100);
			test.Ui.Draw();
			CHECK_FALSE(test.Ui.Views.IsGameInputFocused());
			CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0);
			// Native input never reaches a lockstep session, so everything held there is its owner's: a UI blur neither
			// advances the session nor injects releases into it (the agent's state hash stays independent of the UI).
			CHECK(session.GetTick() == 1 + HeldFrames);
			CHECK(session.GetInput().GetQueuedEventCount() == 0);
			REQUIRE(test.Ui.Client.Call("play.step", Json{ { "ticks", 1 } }));
			CHECK(session.GetInput().GetSummary(InputPhase::Step).Down == std::vector<std::string>{ "Key.Down", "Key.Space" });
			CHECK(session.GetInput().GetSummary(InputPhase::Step).Released.empty());
			ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, false);
			test.Ui.Draw();
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

		// Docs/Decisions/0031-m20-contract.md decision 15: one offer covers every scene with a recoverable generation, and
		// Recover recovers each of them, so the prompt names them all.
		TEST_CASE("EditorLayer: the recovery prompt names every scene the offer recovers")
		{
			LayerUiHarness ui;
			EditorContext& editor = ui.Environment.GetEditor();
			Autosave saves(editor);
			EditorHostRecovery host(editor, saves);
			REQUIRE(ui.Client.Call("entity.create", Json{ { "name", "InMain" } }));
			REQUIRE(ui.Client.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }));
			REQUIRE(ui.Client.Call("entity.create", Json{ { "name", "InSecond" } }));
			REQUIRE(saves.Publish());
			const auto saved = saves.Save(AutosaveReason::Periodic);
			REQUIRE_MESSAGE(saved.has_value(), saved.error().ToString());
			REQUIRE(saved->Written);
			// As after a crash and a reopen: Main is open again as its file holds it, and Second is not open.
			REQUIRE(ui.Client.Call("scene.close", Json{ { "discardChanges", true } }));
			REQUIRE(ui.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true }, { "discardChanges", true } }));
			REQUIRE_FALSE(editor.IsAnySceneDirty());
			REQUIRE(host.Inspect(1));
			REQUIRE(host.GetOffer().has_value());
			REQUIRE(host.GetOffer()->SceneNames == std::vector<std::string>{ "Main", "Second" });
			ui.Panels.Recovery.GetOffer = [&host]()
			{
				return host.GetOffer();
			};
			ui.OnlyPanel(EditorPanel::SceneHierarchy);
			ui.Draw();
			const std::string drawn = ui.Draw();
			CHECK(drawn.contains("Unsaved versions of 2 scenes are available: Main, Second."));
			CHECK_FALSE(drawn.contains("An unsaved version of"));
			ui.Panels.Recovery = {};
			host.Reset();
			CHECK(saves.Reset());
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
			auto& editor = test.Ui.Environment.GetEditor();
			const auto translation = [&editor](UUID id)
			{
				return editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Translation;
			};
			const glm::vec3 left = translation(test.Left);
			const glm::vec3 right = translation(test.Right);
			const size_t history = editor.GetHistory().GetUndoCount();
			test.Advance(0);
			test.Ui.Draw();
			ImGui::SetWindowFocus("Scene###SceneViewport");
			test.Ui.Draw();
			test.Advance(1);
			test.Ui.Draw();
			const auto image = test.Ui.Views.GetImage(ViewportView::Scene);
			const auto* window = ImGui::FindWindowByName("Scene###SceneViewport");
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
			// The release committed the whole gesture as one undo step that moved both entities by one world delta.
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			CHECK(editor.GetHistory().GetUndoLabel() == "Transform selection");
			const glm::vec3 delta = translation(test.Left) - left;
			CHECK(glm::length(delta) > 0.01f);
			CHECK(glm::length((translation(test.Right) - right) - delta) < 1.0e-5f);
			test.Advance(6);
			test.Advance(7);
			test.CheckSelection({ test.Left, test.Right });
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(translation(test.Left) == left);
			CHECK(translation(test.Right) == right);
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
