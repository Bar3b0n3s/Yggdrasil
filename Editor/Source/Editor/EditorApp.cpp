#include "EditorPCH.h"
#include "Editor/EditorApp.h"

#include "Editor/EditorLayer.h"
#include "Editor/EditorPanelContext.h"
#include "Editor/Private/EditorHostAutosave.h"
#include "Editor/Private/EditorHostRecovery.h"
#include "Editor/Private/EditorHostSource.h"
#include "Editor/Private/EditorHostThumbnails.h"
#include "Editor/Private/EditorHostViewports.h"
#include "EditorCore/Audio/AudioPreview.h"
#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/BatchRunner.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/EditorCommandLine.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/EngineAssetGenerators.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "EditorCore/ShownSceneTracker.h"
#include "Engine/App/CommandLine.h"
#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/App/VulkanErrorHandler.h"
#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/EngineAssetBaker.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/ImGui/ImGuiScreenshot.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Window.h"
#include "Engine/Renderer/BlitPass.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/StaleMirrorSchedule.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#if !defined(ENGINE_REPO_ROOT)
	#error "ENGINE_REPO_ROOT must be defined for the editor (Dependencies.lua, ApplyFirstPartySettings): it finds Resources and Docs there"
#endif

namespace Engine {

	struct EditorApp::State
	{
		EditorLaunchOptions Options{};
		// With a device: the GPU environment bake (§8.6) the editor's asset manager imports environments with (M8). Created
		// before the editor and destroyed after it (OnShutdown), since the asset manager keeps a pointer to it.
		Scope<EnvironmentBaker> Baker;
		Scope<EditorContext> Editor; // destroyed last (reverse member order): the server and the run use it
		Scope<Autosave> Saves;
		Scope<EditorHostRecovery> Recovery;
		Scope<AutomationServer> Server;
		Scope<EditorActions> Actions;
		Scope<EditorAutomationControls> Controls;
		Scope<ReflectedEditController> Inspector;
		Scope<GizmoController> Gizmos;
		Scope<EditorHostViewports> Views;
		Scope<EditorHostThumbnails> Thumbnails;
		Scope<EditorPanelContext> Panels;
		Scope<EditorLayer> Layer;
		EditorAutomationPreferenceState Preference{};
		std::optional<bool> PendingPreference{};
		uint64_t ProjectGeneration = 0;
		bool DeviceLostQueued = false;
		const LoadedProject* BoundProject = nullptr; // identity only; cleared before project close
		std::vector<std::filesystem::path> ContentDrops{};
		std::optional<AssetHandle> AudioRequest{};
		std::vector<std::pair<std::filesystem::path, uint32_t>> SourceRequests{};
		std::vector<Process> SourceProcesses{};
		double ProjectOpenedAt = 0.0;
		bool UiConstructed = false;
		bool FreshUiRequested = false;
		uint64_t CaptureReadyFrame = 0;
		bool ScreenshotsWritten = false;
		bool FinalFrame = false;
		std::optional<uint64_t> TimingSession{};
		double TimingDroppedBase = 0.0;
		std::string LastHostError{};
		std::string LastAutosaveError{};
		Scope<BatchRunner> Batch;    // --batch or --upgrade
		double ElapsedSeconds = 0.0; // the frame clock's accumulated unscaled time, for EditorContext::Update

		// OnInitialize's work for `app`, whose server gets `captures` and `systemErrors` (empty without a device); on
		// failure the caller releases what was built.
		[[nodiscard]] Status Initialize(EditorApp& app, ScreenshotCaptures captures, SystemErrorHandler systemErrors);
		// Releases the run and the server, in that order (the server's pending operations are cancelled against a live
		// editor, and it holds the captures).
		void ReleaseServer();
	};

	struct EditorApp::Rendering
	{
		// Declared in creation order; destroyed in reverse (§8.14 item 4: the scene renderers before the pipelines they
		// record with, the pipelines and renderers before the GpuResourceCache).
		Scope<GpuResourceCache> Cache;
		Scope<SceneRendererPipelines> Pipelines;
		Scope<ViewportCapture> Capture; // screenshots (CaptureView)
		bool IsRenderFailing = false;
		// Stale mirrors (SceneRenderer.h; Docs/Decisions/0013-m8-decisions.md decision 7): the scene change OnUpdate noticed
		// and when the collections release what only the previous scene used.
		ShownSceneTracker ShownScenes{};
		StaleMirrorSchedule Mirrors{};
	};

	namespace Utils {

		constexpr std::string_view ViewportScreenshotOption = "--viewport-screenshot";

		// Ends the process for a GPU object the editor creates at startup or resize that the device has no memory for
		// (§8.14 item 7); other errors are returned with `context`.
		[[nodiscard]] static Error CheckStartupGpuObject(Error error, std::string_view what)
		{
			if (error.GetCode() == ErrorCode::Gpu)
				FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot create {}: {}", what, error.ToString()));
			return std::move(error).WithContext(std::format("while creating {}", what));
		}
		constexpr std::string_view EditorScreenshotOption = "--editor-screenshot";

		constexpr std::array EditorAppCommandLineOptions = {
			CommandLineOption{
				.Name = ViewportScreenshotOption,
				.Value = CommandLineValue::Required,
				.ValueName = "path",
				.Description = "After the last frame of a --frames run, write a 640x360 screenshot of the scene viewport to "
							   "this PNG file.",
			},
			CommandLineOption{
				.Name = EditorScreenshotOption,
				.Value = CommandLineValue::Required,
				.ValueName = "path",
				.Description = "After the last frame of a --frames run (2 or more), write a screenshot of the editor UI to this PNG file.",
			},
		};

