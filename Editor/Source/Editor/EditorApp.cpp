#include "EditorPCH.h"
#include "Editor/EditorApp.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/BatchRunner.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/EditorCommandLine.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
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
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>

#include <array>
#include <format>
#include <vector>

#if !defined(ENGINE_REPO_ROOT)
	#error "ENGINE_REPO_ROOT must be defined for the editor (Dependencies.lua, ApplyFirstPartySettings): it finds Resources and Docs there"
#endif

namespace Engine {

	struct EditorApp::State
	{
		EditorLaunchOptions Options{};
		Scope<EditorContext> Editor; // destroyed last (reverse member order): the server and the run use it
		Scope<AutomationServer> Server;
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
		Scope<ViewportCapture> Capture;           // screenshots (CaptureView)
		Scope<SceneRenderer> Viewport;            // the view drawn under the UI every frame
		Scope<BlitPass> Blit;                     // created for the frame target's format on the first frame (OnRender)
		RenderSnapshot ViewSnapshot{};            // the view OnUpdate extracted for this frame (outside Play mode)
		bool UsesSessionView = false;             // this frame shows the play session's last extraction instead
		nvrhi::FramebufferInfo BlitFramebuffer{}; // the target format Blit was created for
		bool IsViewFailing = false;               // a failed extraction is logged once per run of failing frames
		bool IsRenderFailing = false;             // a failed render or blit is logged once per run of failing frames
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
				.Description = "After the last frame of a --frames run, write a 640x360 screenshot of the viewport (the view under the UI) to "
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

		// The editor's fatal-error hook (ProcessContext::SetFatalErrorHook): the place of the autosave (§4.13, §8.14 item 6),
		// which arrives with EditorCore/Autosave (M10). It may run on any thread, so it reads no editor state.
		static void OnEditorFatalError(void* /*userData*/, FatalErrorKind kind, std::string_view /*message*/)
		{
			ENGINE_WARN("Editor fatal-error hook ({}): nothing is autosaved before EditorCore/Autosave (M10)", FatalErrorKindToString(kind));
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
		// into enginecache:// with the built-in importers. There is no environment baker and there are no generators before
		// M8, so those entries are skipped with a warning. Returns the exit code: Success when every entry is baked, up to
		// date or skipped with a warning, Failed when an entry or the run failed (each failure logged at Error).
		[[nodiscard]] static int BakeEngineResources(EngineContext& context)
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
				.EnvironmentBaker = nullptr,
				.Generators = {},
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
		// The bake needs no project and no editor state: a --project given with it is not opened.
		if (options.BakeEngineAssets)
		{
			app.RequestExit(Utils::BakeEngineResources(app.GetContext()));
			return {};
		}

		const std::filesystem::path repository = Utils::GetEditorRepositoryRoot();
		const UserDataPaths& userData = app.GetProcessContext().GetUserDataPaths();
		EditorContextSpecification editorSpecification;
		editorSpecification.TemplatesDirectory = repository / "Resources" / "Templates" / "Projects";
		editorSpecification.ReadOnlyCacheRoot = userData.Root / "ReadOnlyCache";
		ENGINE_TRY_ASSIGN(state.Editor, EditorContext::Create(app.GetContext(), editorSpecification));

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
		if (listens || options.IsOneShot())
		{
			AutomationServerSpecification serverSpecification;
			serverSpecification.Listen = listens;
			serverSpecification.Port = options.AutomationPort.value_or(0);
			serverSpecification.TestHooks = options.AutomationTestHooks;
			serverSpecification.Headless = headless;
			serverSpecification.RendererName = std::string(RendererModeToCommandLine(app.GetSpecification().Renderer));
			serverSpecification.Screenshots = std::move(captures);
			serverSpecification.SystemErrors = std::move(systemErrors);
			serverSpecification.SessionsDirectory = userData.Root / "Automation" / "Sessions";
			serverSpecification.DocsRoot = repository;
			serverSpecification.ExportBinaryRoot = repository / "bin";
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
		GetProcessContext().SetFatalErrorHook({ .Function = &Utils::OnEditorFatalError, .UserData = nullptr });
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
			captures.EditorUi = [this]()
			{
				return CaptureEditorUi();
			};
			// A Vulkan error thrown out of NVRHI inside a method (vk::SystemError, §4.6 item 2) ends the process like the
			// frame-boundary catch (App/FrameLoop.cpp) would, instead of becoming an Internal response on a lost device.
			systemErrors = MakeVulkanSystemErrorHandler(GetContext());
		}

		ENGINE_TRY(m_State->Initialize(*this, std::move(captures), std::move(systemErrors)));
		return InitializeRendering();
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
		Window& window = *context.GetWindow();
		const SceneRendererSpecification viewport{
			.Width = std::max(window.GetFramebufferWidth(), 1U),
			.Height = std::max(window.GetFramebufferHeight(), 1U),
		};
		Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(*device, *rendering->Pipelines, *rendering->Cache, assets, viewport);
		if (!renderer.has_value())
			return std::unexpected(Utils::CheckStartupGpuObject(std::move(renderer).error(), "the viewport's scene renderer"));
		rendering->Viewport = std::move(*renderer);
		m_Rendering = std::move(rendering);
		return {};
	}

