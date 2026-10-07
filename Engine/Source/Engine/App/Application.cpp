#include "EnginePCH.h"
#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"

#include <array>
#include <utility>

namespace Engine {

	namespace Utils {

		constexpr std::string_view HeadlessOption = "--headless";
		constexpr std::string_view FramesOption = "--frames";
#if !defined(ENGINE_DIST)
		constexpr std::string_view UserDataDirectoryOption = "--user-data-dir";
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
			.RegisterTypes = m_Specification.RegisterTypes,
		};
		Result<Scope<EngineContext>> context = EngineContext::Create(contextSpecification);
		if (!context.has_value())
		{
			ENGINE_CORE_ERROR("Cannot initialize the application '{}': {}", m_Specification.Name, context.error());
			return ExitCode::InitFailed;
		}
		m_Context = std::move(*context);

		const Status initialized = OnInitialize();
		if (!initialized.has_value())
		{
			ENGINE_CORE_ERROR("Cannot initialize the application '{}': {}", m_Specification.Name, initialized.error());
			m_PendingExitCode.reset();
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
		const int exitCode = m_FrameLoop->Run();
		m_FrameLoop.reset();

		// 3. Shutdown in reverse order.
		OnShutdown();
		m_PendingExitCode.reset();
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

	void Application::OnFrameSafePoint()
	{
		OnSafePoint();
	}

	void Application::OnFrameFixedStep(const SimStep& step)
	{
		OnFixedStep(step);
	}

	void Application::OnFrameUpdate(const FrameTime& frame)
	{
		OnUpdate(frame);
	}

	Scope<Clock> Application::CreateClock() const
	{
		if (m_Specification.Clock == ClockKind::Manual)
			return CreateScope<ManualClock>(m_Specification.Loop.GetFixedDelta());
		return CreateScope<SystemClock>();
	}

}
