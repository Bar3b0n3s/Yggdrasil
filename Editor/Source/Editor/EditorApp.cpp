#include "EditorPCH.h"
#include "Editor/EditorApp.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/BatchRunner.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/EditorCommandLine.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/CommandLine.h"
#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/ImGui/ImGuiScreenshot.h"
#include "Engine/Platform/Process.h"
#include "Engine/Renderer/ViewportCapture.h"

#include <imgui.h>
#include <vulkan/vulkan.hpp>

#include <array>
#include <format>
#include <system_error>
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
		Scope<BatchRunner> Batch; // --batch or --upgrade

		// OnInitialize's work for `app`, whose server gets `captures` and `systemErrors` (empty without a device); on
		// failure the caller releases what was built.
		[[nodiscard]] Status Initialize(EditorApp& app, ScreenshotCaptures captures, SystemErrorHandler systemErrors);
		// Releases the run, the server and the editor, in that order, while the engine context still exists.
		void Release();
	};

	namespace Utils {

		constexpr std::string_view ViewportScreenshotOption = "--viewport-screenshot";
		constexpr std::string_view EditorScreenshotOption = "--editor-screenshot";

		constexpr std::array EditorAppCommandLineOptions = {
			CommandLineOption{
				.Name = ViewportScreenshotOption,
				.Value = CommandLineValue::Required,
				.ValueName = "path",
				.Description = "After the last frame of a --frames run, write a 640x360 viewport screenshot to this PNG file.",
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

	void EditorApp::State::Release()
	{
		Batch.reset();
		Server.reset();
		Editor.reset();
	}

	Status EditorApp::OnInitialize()
	{
		GetProcessContext().SetFatalErrorHook({ .Function = &Utils::OnEditorFatalError, .UserData = nullptr });
		Status initialized = InitializeEditor();
		// Run calls OnShutdown only after a successful OnInitialize, and destroys the engine context and its device next:
		// what was built (the viewport capture's GPU objects, the editor the server refers to) and the hook go now.
		if (!initialized)
			OnShutdown();
		return initialized;
	}

	Status EditorApp::InitializeEditor()
	{
		// The viewport capture owns its pipeline from startup (§8.12); creating a pipeline at startup that the device has no
		// memory for is fatal (§8.14 item 7).
		EngineContext& context = GetContext();
		ScreenshotCaptures captures;
		SystemErrorHandler systemErrors;
		if (GraphicsDevice* device = context.GetGraphicsDevice())
		{
			Result<Scope<ViewportCapture>> capture = ViewportCapture::Create(*device, *context.GetPipelineFactory());
			if (!capture.has_value())
			{
				if (capture.error().GetCode() == ErrorCode::Gpu)
					FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot create the viewport capture: {}", capture.error().ToString()));
				return std::unexpected(std::move(capture).error().WithContext("while creating the viewport capture"));
			}
			m_ViewportCapture = std::move(*capture);

			// The captures and the system-error mapping refer to this application, which outlives the server (OnShutdown
			// releases the server first).
			captures.Viewport = [this](uint32_t width, uint32_t height)
			{
				return CaptureViewport(width, height);
			};
			captures.EditorUi = [this]()
			{
				return CaptureEditorUi();
			};
			// A Vulkan error thrown out of NVRHI inside a method (vk::SystemError, §4.6 item 2) ends the process like the
			// frame-boundary catch (App/FrameLoop.cpp) would, instead of becoming an Internal response on a lost device.
			systemErrors = [this](const std::system_error& error, std::string_view method)
			{
				if (error.code().category() == vk::errorCategory())
				{
					RaiseVulkanError(GetContext().GetGraphicsDevice(), static_cast<VkResult>(error.code().value()),
						std::format("Vulkan error in automation method '{}': {}", method, error.what()));
				}
			};
		}

		return m_State->Initialize(*this, std::move(captures), std::move(systemErrors));
	}

	void EditorApp::OnShutdown()
	{
		// The server first (its pending operations are cancelled against a live editor, and it holds the captures), then the
		// editor (the project lock), then the viewport capture, then the hook. Also what a failed OnInitialize has built,
		// any part of which may be missing.
		m_State->Release();
		m_ViewportCapture.reset();
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
		}
	}

	void EditorApp::OnUpdate(const FrameTime& frame)
	{
		const std::optional<uint64_t> maxFrames = GetSpecification().MaxFrames;
		if (maxFrames.has_value() && frame.FrameIndex + 1 == *maxFrames && !WriteScreenshots())
			RequestExit(ExitCode::Failed);
	}

	void EditorApp::OnImGuiRender()
	{
		ImGui::ShowDemoWindow();
	}

	Result<Image> EditorApp::CaptureViewport(uint32_t width, uint32_t height)
	{
		if (m_ViewportCapture == nullptr)
			return MakeError(ErrorCode::Unsupported, "screenshots need a renderer (not --renderer none)");
		return m_ViewportCapture->Capture({ .Width = width, .Height = height, .MaxDimension = 0 });
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
