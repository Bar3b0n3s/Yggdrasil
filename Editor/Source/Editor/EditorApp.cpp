#include "EditorPCH.h"
#include "Editor/EditorApp.h"

#include "Editor/EditorLayer.h"
#include "Editor/EditorPanelContext.h"
#include "Editor/Private/EditorHostAutosave.h"
#include "Editor/Private/EditorHostClose.h"
#include "Editor/Private/EditorHostErrorLog.h"
#include "Editor/Private/EditorHostPreferences.h"
#include "Editor/Private/EditorHostRecovery.h"
#include "Editor/Private/EditorHostSource.h"
#include "Editor/Private/EditorHostThumbnails.h"
#include "Editor/Private/EditorHostViewports.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Audio/AudioPreview.h"
#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/BatchRunner.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorCommandLine.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/EngineAssetGenerators.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "EditorCore/Reference/ReferenceGenerator.h"
#include "EditorCore/ShownSceneTracker.h"
#include "EditorCore/Viewport/GizmoController.h"
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
#include "Engine/Core/EventLog.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/ImGui/ImGuiScreenshot.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Window.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/StaleMirrorSchedule.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Testing/TestResults.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
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
		EditorHostPreferences Preferences{};
		// The recent-project snapshot (Preferences) is read again at the next safe point: set by each project open and close.
		bool RecentProjectsStale = false;
		Scope<EditorHostClose> Close; // the window-close confirmation; uses Actions and Saves
		uint64_t ProjectGeneration = 0;
		bool DeviceLostQueued = false;
		const LoadedProject* BoundProject = nullptr; // identity only; cleared before project close
		std::vector<std::filesystem::path> ContentDrops{};
		std::optional<AssetHandle> AudioRequest{};
		EditorSourceLauncher Sources{}; // detaches the user's external editors when the host is destroyed
		double ProjectOpenedAt = 0.0;
		bool UiConstructed = false;
		bool FreshUiRequested = false;
		uint64_t CaptureReadyFrame = 0;
		uint64_t CaptureReadyWithdrawals = 0; // EditorHostViewports' texture withdrawals when CaptureReadyFrame completed
		bool ScreenshotsWritten = false;
		bool FinalFrame = false;
		float UiScale = 0.0f;
		std::optional<uint64_t> TimingSession{};
		double TimingDroppedBase = 0.0;
		// Recurring failures are logged once per distinct error, never every frame.
		EditorHostErrorLog HostErrors{};
		EditorHostErrorLog AutosaveErrors{};
		EditorHostErrorLog UiActionErrors{};
		EditorHostErrorLog UiDrawErrors{};
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

		// The console summary of a --run-tests result: the totals, every failure of every row that did not pass (with its
		// location) and each coverage gate's outcome. The reports hold everything else; a full FeatureTest result lists
		// thousands of coverage counters, which the console never repeats.
		static void LogTestRunSummary(const TestRunResult& result)
		{
			size_t failed = 0, skipped = 0;
			for (const TestCaseResult& row : result.Cases)
			{
				skipped += row.Status == "skipped" ? 1 : 0;
				const bool passed = row.Failures.empty() && (row.Status == "passed" || row.Status == "skipped" || (row.Status == "quit" && row.QuitExpected));
				if (passed)
					continue;
				++failed;
				if (row.Failures.empty())
					ENGINE_WARN("Test {} {}/{}: {}:{}: {}", row.Status, row.Suite, row.Case, row.File, row.Line, row.Message);
				for (const TestFailureResult& failure : row.Failures)
					ENGINE_WARN("Test {} {}/{}: {}:{}: {}", row.Status, row.Suite, row.Case, failure.File, failure.Line, failure.Message);
			}
			for (const TestCoverageGateResult& gate : result.Coverage.Gates)
			{
				if (!gate.Evaluated)
					ENGINE_INFO("Coverage gate {}: not evaluated (only an unfiltered, non-recording run that completes evaluates it)", gate.Gate);
				else if (gate.Passed)
					ENGINE_INFO("Coverage gate {}: passed", gate.Gate);
				else
					ENGINE_WARN("Coverage gate {}: {} uncovered (first: {})", gate.Gate, gate.Missing.size(), gate.Missing.empty() ? std::string() : gate.Missing.front());
			}
			ENGINE_INFO("Test run {}: {} case(s), {} not passed, {} skipped, {} suite(s) and replay(s)", result.Passed ? "passed" : "failed", result.Cases.size(), failed,
				skipped, result.Suites.size());
		}

		// --run-tests' reports (ADR 0022 decision 7): the JUnit report at --junit, else <working directory>/bin/TestResults/
		// FeatureTest-Editor.xml, and the JSON report beside it with the extension replaced by .json, both written from the
		// run's result, so they never depend on the automation server's offload directory or its cleanup at exit. Their
		// jsonPath and junitPath name these files. Logs the run's summary first.
		[[nodiscard]] static Status WriteTestRunReportFiles(TestRunResult result, const std::optional<std::filesystem::path>& junitOption,
			const TypeRegistry& registry)
		{
			LogTestRunSummary(result);
			std::filesystem::path junit = junitOption.value_or(std::filesystem::path("bin") / "TestResults" / "FeatureTest-Editor.xml");
			std::error_code error;
			if (std::filesystem::path absolute = std::filesystem::absolute(junit, error); !error)
				junit = std::move(absolute);
			std::filesystem::path json = junit;
			json.replace_extension(".json");
			result.JunitPath = FileSystem::PathToUtf8(junit);
			result.JsonPath = FileSystem::PathToUtf8(json);
			ENGINE_TRY_ASSIGN(const std::string xml, TestRunResultToJUnit(result));
			ENGINE_TRY_ASSIGN(const Json document, TestRunResultToJson(result, registry));
			ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(document));
			ENGINE_TRY(FileSystem::CreateDirectories(junit.parent_path()));
			ENGINE_TRY(FileSystem::WriteFileAtomic(junit, std::as_bytes(std::span(xml.data(), xml.size()))));
			ENGINE_TRY(FileSystem::WriteFileAtomic(json, std::as_bytes(std::span(text.data(), text.size()))));
			ENGINE_INFO("Test reports: '{}' and '{}'", result.JunitPath, result.JsonPath);
			return {};
		}

		// The reports of a test.run result (the RPC result, or the "testRun" member of an error's data).
		[[nodiscard]] static Status WriteTestRunReports(const JsonReader& response, const std::optional<std::filesystem::path>& junitOption,
			const TypeRegistry& registry)
		{
			const StructInfo* type = registry.FindStruct<TestRunResult>();
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "TestRunResult is not registered");
			TestRunResult result;
			ENGINE_TRY(WithContext(type->FromJson(&result, response, {}), "while reading the test.run result"));
			return WriteTestRunReportFiles(std::move(result), junitOption, registry);
		}

		// The reports of a --run-tests run whose test.run failed ("Written whatever the outcome", EditorCommandLine.h): the
		// result test.run published with its error when its cases ran (error.data.testRun: a recording verification or
		// coverage capture failed after them, TestMethods.cpp), otherwise a failed run naming `failure`
		// (MakeFailedTestRunResult: a preflight error such as script type errors, a refused admission or a failed Begin).
		// Either way the files at the report paths describe this run, so a report of an earlier run never survives there.
		[[nodiscard]] static Status WriteFailedTestRunReports(const Json& failedResponse, const Error& failure,
			const std::optional<std::filesystem::path>& junitOption, const TypeRegistry& registry)
		{
			const JsonReader response(failedResponse);
			if (const std::optional<JsonReader> error = response.FindMember("error"))
			{
				if (const std::optional<JsonReader> data = error->FindMember("data"))
				{
					if (const std::optional<JsonReader> run = data->FindMember("testRun"))
						return WriteTestRunReports(*run, junitOption, registry);
				}
			}
			return WriteTestRunReportFiles(MakeFailedTestRunResult(TestSuiteSettings::Mode::Editor, failure.ToString()), junitOption, registry);
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
			ENGINE_TRY(DumpReferences(app.GetContext().GetTypeRegistry(), app.GetContext().GetScriptApiRegistry(), *options.DumpReferenceDirectory));
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
		// --id-seed (ADR 0022 decision 8): a committed scaffold's batch run writes the same entity ids and asset handles on
		// every machine; without it the generator is seeded from the OS CSPRNG.
		if (options.IdSeed.has_value())
			editorSpecification.IdGeneratorState = Random(*options.IdSeed).GetState();
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
		state.Thumbnails = CreateScope<EditorHostThumbnails>(*state.Editor, [&app](const RenderSnapshot& snapshot, uint32_t size) -> Result<Image>
		{
			// Unlike CaptureView, a thumbnail never waits for every pending load and import (AssetManager::WaitIdle): the
			// cache renders only after its own inputs are loaded, and skips requests whose inputs are still loading.
			if (app.m_Rendering == nullptr)
				return MakeError(ErrorCode::Unsupported, "thumbnails need a renderer (not --renderer none)");
			auto captured = app.m_Rendering->Capture->Capture(ViewportScreenshotRequest{ .Width = size, .Height = size }, snapshot);
			if (!captured && captured.error().GetCode() == ErrorCode::Gpu)
			{
				app.RefreshFatalSnapshot();
				FatalError(FatalErrorKind::OutOfMemory, captured.error().ToString());
			}
			return captured;
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
		else if (options.CheckScripts || options.RunTests)
		{
			Json params = Json::object();
			if (options.RunTests && !options.TestFilter.empty())
				params["filter"] = options.TestFilter;
			std::vector<BatchRequest> requests = { BatchRequest{ .Method = options.CheckScripts ? "script.check" : "test.run", .Params = VariantValue(std::move(params)), .Line = 0 } };
			state.Batch = CreateScope<BatchRunner>(std::move(requests), BatchRunOptions{ .ClientName = "cli", .WriteTranscript = false });
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
			captures.RequestUiFrame = [this]() -> Status
			{
				m_State->FreshUiRequested = true;
				if (m_State->Layer)
					m_State->Layer->RequestFrame();
				return WithContext(RequestOffscreenUiFrame(), "while requesting a fresh editor UI frame");
			};
			captures.IsUiFrameCurrent = [this]()
			{
				return m_State->Views == nullptr || m_State->Views->GetTextureWithdrawalCount() == m_State->CaptureReadyWithdrawals;
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
		if (GetImGuiLayer() != nullptr)
			ENGINE_TRY(Utils::LoadEditorFont(state.Editor->GetVfs()));
		// A malformed or newer user://Editor.json, or a listener that cannot start, never stops the editor: the defaults
		// apply and the Automation panel shows the failure.
		if (const Status loaded = state.Preferences.Load(state.Editor->GetVfs()); !loaded)
			ENGINE_WARN("Editor preferences: {}", loaded.error());
		const Status listening = state.Preferences.StartListening([&state](bool allowed)
		{
			return state.Server->SetPreferenceListening(allowed);
		});
		if (!listening)
			ENGINE_WARN("Editor preferences: {}", listening.error());
		state.Preferences.RefreshRecentProjects(state.Editor->GetVfs());
		state.Actions = CreateScope<EditorActions>(*state.Editor, *state.Server);
		state.Close = CreateScope<EditorHostClose>(*state.Editor, *state.Actions, *state.Saves);
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
			return m_State->Preferences.GetAutomationState();
		};
		state.Panels->AutomationPreferences.QueueChange = [this](bool allowed) -> Status
		{
			return m_State->Preferences.QueueAutomationChange(allowed);
		};
		state.Panels->ExternalEditor.GetState = [this]()
		{
			return m_State->Preferences.GetExternalEditorState();
		};
		state.Panels->ExternalEditor.QueueChange = [this](std::string executable, std::vector<std::string> arguments) -> Status
		{
			return m_State->Preferences.QueueExternalEditorChange(std::move(executable), std::move(arguments));
		};
		state.Panels->GetRecentProjects = [this]()
		{
			return m_State->Preferences.GetRecentProjects();
		};
		// Configuration and path errors return to the clicked panel; the launch itself starts at the next safe point.
		state.Panels->OpenSource = [this](const std::filesystem::path& path, uint32_t line) -> Status
		{
			const EditorContext& editor = *m_State->Editor;
			ENGINE_TRY_ASSIGN(EditorPreferences preferences, ReadEditorPreferences(editor.GetVfs()));
			return m_State->Sources.Queue({
				.Source = path,
				.Line = line,
				.ProjectRoot = editor.HasProject() ? editor.GetProject().GetRoot() : std::filesystem::path{},
				.RepositoryRoot = Utils::GetEditorRepositoryRoot(),
				.Preferences = std::move(preferences),
				.PreferencesFile = GetProcessContext().GetUserDataPaths().Root / "Editor.json",
			});
		};
		// The layer's constructor registers its imgui.ini open-panel handler in the current Dear ImGui context (the
		// ImGuiLayer's, before the first frame loads the ini; none with --renderer none) and otherwise builds CPU models only.
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
		state.RecentProjectsStale = true; // project.open and project.create move the project to the front of the list
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
		state.Sources.Cancel();
		state.Recovery->Reset();
		state.BoundProject = nullptr;
		state.RecentProjectsStale = true; // the launcher reappears and shows the list
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
		if (m_State->Saves)
			ReportAutosave(m_State->Saves->Publish());
	}
	void EditorApp::ReportAutosave(const Status& status)
	{
		const bool conflict = !status && status.error().GetCode() == ErrorCode::Conflict;
		m_State->AutosaveErrors.Report(conflict ? "Autosave waits for the source conflict to be resolved" : "Cannot autosave", status,
			conflict ? EditorHostErrorLevel::Warning : EditorHostErrorLevel::Error);
	}
	void EditorApp::RefreshFatalSnapshot()
	{
		// Only the publication: the fatal hook that follows writes it once (Autosave::WriteFatalSnapshot). Writing here as
		// well would duplicate the generation and evict an older one, possibly another scene's only recovery.
		PublishAutosave();
	}
	Status EditorApp::UpdateHostServices()
	{
		State& state = *m_State;
		if (!state.Editor)
			return {};
		ENGINE_TRY(SynchronizeProject());
		if (std::exchange(state.RecentProjectsStale, false))
			state.Preferences.RefreshRecentProjects(state.Editor->GetVfs());
		ENGINE_TRY(state.Preferences.Pump([&state](bool allowed)
		{
			return state.Controls->SetAllowAiAutomation(allowed);
		}));
		// The Automation panel shows a failed write; the other host services still run this safe point.
		if (const Status written = state.Preferences.PumpExternalEditor(state.Editor->GetVfs()); !written)
			ENGINE_ERROR("Cannot save the external editor preference: {}", written.error());
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
		// Each failed launch is reported once; the other queued launches still start.
		if (const Status launched = state.Sources.Pump(); !launched)
			ENGINE_ERROR("Cannot open the source editor: {}", launched.error());

		if (state.Thumbnails)
			ENGINE_TRY(state.Thumbnails->Pump());
		return {};
	}
	void EditorApp::OnEvent(Event& event)
	{
		State& state = *m_State;
		if (!state.Editor)
			return;
		// A close with unsaved scene changes waits for Save / Don't save / Cancel instead of ending the frame loop, when an
		// ImGui layer can show that prompt (EditorHostClose closes at once otherwise). Active gestures end first, so the
		// choice saves or records committed state only.
		if (auto* close = std::get_if<WindowCloseEvent>(&event); close != nullptr && state.Close && state.Close->RequestClose(GetImGuiLayer() != nullptr))
		{
			close->Handled = true;
			state.Gizmos->Cancel();
			state.Inspector->Cancel();
		}
		if (auto* drop = std::get_if<FileDropEvent>(&event))
		{
			if (state.Editor->HasProject())
			{
				for (const auto& path : drop->Paths)
					if (IsValidUtf8(path))
						state.ContentDrops.push_back(FileSystem::PathFromUtf8(path));
				// Immediate feedback: the Content panel opens and takes focus, then imports the drop or asks where it goes.
				if (state.Layer)
					if (const Status revealed = state.Layer->RevealPanel(EditorPanel::ContentBrowser); !revealed)
						ENGINE_WARN("Cannot show the Content panel for dropped files: {}", revealed.error());
			}
			drop->Handled = true;
			return;
		}
		if (const auto* resized = std::get_if<WindowResizeEvent>(&event))
		{
			if (resized->FramebufferWidth == 0 || resized->FramebufferHeight == 0)
				state.Views->SetUnavailable();
			else
				state.Views->SetAvailable();
		}
		if (const auto* focused = std::get_if<WindowFocusEvent>(&event))
			if (!focused->Focused)
			{
				state.Views->SetGameInputFocused(false);
				state.Gizmos->Cancel();
			}
		// While the Game view owns input, device events reach the session in its view coordinates (the Game panel's image),
		// never in editor-window coordinates; anything else still reaches the controller, which releases the play cursor on blur.
		const std::optional<Event> game = state.Views->ToGameEvent(event);
		state.Editor->GetPlay().OnInputEvent(game.value_or(event), game.has_value());
	}

	void EditorApp::OnShutdown()
	{
		// The final GPU wait can itself discover device loss. Keep the last committed snapshot claimable through it.
		PublishAutosave();
		// The layout's last save: the current imgui.ini (the project's, or the launcher's in user data) is written while the
		// layer's open-panel handler is registered and this process still holds the project's lock. Dear ImGui then keeps
		// no file, so neither the layer's destruction nor the context's saves again: a save without the handler would drop
		// the open panels, and one after the lock is released could write into a project another editor has opened.
		if (ImGuiLayer* imgui = GetImGuiLayer())
			if (const Status stopped = imgui->SetIniFilePath({}); !stopped)
				ENGINE_WARN("Cannot save the editor layout at shutdown: {}", stopped.error());
		m_State->Close.reset();
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
		if (GetImGuiLayer() != nullptr && GetContext().GetWindow() != nullptr)
		{
			const Window& window = *GetContext().GetWindow();
			const float framebufferScale = window.GetWidth() != 0 && window.GetFramebufferWidth() != 0
				? static_cast<float>(window.GetFramebufferWidth()) / static_cast<float>(window.GetWidth())
				: 1.0f;
			const float scale = Utils::EditorDisplayScale(window.GetContentScale().x, framebufferScale);
			if (state.UiScale != scale)
			{
				Utils::ApplyEditorStyle(scale);
				state.UiScale = scale;
			}
		}
		if (state.Layer)
			state.UiActionErrors.Report("Editor UI action", state.Layer->OnSafePoint(state.ElapsedSeconds));
		// After the layer pumped EditorActions: a confirmed close completes here, outside ImGui traversal.
		if (state.Close && state.Close->Pump())
			RequestExit(ExitCode::Success);
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
					if (state.Options.RunTests)
					{
						const Status written = Utils::WriteFailedTestRunReports(state.Batch->GetFailedResponse(), *failure, state.Options.TestJunitPath,
							GetContext().GetTypeRegistry());
						if (!written)
							ENGINE_ERROR("Cannot write the test reports: {}", written.error().ToString());
					}
					RequestExit(ExitCode::Failed);
				}
				else
				{
					bool passed = true;
					if (state.Options.CheckScripts || state.Options.RunTests)
					{
						const auto& results = state.Batch->GetResults();
						passed = !results.empty() && JsonReader(results.back()).ReadMember<bool>("passed").value_or(false);
						if (state.Options.RunTests && !results.empty())
						{
							const Status written = Utils::WriteTestRunReports(JsonReader(results.back()), state.Options.TestJunitPath, GetContext().GetTypeRegistry());
							if (!written)
							{
								ENGINE_ERROR("Cannot write the test reports: {}", written.error().ToString());
								passed = false;
							}
						}
						else if (!results.empty())
						{
							const auto text = JsonWriter::Write(results.back(), JsonStyle::Pretty);
							if (text)
								ENGINE_INFO("{}", *text);
						}
					}
					ENGINE_INFO("The {} run completed {} request(s)", state.Options.Upgrade ? "upgrade" : "batch", state.Batch->GetResults().size());
					RequestExit(passed ? ExitCode::Success : ExitCode::Failed);
				}
			}
		}

		// UI/RPC handlers and pending-operation polls have released all borrowed session/VM pointers.
		if (state.Editor)
			state.Editor->GetPlay().OnSafePoint();

		const Status services = UpdateHostServices();
		if (!services && services.error().GetCode() == ErrorCode::Gpu)
		{
			RefreshFatalSnapshot();
			FatalError(FatalErrorKind::OutOfMemory, services.error().ToString());
		}
		state.HostErrors.Report("Editor host", services);
		// The safe point's one publication of the post-dispatch scene, then a periodic write when its interval elapsed
		// (Autosave::Update publishes first). A failed content action must not starve periodic recovery publication.
		if (state.Saves && state.Editor->HasProject())
		{
			const auto saved = state.Saves->Update(state.ElapsedSeconds - state.ProjectOpenedAt);
			ReportAutosave(saved ? Status() : Status(std::unexpected(saved.error())));
		}
		else
			PublishAutosave();
		// Test-only fault intent is consumed after publishing the post-dispatch scene. The next real submission takes
		// the normal device-loss path, so the fatal callback can recover this edit rather than a startup snapshot.
		if (std::exchange(state.DeviceLostQueued, false))
			if (GraphicsDevice* device = GetContext().GetGraphicsDevice())
				device->GetDiagnostics().SetDeviceLost(true);

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
			// Ordinary live play's view (scripts' view coordinates) follows the Game panel's image; deterministic sessions
			// (test runs, recording, replay, lockstep) keep their fixed view, so their results never depend on the panel.
			play.SyncGameView();
			// The play session's frame phase (§4.2 step 6, §5.7), then the editor's own frame work.
			play.OnUpdate(frame);
			state.Editor->Update(state.ElapsedSeconds);
		}

		if (m_Rendering)
			NoteShownScene();
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
		// The frame's edit scene was published at its safe point; nothing between it and this GPU work commits edits.
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
		m_State->UiDrawErrors.Report("Cannot draw editor UI", m_State->Layer->OnImGuiRender());
		if (m_State->Close)
			m_State->Close->Draw();
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
				m_State->CaptureReadyWithdrawals = m_State->Views->GetTextureWithdrawalCount();
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
		// ADR 0019 decision 6, §10.1: every editor constructs its AudioEngine with deterministic decoding from startup, the
		// windowed one included (keeping its device, also one --audio-device chose), so test.run, test.list and --run-tests
		// work in the normal editor without replacing the resource manager underneath loaded clips.
		AudioEngineSpecification audio = specification.Audio.value_or(GetDefaultAudioSpecification(specification.Window));
		audio.Decoding = AudioDecoding::Deterministic;
		specification.Audio = audio;
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
