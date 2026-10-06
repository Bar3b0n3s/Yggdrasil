#include "EnginePCH.h"
#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/Platform/ErrorDialog.h"

#include <array>
#include <utility>

namespace Engine {

	namespace Utils {

		constexpr std::string_view HeadlessOption = "--headless";
		constexpr std::string_view FramesOption = "--frames";
#if !defined(ENGINE_DIST)
		constexpr std::string_view UserDataDirectoryOption = "--user-data-dir";
		constexpr std::string_view RendererOption = "--renderer";
		constexpr std::string_view GpuValidationOption = "--gpu-validation";
		constexpr std::string_view GpuValidationSync = "sync";
		constexpr std::string_view ExpectNoGpuErrorsOption = "--expect-no-gpu-errors";
		constexpr std::string_view VulkanApiOption = "--vulkan-api";
		constexpr std::string_view GpuOption = "--gpu";
		constexpr std::string_view GpuInjectFaultOption = "--gpu-inject-fault";
#endif

		// Every option of GetEngineCommandLineOptions. Dist honours only the options §13.9 lists, so --user-data-dir is
		// an unknown option there.
		constexpr std::array EngineCommandLineOptions = {
			CommandLineOption{
				.Name = HeadlessOption,
				.Description = "Run without a display on GLFW's null platform, one fixed step per frame (ManualClock).",
			},
			CommandLineOption{
				.Name = FramesOption,
				.Value = CommandLineValue::Required,
				.ValueName = "N",
				.Description = "Exit with code 0 after N frames (N >= 1).",
			},
#if !defined(ENGINE_DIST)
			CommandLineOption{
				.Name = UserDataDirectoryOption,
				.Value = CommandLineValue::Required,
				.ValueName = "path",
				.Description = "Keep logs and crash reports under this absolute directory instead of the user's data folder.",
			},
			CommandLineOption{
				.Name = RendererOption,
				.Value = CommandLineValue::Required,
				.ValueName = "vulkan|none",
				.Description = "Render with Vulkan (default), or not at all (logic only, no GPU needed).",
			},
			CommandLineOption{
				.Name = GpuValidationOption,
				.Value = CommandLineValue::Optional,
				.ValueName = "sync",
				.Description = "Enable the Vulkan validation layer and NVRHI validation (on by default in Debug); =sync adds "
							   "synchronization validation.",
			},
			CommandLineOption{
				.Name = ExpectNoGpuErrorsOption,
				.Description = "Exit with code 1 when the GPU device reported any validation or other error during the run.",
			},
			CommandLineOption{
				.Name = VulkanApiOption,
				.Value = CommandLineValue::Required,
				.ValueName = "1.3|1.4",
				.Description = "Request at most this Vulkan API version (1.3 forces the fallbacks of the 1.4 paths).",
			},
			CommandLineOption{
				.Name = GpuOption,
				.Value = CommandLineValue::Required,
				.ValueName = "index|name",
				.Description = "Use the GPU with this index or a name containing this text (overrides ENGINE_GPU).",
			},
			CommandLineOption{
				.Name = GpuInjectFaultOption,
				.Value = CommandLineValue::Required,
				.ValueName = "device-lost|oom-texture|hang",
				.Description = "Force a GPU fault path for tests: device loss, texture out-of-memory or a GPU hang.",
			},
#endif
		};

	}

	std::span<const CommandLineOption> GetEngineCommandLineOptions()
	{
		return Utils::EngineCommandLineOptions;
	}