		// The engine's options, EditorCore's and the Editor executable's own. The views refer to string literals
		// (CommandLineOption's rule).
		[[nodiscard]] static std::vector<CommandLineOption> GetEditorAppCommandLineOptions()
		{
			std::vector<CommandLineOption> options(GetEngineCommandLineOptions().begin(), GetEngineCommandLineOptions().end());
			options.insert(options.end(), GetEditorCommandLineOptions().begin(), GetEditorCommandLineOptions().end());
			options.insert(options.end(), EditorAppCommandLineOptions.begin(), EditorAppCommandLineOptions.end());
			return options;
		}

		// The value of path option `name` as a path; an empty path when the option was not given.
		[[nodiscard]] static Result<std::filesystem::path> GetPathOption(const CommandLine& commandLine, std::string_view name)
		{
			const std::optional<std::string_view> value = commandLine.GetValue(name);
			if (!value.has_value())
				return std::filesystem::path();
			// std::filesystem::path throws on ill-formed UTF-8 on Windows, and the option is external input.
			if (value->empty() || !IsValidUtf8(*value))
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a file path in UTF-8", name);
			return FileSystem::PathFromUtf8(*value);
		}

		// The repository root of development builds: Resources and Docs live there (§2.2; the editor has no Dist build).
		[[nodiscard]] static std::filesystem::path GetEditorRepositoryRoot()
		{
			return std::filesystem::path(ENGINE_REPO_ROOT);
		}