	void EditorApp::OnShutdown()
	{
		// The server first (its pending operations are cancelled against a live editor, and it holds the captures), then the
		// GPU objects (the cache refers to the editor's asset manager), then the editor (the project lock), then the hook.
		// Also what a failed OnInitialize has built, any part of which may be missing.
		m_State->ReleaseServer();
		m_Rendering.reset();
		m_State->Editor.reset();
		GetProcessContext().SetFatalErrorHook({});
	}

	void EditorApp::OnSafePoint()
	{
		State& state = *m_State;
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

		if (state.Editor != nullptr)
		{
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
		const Window& window = *GetContext().GetWindow();
		const uint32_t width = window.GetFramebufferWidth();
		const uint32_t height = window.GetFramebufferHeight();
		if (state.Editor != nullptr)
		{
			// The game view is extracted at the window's size (§5.7 frame phase step 4); a minimized window keeps the last.
			EditorPlayController& play = state.Editor->GetPlay();
			if (PlaySession* session = play.GetSession(); session != nullptr && width > 0 && height > 0)
				session->SetViewSize(width, height);
			// The play session's frame phase (§4.2 step 6, §5.7), then the editor's own frame work.
			play.OnUpdate(frame);
			state.Editor->Update(state.ElapsedSeconds);
		}

		// The view of this frame, after everything that changes the scenes this frame.
		if (m_Rendering != nullptr && width > 0 && height > 0)
			PrepareViewportView(width, height);

		const std::optional<uint64_t> maxFrames = GetSpecification().MaxFrames;
		if (maxFrames.has_value() && frame.FrameIndex + 1 == *maxFrames && !WriteScreenshots())
			RequestExit(ExitCode::Failed);
	}

	void EditorApp::PrepareViewportView(uint32_t width, uint32_t height)
	{
		Rendering& rendering = *m_Rendering;
		const PlaySession* session = m_State->Editor != nullptr ? m_State->Editor->GetPlay().GetSession() : nullptr;
		rendering.UsesSessionView = session != nullptr && session->GetMode() == PlayMode::Play;
		if (rendering.UsesSessionView)
			return; // the session extracted its game view in its frame phase
		Result<RenderSnapshot> view = ExtractViewportView(width, height);
		if (view.has_value())
		{
			rendering.ViewSnapshot = std::move(*view);
			rendering.IsViewFailing = false;
			return;
		}
		if (!rendering.IsViewFailing)
			ENGINE_ERROR("Cannot extract the viewport's view: {}", view.error());
		rendering.IsViewFailing = true;
		rendering.ViewSnapshot = RenderSnapshot{};
	}

	void EditorApp::OnRender(RenderContext& context)
	{
		if (m_Rendering == nullptr)
			return;
		Rendering& rendering = *m_Rendering;
		SceneRenderer& viewport = *rendering.Viewport;
		GpuProfileScope scope(*context.Profiler, *context.CommandList, "Viewport");
		// Render targets are created at startup or resize, and one the device has no memory for is fatal (§8.14 item 7).
		const Status resized = viewport.Resize(context.Width, context.Height);
		if (!resized.has_value())
			FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot resize the viewport's targets: {}", resized.error().ToString()));

		// The blit's pipeline is created for the frame target's format on the first frame that renders into it (§8.12, as
		// ImGui's), and again if the format changes.
		const nvrhi::FramebufferInfo& target = context.Framebuffer->getFramebufferInfo();
		if (rendering.Blit == nullptr || target != rendering.BlitFramebuffer)
		{
			Result<Scope<BlitPass>> blit = BlitPass::Create(*context.Device, *GetContext().GetPipelineFactory(), target);
			if (!blit.has_value())
			{
				const Error error = Utils::CheckStartupGpuObject(std::move(blit).error(), "the viewport's blit");
				if (!rendering.IsRenderFailing)
					ENGINE_ERROR("Cannot draw the viewport: {}", error);
				rendering.IsRenderFailing = true;
				return;
			}
			rendering.Blit = std::move(*blit);
			rendering.BlitFramebuffer = target;
		}

		// Play mode shows the session's game view, extracted in its frame phase; every other mode the view OnUpdate extracted.
		const PlaySession* session = m_State->Editor != nullptr ? m_State->Editor->GetPlay().GetSession() : nullptr;
		const RenderSnapshot& snapshot = rendering.UsesSessionView && session != nullptr ? session->GetLastExtraction() : rendering.ViewSnapshot;
		// A render error names a draw it skipped (a non-finite matrix); the rest of the view rendered and is shown.
		const Status rendered = viewport.Render(*context.CommandList, snapshot);
		const Status blitted = rendering.Blit->Record(*context.CommandList, *viewport.GetFinalTexture(), *context.Framebuffer);
		// A binding set the device has no memory for is fatal like any GPU object (§8.14 item 7); anything else is logged
		// once per run of failing frames.
		if (!blitted.has_value() && blitted.error().GetCode() == ErrorCode::Gpu)
			FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot draw the viewport: {}", blitted.error().ToString()));
		const Status& drawn = blitted.has_value() ? rendered : blitted;
		if (!drawn.has_value() && !rendering.IsRenderFailing)
			ENGINE_ERROR("Cannot draw the viewport: {}", drawn.error());
		rendering.IsRenderFailing = !drawn.has_value();
	}

	void EditorApp::OnImGuiRender()
	{
		ImGui::ShowDemoWindow();
	}

	Result<RenderSnapshot> EditorApp::ExtractViewportView(uint32_t width, uint32_t height)
	{
		EditorContext* editor = m_State->Editor.get();
		if (editor != nullptr)
		{
			// While playing, the game view (the session's primary camera); while simulating, the scene-view camera (§5.6).
			if (PlaySession* session = editor->GetPlay().GetSession())
			{
				RenderExtractionRequest request{ .Width = width, .Height = height };
				if (session->GetMode() == PlayMode::Simulate)
				{
					request.Camera = RenderCameraSource::Explicit;
					request.ExplicitCamera = editor->GetSceneViewCamera();
				}
				return session->ExtractView(request);
			}
			// The edit scene's world matrices are runtime-only components: recomputing them leaves its revision unchanged.
			if (editor->HasScene())
			{
				Scene& scene = editor->GetScene();
				TransformSystem::Update(scene);
				return ExtractRenderSnapshot(scene,
					{ .Camera = RenderCameraSource::Explicit, .ExplicitCamera = editor->GetSceneViewCamera(), .Width = width, .Height = height });
			}
		}
		// No scene: the empty view, cleared to the default clear colour.
		RenderSnapshot empty;
		empty.Camera.ViewportWidth = width;
		empty.Camera.ViewportHeight = height;
		return empty;
	}

	Result<Image> EditorApp::CaptureView(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request)
	{
		if (m_Rendering == nullptr)
			return MakeError(ErrorCode::Unsupported, "screenshots need a renderer (not --renderer none)");
		// §8.13: screenshots render after the asset manager has published every load and reload requested so far.
		if (AssetManager* assets = GetContext().GetAssetManager())
			assets->WaitIdle();
		return m_Rendering->Capture->Capture(request, snapshot);
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
		GraphicsDevice* device = GetContext().GetGraphicsDevice();
		ImGuiLayer* imgui = GetImGuiLayer();
		if (device == nullptr || imgui == nullptr)
			return MakeError(ErrorCode::Unsupported, "screenshots need a renderer (not --renderer none)");
		return CaptureImGuiScreenshot(*device, *imgui, {});
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
			return MakeError(ErrorCode::InvalidArgument, "option '{}' shows the UI of the frame before the last and needs --frames 2 or more",
				Utils::EditorScreenshotOption);
		}
		return CreateScope<EditorApp>(std::move(specification), std::move(editorOptions));
	}

}