	Status ApplyEngineCommandLine(const CommandLine& commandLine, ApplicationSpecification& specification)
	{
		if (commandLine.Has(Utils::HeadlessOption))
		{
			specification.Window = WindowMode::Headless;
			specification.Clock = ClockKind::Manual; // every headless run uses ManualClock (§13.9)
		}

		ENGINE_TRY_ASSIGN(const std::optional<uint64_t> frames, commandLine.GetUnsigned(Utils::FramesOption));
		if (frames.has_value())
		{
			if (*frames == 0)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a number of frames >= 1, got 0", Utils::FramesOption);
			specification.MaxFrames = *frames;
		}

#if !defined(ENGINE_DIST)
		if (const std::optional<std::string_view> directory = commandLine.GetValue(Utils::UserDataDirectoryOption))
		{
			// std::filesystem::path throws on ill-formed UTF-8 on Windows, and the option is external input.
			if (!IsValidUtf8(*directory))
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a path in UTF-8", Utils::UserDataDirectoryOption);
			std::filesystem::path root(std::u8string_view(reinterpret_cast<const char8_t*>(directory->data()), directory->size()));
			if (!root.is_absolute())
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs an absolute directory, got '{}'",
					Utils::UserDataDirectoryOption, *directory);
			}
			specification.UserDataRoot = std::move(root);
		}

		if (const std::optional<std::string_view> renderer = commandLine.GetValue(Utils::RendererOption))
		{
			const std::optional<RendererMode> mode = RendererModeFromCommandLine(*renderer);
			if (!mode.has_value())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes 'vulkan' or 'none', got '{}'", Utils::RendererOption, *renderer);
			specification.Renderer = *mode;
		}

		if (commandLine.Has(Utils::GpuValidationOption))
		{
			const std::optional<std::string_view> validation = commandLine.GetValue(Utils::GpuValidationOption);
			if (validation.has_value() && *validation != Utils::GpuValidationSync)
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes no value or '{}', got '{}'", Utils::GpuValidationOption,
					Utils::GpuValidationSync, *validation);
			}
			specification.Graphics.Validation = true;
			specification.Graphics.SynchronizationValidation = validation.has_value();
		}

		if (commandLine.Has(Utils::ExpectNoGpuErrorsOption))
			specification.ExpectNoGpuErrors = true;

		if (const std::optional<std::string_view> api = commandLine.GetValue(Utils::VulkanApiOption))
		{
			const std::optional<VulkanApiVersion> version = VulkanApiVersionFromString(*api);
			if (!version.has_value())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes '1.3' or '1.4', got '{}'", Utils::VulkanApiOption, *api);
			specification.Graphics.MaxApiVersion = *version;
		}

		// CommandLine::Parse never yields an empty value for a Required option.
		if (const std::optional<std::string_view> gpu = commandLine.GetValue(Utils::GpuOption))
			specification.Graphics.GpuOverride = std::string(*gpu);

		if (const std::optional<std::string_view> fault = commandLine.GetValue(Utils::GpuInjectFaultOption))
		{
			const std::optional<GpuFault> injected = GpuFaultFromString(*fault);
			if (!injected.has_value() || *injected == GpuFault::None)
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes 'device-lost', 'oom-texture' or 'hang', got '{}'",
					Utils::GpuInjectFaultOption, *fault);
			}
			specification.Graphics.InjectFault = *injected;
		}