		// Writes `document` canonically (JsonWriter, pretty) to `file`.
		[[nodiscard]] static Status WriteReferenceFile(const std::filesystem::path& file, const Json& document)
		{
			ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(document, JsonStyle::Pretty));
			return FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false });
		}

		// --bake-engine-assets (§7.5, ADR 0010 decision 13): bakes every File and Generated entry of engine://EngineAssets.json
		// into enginecache:// with the built-in importers, the editor's generators (EditorCore/EngineAssetGenerators.h) and,
		// with a device, the GPU environment baker (M8); without one (--renderer none, no Vulkan device) the environments are
		// skipped with a warning. Returns the exit code: Success when every entry is baked, up to date or skipped with a
		// warning, Failed when an entry or the run failed (each failure logged at Error).
		[[nodiscard]] static int BakeEngineResources(EngineContext& context, IEnvironmentBaker* environmentBaker)
		{
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Load(context.GetVfs());
			if (!catalog.has_value())
			{
				ENGINE_ERROR("Cannot bake the engine assets: {}", catalog.error());
				return ExitCode::Failed;
			}
			const EngineBakeSpecification specification{
				.Vfs = &context.GetVfs(),
				.Importers = &importers,
				.Registry = &context.GetTypeRegistry(),
				.Jobs = &context.GetJobSystem(),
				.EnvironmentBaker = environmentBaker,
				.Generators = GetEngineAssetGenerators(),
			};
			const Result<EngineBakeReport> report = BakeEngineAssets(specification, *catalog);
			if (!report.has_value())
			{
				ENGINE_ERROR("Cannot bake the engine assets: {}", report.error());
				return ExitCode::Failed;
			}
			bool failed = false;
			for (const AssetDiagnostic& diagnostic : report->Skipped)
			{
				if (diagnostic.Severity == DiagnosticSeverity::Error)
				{
					ENGINE_ERROR("{}", AssetDiagnosticToString(diagnostic));
					failed = true;
				}
				else
				{
					ENGINE_WARN("{}", AssetDiagnosticToString(diagnostic));
				}
			}
			ENGINE_INFO("Engine assets: {} baked, {} up to date, {} not baked", report->Baked.size(), report->UpToDate.size(), report->Skipped.size());
			return failed ? ExitCode::Failed : ExitCode::Success;
		}

		// --dump-reference <dir>: Methods.json and catalog.json (ADR 0008 decision 9), from every method but the test hooks.
		[[nodiscard]] static Status DumpReference(const TypeRegistry& types, const std::filesystem::path& directory)
		{
			MethodRegistry methods(types);
			RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = false });
			methods.Freeze();
			ENGINE_TRY(FileSystem::CreateDirectories(directory));
			ENGINE_TRY(WriteReferenceFile(directory / "Methods.json", methods.BuildMethodCatalog()));
			ENGINE_TRY(WriteReferenceFile(directory / "catalog.json", methods.BuildToolCatalog()));
			ENGINE_INFO("Wrote the method catalogue ({} methods) and the MCP tool catalogue", methods.GetMethods().size());
			return {};
		}

	}

	EditorApp::EditorApp(ApplicationSpecification specification, EditorAppOptions options)
		: Application(std::move(specification)), m_Options(std::move(options)), m_State(CreateScope<State>())
	{
	}

	EditorApp::~EditorApp() = default;

	Status EditorApp::State::Initialize(EditorApp& app, ScreenshotCaptures captures, SystemErrorHandler systemErrors)
	{
		State& state = *this;
		ENGINE_TRY_ASSIGN(state.Options, ParseEditorLaunchOptions(app.GetSpecification().Args));
		const EditorLaunchOptions& options = state.Options;
		const bool headless = app.GetSpecification().Window == WindowMode::Headless;

		if (options.DumpReferenceDirectory.has_value())
		{
			ENGINE_TRY(Utils::DumpReference(app.GetContext().GetTypeRegistry(), *options.DumpReferenceDirectory));
			app.RequestExit(ExitCode::Success);
			return {};
		}

		// With a device, the environment baker's pipelines are created at startup with the editor's other pipelines (§8.12);
		// the asset manager's EnvironmentImporter and the engine asset bake use it (§7.4, §7.5).
		EngineContext& context = app.GetContext();
		if (GraphicsDevice* device = context.GetGraphicsDevice())
		{
			Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(*device, *context.GetPipelineFactory());
			if (!baker.has_value())
				return std::unexpected(Utils::CheckStartupGpuObject(std::move(baker).error(), "the environment baker"));
			state.Baker = std::move(*baker);
		}

		// The bake needs no project and no editor state: a --project given with it is not opened.
		if (options.BakeEngineAssets)
		{
			app.RequestExit(Utils::BakeEngineResources(context, state.Baker.get()));
			return {};
		}

		const std::filesystem::path repository = Utils::GetEditorRepositoryRoot();
		const UserDataPaths& userData = app.GetProcessContext().GetUserDataPaths();
		EditorContextSpecification editorSpecification;
		editorSpecification.TemplatesDirectory = repository / "Resources" / "Templates" / "Projects";
		editorSpecification.ReadOnlyCacheRoot = userData.Root / "ReadOnlyCache";
		editorSpecification.EnvironmentBaker = state.Baker.get();
		ENGINE_TRY_ASSIGN(state.Editor, EditorContext::Create(app.GetContext(), editorSpecification));
		state.Saves = CreateScope<Autosave>(*state.Editor);
		state.Recovery = CreateScope<EditorHostRecovery>(*state.Editor, *state.Saves);
		state.Gizmos = CreateScope<GizmoController>(*state.Editor);
		state.Inspector = CreateScope<ReflectedEditController>(*state.Editor);
		state.Editor->SetLifecycleCallbacks({ .BeforePlay = [&app]()
		{
			return app.BeforePlay();
		},
			.AfterSceneSaved = [&app]()
		{
			return app.AfterSceneSaved();
		},
			.BeforeProjectClose = [&app]()
		{
			return app.BeforeProjectClose();
		} });
		state.Views = CreateScope<EditorHostViewports>(*state.Editor, *state.Gizmos);
		state.Thumbnails = CreateScope<EditorHostThumbnails>(*state.Editor, [&app](const RenderSnapshot& snapshot, uint32_t size)
		{
			return app.CaptureView(snapshot, ViewportScreenshotRequest{ .Width = size, .Height = size });
		});
		app.GetProcessContext().SetFatalErrorHook(MakeEditorAutosaveFatalHook(*state.Saves));

		if (options.Project.has_value())
		{
			const ProjectOpenOptions openOptions{
				.ReadOnly = options.ReadOnly,
				.StrictUnknowns = false,
				.ReadOnlyCacheDirectory = options.ReadOnly ? editorSpecification.ReadOnlyCacheRoot / std::to_string(Process::GetCurrentId())
														   : std::filesystem::path(),
			};
			ENGINE_TRY_ASSIGN(Scope<LoadedProject> project, ProjectManager::OpenProject(*options.Project, openOptions, app.GetContext().GetTypeRegistry()));
			ENGINE_TRY(state.Editor->OpenProject(std::move(project)));
		}

		const bool listens = options.ListensForAutomation(headless);
		if (state.Editor != nullptr)
		{
			AutomationServerSpecification serverSpecification;
			serverSpecification.Listen = listens;
			serverSpecification.PreferenceListeningAllowed = !options.IsOneShot();
			serverSpecification.AutosaveService = state.Saves.get();
			serverSpecification.Port = options.AutomationPort.value_or(0);
			serverSpecification.TestHooks = options.AutomationTestHooks;
			serverSpecification.Headless = headless;
			serverSpecification.RendererName = std::string(RendererModeToCommandLine(app.GetSpecification().Renderer));
			serverSpecification.Screenshots = std::move(captures);
			serverSpecification.SystemErrors = std::move(systemErrors);
			serverSpecification.SessionsDirectory = userData.Root / "Automation" / "Sessions";
			serverSpecification.DocsRoot = repository;
			serverSpecification.ExportBinaryRoot = repository / "bin";
			if (options.AutomationTestHooks && context.GetGraphicsDevice())
				serverSpecification.QueueDeviceLost = [&app]() -> Status
				{
					app.m_State->DeviceLostQueued = true;
					return {};
				};
			serverSpecification.ReadHostStatistics = [&app]()
			{
				StatsGetResult result;
				const FrameLoopStatistics timing = app.GetFrameStatistics();
				result.Fps = ToStatsTelemetry(timing.Fps);
				result.CpuMilliseconds = ToStatsTelemetry(timing.CpuMilliseconds);
				const PlaySession* session = app.m_State->Editor->GetPlay().GetSession();
				if (session && app.m_State->TimingSession == session->GetSerial())
					result.DroppedSeconds = ToStatsTelemetry(std::max(0.0, timing.DroppedSeconds - app.m_State->TimingDroppedBase));
				for (const ViewportView view : { ViewportView::Scene, ViewportView::Game })
				{
					const std::string name = view == ViewportView::Scene ? "scene" : "game";
					const SceneRenderer* renderer = app.m_State->Views->GetRenderer(view);
					if (renderer && app.m_State->Views->GetImage(view).Texture != 0)
						result.Views.push_back(MakeStatsViewSummary(name, renderer->GetRenderStats()));
					else
						result.Views.push_back(StatsViewSummary{ .Name = name });
				}
				return result;
			};
			ENGINE_TRY_ASSIGN(state.Server, AutomationServer::Create(*state.Editor, serverSpecification));
		}

		if (options.BatchFile.has_value())
		{
			ENGINE_TRY_ASSIGN(std::vector<BatchRequest> requests, BatchRunner::LoadFile(*options.BatchFile));
			state.Batch = CreateScope<BatchRunner>(std::move(requests), BatchRunOptions{ .ClientName = "batch", .WriteTranscript = false });
		}
		else if (options.Upgrade)
		{
			std::vector<BatchRequest> requests = { BatchRequest{ .Method = "project.upgrade", .Params = {}, .Line = 0 } };
			state.Batch = CreateScope<BatchRunner>(std::move(requests), BatchRunOptions{ .ClientName = "cli", .WriteTranscript = true });
		}
		return {};
	}

	void EditorApp::State::ReleaseServer()
	{
		Batch.reset();
		Server.reset();
	}

	Status EditorApp::OnInitialize()
	{
		Status initialized = InitializeEditor();
		// Run calls OnShutdown only after a successful OnInitialize, and destroys the engine context and its device next:
		// what was built (the GPU objects, the editor the server refers to) and the hook go now.
		if (!initialized)
			OnShutdown();
		return initialized;
	}

	Status EditorApp::InitializeEditor()
	{
		EngineContext& context = GetContext();
		ScreenshotCaptures captures;
		SystemErrorHandler systemErrors;
		if (context.GetGraphicsDevice() != nullptr)
		{
			// The captures and the system-error mapping refer to this application, which outlives the server (OnShutdown
			// releases the server first). They render through the GPU objects InitializeRendering creates below, before the
			// first frame serves a request.
			captures.View = [this](const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request)
			{
				return CaptureView(snapshot, request);
			};
			captures.CompletedUiFrame = [this]()
			{
				return m_State->CaptureReadyFrame;
			};
			captures.RequestUiFrame = [this]()
			{
				m_State->FreshUiRequested = true;
				if (m_State->Layer)
					m_State->Layer->RequestFrame();
				const Status requested = RequestOffscreenUiFrame();
				if (!requested)
					ENGINE_ERROR("Cannot request editor frame: {}", requested.error());
			};
			captures.EditorUi = [this]()
			{
				return CaptureEditorUi();
			};
			// A Vulkan error thrown out of NVRHI inside a method (vk::SystemError, §4.6 item 2) ends the process like the
			// frame-boundary catch (App/FrameLoop.cpp) would, instead of becoming an Internal response on a lost device.
			systemErrors = [this, handler = MakeVulkanSystemErrorHandler(GetContext())](const std::system_error& error, std::string_view phase)
			{
				RefreshFatalSnapshot();
				handler(error, phase);
			};
		}

		ENGINE_TRY(m_State->Initialize(*this, std::move(captures), std::move(systemErrors)));
		ENGINE_TRY(InitializeRendering());
		return InitializeUi();
	}

	Status EditorApp::InitializeRendering()
	{
		// Only an editor with a device and an EditorContext renders views (the one-shot modes --dump-reference and
		// --bake-engine-assets create no editor).
		EngineContext& context = GetContext();
		GraphicsDevice* device = context.GetGraphicsDevice();
		if (device == nullptr || m_State->Editor == nullptr)
			return {};

		// Every pipeline is created at startup (§8.12), and a startup creation the device has no memory for is fatal (§8.14
		// item 7).
		AssetManager& assets = m_State->Editor->GetAssets();
		Scope<Rendering> rendering = CreateScope<Rendering>();
		rendering->Cache = CreateScope<GpuResourceCache>(*device, assets);
		Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(*device, *context.GetPipelineFactory());
		if (!pipelines.has_value())
			return std::unexpected(Utils::CheckStartupGpuObject(std::move(pipelines).error(), "the scene renderer's pipelines"));
		rendering->Pipelines = std::move(*pipelines);
		Result<Scope<ViewportCapture>> capture = ViewportCapture::CreateForScenes(*device, *rendering->Pipelines, *rendering->Cache, assets);
		if (!capture.has_value())
			return std::unexpected(Utils::CheckStartupGpuObject(std::move(capture).error(), "the viewport capture"));
		rendering->Capture = std::move(*capture);
		m_Rendering = std::move(rendering);
		// Adopt dependencies before constructing views: partial startup failure tears the views down first too.
		const Status initialized = m_State->Views->Initialize(*device, *m_Rendering->Pipelines, *m_Rendering->Cache, GetImGuiLayer()->GetRenderer());
		if (!initialized)
			return std::unexpected(Utils::CheckStartupGpuObject(initialized.error(), "the editor views"));
		m_State->Thumbnails->SetGraphics(*device, GetImGuiLayer()->GetRenderer());
		return {};
	}

	Status EditorApp::InitializeUi()
	{
		State& state = *m_State;
		if (!state.Editor)
			return {};
		ENGINE_TRY_ASSIGN(const EditorPreferences preferences, ReadEditorPreferences(state.Editor->GetVfs()));
		state.Preference.Allowed = preferences.AllowAiAutomation;
		ENGINE_TRY(state.Server->SetPreferenceListening(preferences.AllowAiAutomation));
		state.Actions = CreateScope<EditorActions>(*state.Editor, *state.Server);
		state.Controls = CreateScope<EditorAutomationControls>(*state.Editor, *state.Server);
		state.Panels = CreateScope<EditorPanelContext>(*state.Editor, *state.Server, *state.Actions, *state.Controls,
			*state.Views, *state.Inspector, state.Thumbnails->GetCache(), *state.Gizmos);
		state.Panels->FindThumbnailTexture = [this](const ThumbnailRequest& request)
		{
			return m_State->Thumbnails->FindTexture(request);
		};
		state.Panels->TakeContentDrops = [this]()
		{
			return std::exchange(m_State->ContentDrops, {});
		};
		state.Panels->QueueAudioPreview = [this](AssetHandle asset) -> Status
		{
			if (!m_State->Editor->HasProject())
				return MakeError(ErrorCode::InvalidState, "Open a project before previewing audio");
			m_State->AudioRequest = asset;
			return {};
		};
		state.Panels->Recovery.GetOffer = [this]()
		{
			return m_State->Recovery->GetOffer();
		};
		state.Panels->Recovery.QueueDecision = [this](const EditorRecoveryOffer& offer, EditorRecoveryDecision decision) -> Status
		{
			return m_State->Recovery->QueueDecision(offer, decision);
		};
		state.Panels->AutomationPreferences.GetState = [this]()
		{
			return m_State->Preference;
		};
		state.Panels->AutomationPreferences.QueueChange = [this](bool allowed) -> Status
		{
			if (m_State->PendingPreference)
				return MakeError(ErrorCode::InvalidState, "A preference change is already queued");
			m_State->PendingPreference = allowed;
			m_State->Preference.Pending = true;
			m_State->Preference.Failure.reset();
			return {};
		};
		state.Panels->OpenSource = [this](const std::filesystem::path& path, uint32_t line) -> Status
		{
			m_State->SourceRequests.emplace_back(path, line);
			return {};
		};
		// The layer's constructor only builds CPU models; no ImGui calls until OnImGuiRender.
		state.Layer = CreateScope<EditorLayer>(*state.Panels);
		return SynchronizeProject();
	}

	Status EditorApp::SynchronizeProject()
	{
		State& state = *m_State;
		if (!state.Editor || !state.Editor->HasProject())
			return {};
		if (state.BoundProject == &state.Editor->GetProject())
			return {};
		if (state.BoundProject != nullptr)
			return MakeError(ErrorCode::InvalidState, "Project changed without completing its close handshake");
		ENGINE_VERIFY(state.ProjectGeneration < std::numeric_limits<uint64_t>::max(), "project epoch exhausted");
		ENGINE_TRY(state.Thumbnails->GetCache().BindProject(++state.ProjectGeneration));
		if (ImGuiLayer* imgui = GetImGuiLayer())
		{
			const auto path = state.Editor->IsReadOnly() ? state.Editor->GetProject().GetCacheDirectory() / "Editor" / "imgui.ini"
														 : state.Editor->GetProject().GetRoot() / "Library" / "Editor" / "imgui.ini";
			const Status changed = imgui->SetIniFilePath(path);
			if (!changed)
			{
				state.Thumbnails->Reset();
				return std::unexpected(changed.error());
			}
		}
		state.BoundProject = &state.Editor->GetProject();
		state.ProjectOpenedAt = state.ElapsedSeconds;
		const Status inspected = state.Recovery->Inspect(state.ProjectGeneration);
		if (!inspected)
			ENGINE_WARN("Recovery is unavailable: {}", inspected.error());
		return {};
	}
	Status EditorApp::BeforeProjectClose()
	{
		State& state = *m_State;
		ImGuiLayer* imgui = GetImGuiLayer();
		const auto previousIni = imgui ? imgui->GetIniFilePath() : std::filesystem::path{};
		// SetIniFilePath can fail while preparing its directory; do that before closing writer admission or releasing
		// any project binding. If a fatal writer still owns the lease, restore the open project's layout and retry later.
		if (imgui)
			ENGINE_TRY(imgui->SetIniFilePath(GetProcessContext().GetUserDataPaths().Root / "Editor" / "imgui.ini"));
		if (state.Saves && !state.Saves->Reset())
		{
			if (imgui)
				ENGINE_TRY(imgui->SetIniFilePath(previousIni));
			return MakeError(ErrorCode::InvalidState, "Autosave is finishing; retry closing the project");
		}
		if (state.Gizmos)
			state.Gizmos->Cancel();
		if (state.Inspector)
			state.Inspector->Cancel();
		if (state.Views)
			state.Views->Invalidate();
		if (state.Thumbnails)
			state.Thumbnails->Reset();
		state.ContentDrops.clear();
		state.AudioRequest.reset();
		state.SourceRequests.clear();
		state.Recovery->Reset();
		state.BoundProject = nullptr;
		return {};
	}
	Status EditorApp::BeforePlay()
	{
		State& state = *m_State;
		state.Gizmos->Cancel();
		state.Inspector->Cancel();
		if (!state.Editor->IsReadOnly() && !state.Editor->IsDryRun())
			ENGINE_TRY(state.Saves->Save(AutosaveReason::BeforePlay));
		return {};
	}
	Status EditorApp::AfterSceneSaved()
	{
		if (!m_State->Editor->IsReadOnly() && !m_State->Editor->IsDryRun())
			return m_State->Saves->DiscardSavedRecovery();
		return {};
	}
	void EditorApp::PublishAutosave()
	{
		State& state = *m_State;
		if (!state.Saves)
			return;
		const Status published = state.Saves->Publish();
		if (!published)
		{
			const std::string error = published.error().ToString();
			if (error != state.LastAutosaveError)
			{
				if (published.error().GetCode() == ErrorCode::Conflict)
					ENGINE_WARN("Autosave waits for the source conflict to be resolved: {}", error);
				else
					ENGINE_ERROR("Cannot publish autosave: {}", error);
			}
			state.LastAutosaveError = error;
		}
		else
			state.LastAutosaveError.clear();
	}
	void EditorApp::RefreshFatalSnapshot()
	{
		if (m_State->Saves && m_State->Editor && !m_State->Editor->IsReadOnly() && !m_State->Editor->IsDryRun())
			(void)m_State->Saves->Save(AutosaveReason::FatalError);
	}
	Status EditorApp::UpdateHostServices()
	{
		State& state = *m_State;
		if (!state.Editor)
			return {};
		ENGINE_TRY(SynchronizeProject());
		if (state.PendingPreference)
		{
			const bool allowed = *std::exchange(state.PendingPreference, std::nullopt);
			const Status applied = state.Controls->SetAllowAiAutomation(allowed);
			state.Preference.Pending = false;
			if (applied)
			{
				state.Preference.Allowed = allowed;
				state.Preference.Failure.reset();
			}
			else
			{
				state.Preference.Failure = applied.error();
				return std::unexpected(applied.error());
			}
		}
		ENGINE_TRY(state.Recovery->Pump());
		if (state.AudioRequest)
		{
			const auto clip = *std::exchange(state.AudioRequest, std::nullopt);
			AudioPreview* preview = state.Editor->GetAudioPreview();
			if (!preview)
				return MakeError(ErrorCode::Unsupported, "No audio preview service");
			if (clip.IsValid())
				ENGINE_TRY(preview->Play(clip));
			else
				preview->Stop();
		}
		if (!state.SourceRequests.empty())
		{
			const auto requests = std::exchange(state.SourceRequests, {});
			ENGINE_TRY_ASSIGN(const EditorPreferences preferences, ReadEditorPreferences(state.Editor->GetVfs()));
			if (preferences.ExternalEditorExecutable.empty())
				return MakeError(ErrorCode::Unsupported, "Configure an external source editor first");
			for (const auto& [path, line] : requests)
			{
				const auto root = state.Editor->HasProject() ? state.Editor->GetProject().GetRoot() : std::filesystem::path{};
				ENGINE_TRY_ASSIGN(const auto native, ResolveEditorSourcePath(path, root, Utils::GetEditorRepositoryRoot()));
				ENGINE_TRY_ASSIGN(ProcessSpecification specification, BuildEditorSourceProcessSpecification(preferences, native, line));
				specification.WorkingDirectory = root.empty() ? Utils::GetEditorRepositoryRoot() : root;
				ENGINE_TRY_ASSIGN(Process process, Process::Spawn(specification));
				state.SourceProcesses.push_back(std::move(process));
			}
		}
		std::erase_if(state.SourceProcesses, [](Process& process)
		{
			return process.HasExited();
		});

		if (state.Thumbnails)
			ENGINE_TRY(state.Thumbnails->Pump());
		return {};
	}
	void EditorApp::OnEvent(Event& event)
	{
		State& state = *m_State;
		if (!state.Editor)
			return;
		if (auto* drop = std::get_if<FileDropEvent>(&event))
		{
			if (state.Editor->HasProject())
				for (const auto& path : drop->Paths)
					if (IsValidUtf8(path))
						state.ContentDrops.push_back(FileSystem::PathFromUtf8(path));
			drop->Handled = true;
			return;
		}
		if (const auto* resized = std::get_if<WindowResizeEvent>(&event))
			if (resized->FramebufferWidth == 0 || resized->FramebufferHeight == 0)
				state.Views->SetUnavailable();
		if (const auto* focused = std::get_if<WindowFocusEvent>(&event))
			if (!focused->Focused)
			{
				state.Views->SetGameInputFocused(false);
				state.Gizmos->Cancel();
			}
		if (PlaySession* session = state.Editor->GetPlay().GetSession(); session && state.Views->IsGameInputFocused() && !session->IsLockstep())
			session->GetInput().QueueDeviceEvent(event);
	}

	void EditorApp::OnShutdown()
	{
		// The final GPU wait can itself discover device loss. Keep the last committed snapshot claimable through it.
		PublishAutosave();
		m_State->Layer.reset();
		m_State->Panels.reset();
		m_State->Actions.reset();
		m_State->Controls.reset();
		m_State->ReleaseServer();
		if (GraphicsDevice* device = GetContext().GetGraphicsDevice())
			device->WaitForIdle();
		// Removing the hook waits for an already-running invocation; no hook can claim another snapshot afterwards.
		GetProcessContext().SetFatalErrorHook({});
		if (m_State->Saves)
			ENGINE_VERIFY(m_State->Saves->Reset(), "fatal hook must be quiescent before closing the project");
		if (m_State->Editor)
			m_State->Editor->SetLifecycleCallbacks({});
		m_State->Recovery.reset();
		m_State->Thumbnails.reset();
		m_State->Views.reset();
		m_Rendering.reset();
		m_State->Inspector.reset();
		m_State->Gizmos.reset();
		m_State->Saves.reset();
		m_State->Editor.reset();
		m_State->Baker.reset();
	}

	void EditorApp::OnSafePoint()
	{
		State& state = *m_State;
		if (state.Layer)
		{
			const Status ui = state.Layer->OnSafePoint(state.ElapsedSeconds);
			if (!ui)
				ENGINE_ERROR("Editor UI action: {}", ui.error());
		}
		if (state.Server != nullptr)
			state.Server->Pump();

		if (state.Batch != nullptr && state.Server != nullptr)
		{
			state.Batch->Advance(*state.Server);
			if (state.Batch->IsFinished())
			{
				if (const std::optional<Error>& failure = state.Batch->GetFailure())
				{
					ENGINE_ERROR("The {} run failed: {}", state.Options.Upgrade ? "upgrade" : "batch", failure->ToString());
					RequestExit(ExitCode::Failed);
				}
				else
				{
					ENGINE_INFO("The {} run completed {} request(s)", state.Options.Upgrade ? "upgrade" : "batch", state.Batch->GetResults().size());
					RequestExit(ExitCode::Success);
				}
			}
		}

		const Status services = UpdateHostServices();
		if (!services)
		{
			if (services.error().GetCode() == ErrorCode::Gpu)
			{
				RefreshFatalSnapshot();
				FatalError(FatalErrorKind::OutOfMemory, services.error().ToString());
			}
			const std::string error = services.error().ToString();
			if (error != state.LastHostError)
				ENGINE_ERROR("Editor host: {}", error);
			state.LastHostError = error;
		}
		else
			state.LastHostError.clear();
		PublishAutosave();
		// Test-only fault intent is consumed after publishing the post-dispatch scene. The next real submission takes
		// the normal device-loss path, so the fatal callback can recover this edit rather than a startup snapshot.
		if (std::exchange(state.DeviceLostQueued, false))
			if (GraphicsDevice* device = GetContext().GetGraphicsDevice())
				device->GetDiagnostics().SetDeviceLost(true);
		// A failed content action must not starve periodic recovery publication.
		if (state.Saves && state.Editor->HasProject())
		{
			const auto saved = state.Saves->Update(state.ElapsedSeconds - state.ProjectOpenedAt);
			if (!saved)
			{
				const std::string error = saved.error().ToString();
				if (error != state.LastAutosaveError)
				{
					if (saved.error().GetCode() == ErrorCode::Conflict)
						ENGINE_WARN("Cannot autosave a changed source: {}", error);
					else
						ENGINE_ERROR("Cannot autosave: {}", error);
				}
				state.LastAutosaveError = error;
			}
		}

		if (state.Editor != nullptr)
		{
			const PlaySession* session = state.Editor->GetPlay().GetSession();
			const std::optional<uint64_t> serial = session ? std::optional(session->GetSerial()) : std::nullopt;
			if (state.TimingSession != serial)
			{
				state.TimingSession = serial;
				state.TimingDroppedBase = GetFrameStatistics().DroppedSeconds;
			}
			if (const std::optional<int> exitCode = state.Editor->GetShutdownRequest())
				RequestExit(*exitCode);

			// The play session's loop (the project's FixedHz), its time scale and the unthrottled frames of a running
			// play.step reach the frame loop here, after the requests that changed them (§4.2, §13.6).
			const EditorPlayController& play = state.Editor->GetPlay();
			SetFrameLoopConfig(play.GetFrameLoopConfig().value_or(GetSpecification().Loop));
			SetFrameTimeScale(play.GetFrameTimeScale());
			SetFrameThrottleSuspended(play.IsFrameThrottleSuspended());
		}
	}

	void EditorApp::OnFixedStep(const SimStep& /*step*/)
	{
		// A running play session that is not in lockstep advances one tick per loop step (§4.2 step 5, §5.7); the session
		// counts its own ticks from 0.
		State& state = *m_State;
		if (state.Editor != nullptr)
			state.Editor->GetPlay().OnFixedStep();
	}

	void EditorApp::OnUpdate(const FrameTime& frame)
	{
		// Asset hot reload polls on the frame clock (a ManualClock when headless, so headless runs are deterministic).
		State& state = *m_State;
		state.ElapsedSeconds += frame.UnscaledDeltaTime;
		if (state.Editor != nullptr)
		{
			EditorPlayController& play = state.Editor->GetPlay();
			if (PlaySession* session = play.GetSession())
			{
				const auto size = state.Editor->GetViewportState().GetPixelSize(ViewportView::Game);
				if (size)
					session->SetViewSize(size->x, size->y);
			}
			// The play session's frame phase (§4.2 step 6, §5.7), then the editor's own frame work.
			play.OnUpdate(frame);
			state.Editor->Update(state.ElapsedSeconds);
		}

		if (m_Rendering)
			NoteShownScene();
		PublishAutosave();
		const auto maxFrames = GetSpecification().MaxFrames;
		state.FinalFrame = maxFrames && frame.FrameIndex + 1 == *maxFrames;
		if (state.FinalFrame && !m_Rendering && !WriteScreenshots())
			RequestExit(ExitCode::Failed);
	}

	// --- Stale mirrors (SceneRenderer.h; Docs/Decisions/0013-m8-decisions.md decision 7) ---------------------------------

	void EditorApp::NoteShownScene()
	{
		// The edit scene and the play session's scene, compared with the last frame's, plus the SceneOpened and
		// PlayStateChanged events since (ShownSceneTracker).
		Rendering& rendering = *m_Rendering;
		const EditorContext* editor = m_State->Editor.get();
		const PlaySession* session = editor != nullptr ? editor->GetPlay().GetSession() : nullptr;
		const Scene* editScene = editor != nullptr && editor->HasScene() ? &editor->GetScene() : nullptr;
		const Scene* sessionScene = session != nullptr ? &session->GetScene() : nullptr;
		if (rendering.ShownScenes.Update(editScene, sessionScene, GetContext().GetEventLog()))
			rendering.Mirrors.NoteSceneChange();
	}

	void EditorApp::CollectStaleMirrors()
	{
		// The previous frame's renders were executed (each frame's command list runs before the next frame begins): mirrors of
		// replaced asset versions go every frame, and after the first frame rendered with a newly shown scene, also what only
		// the previous scene used (StaleMirrorSchedule). The screenshots rendered in between count as uses.
		Rendering& rendering = *m_Rendering;
		const bool releaseUnused = rendering.Mirrors.TakeReleaseUnused();
		rendering.Cache->CollectStale(releaseUnused);
		if (const AssetManager* assets = GetContext().GetAssetManager())
			rendering.Pipelines->CollectStale(*assets, releaseUnused);
	}

	void EditorApp::OnRender(RenderContext& context)
	{
		if (!m_Rendering)
			return;
		CollectStaleMirrors();
		PublishAutosave();
		const Status rendered = m_State->Views->Render(context);
		m_Rendering->Mirrors.NoteRendered();
		if (!rendered && rendered.error().GetCode() == ErrorCode::Gpu)
		{
			RefreshFatalSnapshot();
			FatalError(FatalErrorKind::OutOfMemory, rendered.error().ToString());
		}
		if (!rendered && !m_Rendering->IsRenderFailing)
			ENGINE_ERROR("Cannot render editor views: {}", rendered.error());
		m_Rendering->IsRenderFailing = !rendered;
	}
	void EditorApp::OnImGuiRender()
	{
		if (!m_State->Layer)
			return;
		const Status drawn = m_State->Layer->OnImGuiRender();
		if (!drawn)
			ENGINE_ERROR("Cannot draw editor UI: {}", drawn.error());
		m_State->UiConstructed = true;
	}
	void EditorApp::OnRenderSubmitted(uint64_t frameIndex, uint64_t submissionId)
	{
		if (m_State->Views)
		{
			const Status picked = m_State->Views->Submitted(frameIndex, submissionId);
			if (!picked)
				ENGINE_ERROR("Cannot request editor pick: {}", picked.error());
		}
		if (m_State->UiConstructed && m_State->Editor)
		{
			m_State->Editor->GetUiState().CompleteFrame();
			if (m_State->Views->IsLayoutReady())
			{
				m_State->CaptureReadyFrame = m_State->Editor->GetUiState().GetCompletedFrame();
				m_State->FreshUiRequested = false;
			}
			else if (m_State->FreshUiRequested)
			{
				const Status requested = RequestOffscreenUiFrame();
				if (!requested)
					ENGINE_ERROR("Cannot finish resized editor frame: {}", requested.error());
			}
			m_State->UiConstructed = false;
		}
		if (m_State->FinalFrame && !m_State->ScreenshotsWritten)
		{
			m_State->ScreenshotsWritten = true;
			if (!WriteScreenshots())
				RequestExit(ExitCode::Failed);
		}
	}

	Result<RenderSnapshot> EditorApp::ExtractViewportView(uint32_t width, uint32_t height)
	{
		if (!m_State->Views)
			return RenderSnapshot{};
		return m_State->Views->Extract(ViewportView::Scene, width, height);
	}

	Result<Image> EditorApp::CaptureView(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request)
	{
		PublishAutosave();
		if (m_Rendering == nullptr)
			return MakeError(ErrorCode::Unsupported, "screenshots need a renderer (not --renderer none)");
		// §8.13: screenshots render after the asset manager has published every load and reload requested so far.
		if (AssetManager* assets = GetContext().GetAssetManager())
			assets->WaitIdle();
		auto captured = m_Rendering->Capture->Capture(request, snapshot);
		if (!captured && captured.error().GetCode() == ErrorCode::Gpu)
		{
			RefreshFatalSnapshot();
			FatalError(FatalErrorKind::OutOfMemory, captured.error().ToString());
		}
		return captured;
	}

	Result<Image> EditorApp::CaptureViewport(uint32_t width, uint32_t height)
	{
		if (m_Rendering == nullptr)
			return MakeError(ErrorCode::Unsupported, "screenshots need a renderer (not --renderer none)");
		ENGINE_TRY_ASSIGN(const RenderSnapshot snapshot, ExtractViewportView(width, height));
		return CaptureView(snapshot, { .Width = width, .Height = height, .MaxDimension = 0 });
	}

	Result<Image> EditorApp::CaptureEditorUi()
	{
		PublishAutosave();
		GraphicsDevice* device = GetContext().GetGraphicsDevice();
		ImGuiLayer* imgui = GetImGuiLayer();
		if (device == nullptr || imgui == nullptr)
			return MakeError(ErrorCode::Unsupported, "screenshots need a renderer (not --renderer none)");
		auto captured = CaptureImGuiScreenshot(*device, *imgui, {});
		if (!captured && captured.error().GetCode() == ErrorCode::Gpu)
		{
			RefreshFatalSnapshot();
			FatalError(FatalErrorKind::OutOfMemory, captured.error().ToString());
		}
		return captured;
	}

	bool EditorApp::WriteScreenshots()
	{
		const auto write = [](std::string_view what, const std::filesystem::path& path, const Result<Image>& image)
		{
			if (!image.has_value())
			{
				ENGINE_ERROR("Cannot capture the {} screenshot: {}", what, image.error());
				return false;
			}
			const Status written = WritePng(path, *image);
			if (!written.has_value())
			{
				ENGINE_ERROR("Cannot write the {} screenshot: {}", what, written.error());
				return false;
			}
			ENGINE_INFO("Wrote the {} screenshot ({}x{})", what, image->Width, image->Height);
			return true;
		};

		bool succeeded = true;
		if (!m_Options.ViewportScreenshotPath.empty())
		{
			const Result<Image> image = CaptureViewport(DefaultViewportScreenshotWidth, DefaultViewportScreenshotHeight);
			succeeded = write("viewport", m_Options.ViewportScreenshotPath, image) && succeeded;
		}
		if (!m_Options.EditorScreenshotPath.empty())
			succeeded = write("editor", m_Options.EditorScreenshotPath, CaptureEditorUi()) && succeeded;
		return succeeded;
	}

	Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments)
	{
		const std::vector<CommandLineOption> options = Utils::GetEditorAppCommandLineOptions();
		ENGINE_TRY_ASSIGN(CommandLine commandLine, CommandLine::Parse(arguments, options));
		if (!commandLine.GetPositional().empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "unexpected argument '{}': the editor takes no positional arguments",
				commandLine.GetPositional().front());
		}
		ENGINE_TRY_ASSIGN(const EditorLaunchOptions launch, ParseEditorLaunchOptions(commandLine));

		ApplicationSpecification specification;
		specification.Name = ENGINE_PRODUCT_NAME;
		// The editor's automation param and result structs join the context's type registry before it freezes (§5.4, §13.4).
		specification.RegisterTypes = &RegisterEditorMethodTypes;
		specification.EnableImGui = true;
		specification.ImGuiIniPath = "Editor/imgui.ini";
		// Development builds mount engine:// at <repo>/Resources and enginecache:// at <repo>/bin/EngineCache, or at
		// --engine-cache-dir (§2.2, §7.5; the editor has no Dist build, so it always is one).
		specification.EngineResourcesDirectory = Utils::GetEditorRepositoryRoot() / "Resources";
		specification.EngineCacheDirectory = launch.EngineCacheDirectory.value_or(Utils::GetEditorRepositoryRoot() / "bin" / "EngineCache");
		ENGINE_TRY(ApplyEngineCommandLine(commandLine, specification));
		// Batch runs are among §4.2's unthrottled modes (ADR 0008 decision 4).
		if (launch.BatchFile.has_value() || launch.Upgrade)
			specification.ThrottleHeadless = false;

		EditorAppOptions editorOptions;
		ENGINE_TRY_ASSIGN(editorOptions.ViewportScreenshotPath, Utils::GetPathOption(commandLine, Utils::ViewportScreenshotOption));
		ENGINE_TRY_ASSIGN(editorOptions.EditorScreenshotPath, Utils::GetPathOption(commandLine, Utils::EditorScreenshotOption));
		const bool anyScreenshot = !editorOptions.ViewportScreenshotPath.empty() || !editorOptions.EditorScreenshotPath.empty();
		if (anyScreenshot && !specification.MaxFrames.has_value())
			return MakeError(ErrorCode::InvalidArgument, "the screenshot options are written after the last frame and need --frames");
		if (!editorOptions.EditorScreenshotPath.empty() && specification.MaxFrames.value_or(0) < 2)
		{
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs --frames 2 or more to measure and render the editor layout",
				Utils::EditorScreenshotOption);
		}
		return CreateScope<EditorApp>(std::move(specification), std::move(editorOptions));
	}

}