#endif

		specification.Args = commandLine;
		return {};
	}

	Application::Application(ApplicationSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	Application::~Application() = default;

	int Application::Run()
	{
		ENGINE_CORE_VERIFY(!m_HasRun, "Application::Run may be called once per Application");
		m_HasRun = true;

		ProcessContext* process = ProcessContext::GetCurrent();
		ENGINE_CORE_VERIFY(process != nullptr, "Application::Run needs a live ProcessContext");
		ENGINE_CORE_ASSERT(process->GetSpecification().Window == m_Specification.Window,
			"the application is {} but the process is {}: a process is windowed or headless, never both",
			WindowModeToString(m_Specification.Window), WindowModeToString(process->GetSpecification().Window));

		if (m_Specification.Clock == ClockKind::Scripted)
		{
			ENGINE_CORE_ERROR("Cannot run the application '{}': the Scripted clock needs a delta table and is chosen per "
							  "FeatureTest suite, never through ApplicationSpecification::Clock",
				m_Specification.Name);
			return ExitCode::InitFailed;
		}

		// 1. Per-context initialization (§4.1 level 2), then the application's own.
		WindowSpecification window = m_Specification.WindowSettings;
		if (window.Title.empty())
			window.Title = m_Specification.Name;
		const EngineContextSpecification contextSpecification = {
			.WorkerCount = m_Specification.WorkerCount.value_or(JobSystem::GetDefaultWorkerCount()),
			.UserDataDirectory = process->GetUserDataPaths().Root,
			.Window = std::move(window),
			.Graphics = m_Specification.Renderer == RendererMode::Vulkan ? std::optional<GraphicsSpecification>(m_Specification.Graphics)
																		 : std::nullopt,
		};
		Result<Scope<EngineContext>> context = EngineContext::Create(contextSpecification);
		if (!context.has_value())
		{
			ReportInitializationFailure(*process, context.error());
			return ExitCode::InitFailed;
		}
		m_Context = std::move(*context);

		const Status rendering = InitializeRendering();
		if (!rendering.has_value())
		{
			ReportInitializationFailure(*process, rendering.error());
			ShutdownRendering();
			m_Context.reset();
			return ExitCode::InitFailed;
		}

		const Status initialized = OnInitialize();
		if (!initialized.has_value())
		{
			ReportInitializationFailure(*process, initialized.error());
			m_PendingExitCode.reset();
			ShutdownRendering();
			m_Context.reset();
			return ExitCode::InitFailed;
		}

		// 2. The frame loop. An exit requested during OnInitialize makes it run no frame.
		const FrameLoopSpecification loopSpecification = {
			.Loop = m_Specification.Loop,
			.MaxFrames = m_Specification.MaxFrames,
			.ThrottleToFixedHz = m_Specification.Window == WindowMode::Headless && m_Specification.ThrottleHeadless,
		};
		IFrameLoopClient& client = *this; // the base is private, so convert here rather than inside CreateScope
		m_FrameLoop = CreateScope<FrameLoop>(*m_Context, client, CreateClock(), loopSpecification);
		if (m_PendingExitCode.has_value())
			m_FrameLoop->RequestExit(*m_PendingExitCode);
		int exitCode = m_FrameLoop->Run();
		m_FrameLoop.reset();

		// 3. Shutdown in reverse order. A run that expects no GPU errors fails when its device reported any (§15.3); an
		// earlier failure keeps its code.
		OnShutdown();
		m_PendingExitCode.reset();
		ShutdownRendering();
		const GraphicsDevice* device = m_Context->GetGraphicsDevice();
		if (m_Specification.ExpectNoGpuErrors && device != nullptr && exitCode == ExitCode::Success)
		{
			const uint64_t errors = device->GetDiagnostics().GetErrorCount();
			if (errors > 0)
			{
				ENGINE_CORE_ERROR("The GPU device reported {} error(s) during the run (--expect-no-gpu-errors)", errors);
				exitCode = ExitCode::Failed;
			}
		}
		m_Context.reset();
		return exitCode;
	}

	void Application::RequestExit(int exitCode)
	{
		if (m_FrameLoop != nullptr)
			m_FrameLoop->RequestExit(exitCode);
		else if (!m_PendingExitCode.has_value())
			m_PendingExitCode = exitCode;
	}

	EngineContext& Application::GetContext()
	{
		ENGINE_CORE_ASSERT(m_Context != nullptr, "Application::GetContext outside initialization and shutdown");
		return *m_Context;
	}

	ProcessContext& Application::GetProcessContext()
	{
		ENGINE_CORE_ASSERT(m_Context != nullptr, "Application::GetProcessContext outside initialization and shutdown");
		ProcessContext* process = ProcessContext::GetCurrent();
		ENGINE_CORE_VERIFY(process != nullptr, "Application::GetProcessContext without a ProcessContext");
		return *process;
	}

	void Application::OnFrameEvent(Event& event)
	{
		OnEvent(event);
	}

	void Application::OnFrameFixedStep(const SimStep& step)
	{
		OnFixedStep(step);
	}

	void Application::OnFrameUpdate(const FrameTime& frame)
	{
		OnUpdate(frame);
	}

	void Application::OnFrameRender(const FrameTime& /*frame*/)
	{
		// M5 contract stub (Roadmap rule 3): stream B (swapchain, present, frame pacing, fault handling) renders the frame
		// as the class comment describes (step 2). Without a device there is nothing to render.
		ENGINE_CONTRACT_STUB();
	}

	Status Application::InitializeRendering()
	{
		// M5 contract stub (Roadmap rule 3): stream B builds the swapchain or offscreen target and the pacer, and the
		// ImGuiLayer with EnableImGui (step 1 of the class comment). RendererMode::None needs none of them.
		ENGINE_CONTRACT_STUB();
		if (m_Context->GetGraphicsDevice() == nullptr)
			return {};
		return MakeError(ErrorCode::Unsupported, "rendering the application's frames is not implemented yet");
	}

	void Application::ShutdownRendering()
	{
		// M5 contract stub (Roadmap rule 3): stream B waits for the device and destroys the rendering objects in reverse.
		ENGINE_CONTRACT_STUB();
		m_ImGuiLayer.reset();
	}

	void Application::ReportInitializationFailure(const ProcessContext& process, const Error& error) const
	{
		ENGINE_CORE_ERROR("Cannot initialize the application '{}': {}", m_Specification.Name, error);
		if (process.GetSpecification().ShowErrorDialogs && m_Specification.Window == WindowMode::Windowed)
			ShowErrorDialog(m_Specification.Name, std::format("Cannot start: {}", error.ToString()));
	}

	Scope<Clock> Application::CreateClock() const
	{
		if (m_Specification.Clock == ClockKind::Manual)
			return CreateScope<ManualClock>(m_Specification.Loop.GetFixedDelta());
		return CreateScope<SystemClock>();
	}

}
