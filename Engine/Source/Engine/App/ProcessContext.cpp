#include "EnginePCH.h"
#include "Engine/App/ProcessContext.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/Process.h"

#include <array>
#include <atomic>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// The live context, read by the fatal-error handler on whichever thread fails (process-level state, §3 rule 5).
		static std::atomic<ProcessContext*> s_CurrentProcessContext{ nullptr };

		constexpr std::array<ProcessContextStep, 4> ProcessContextSteps = {
			ProcessContextStep::Log,
			ProcessContextStep::Profiler,
			ProcessContextStep::CrashHandler,
			ProcessContextStep::Glfw,
		};

		constexpr std::string_view BuildDescription =
#if defined(ENGINE_DEBUG)
			"Debug"
#elif defined(ENGINE_RELEASE)
			"Release"
#elif defined(ENGINE_DIST)
			"Dist"
#endif
#if defined(ENGINE_PLATFORM_WINDOWS)
			" windows-x86_64";
#elif defined(ENGINE_PLATFORM_LINUX)
			" linux-x86_64";
#elif defined(ENGINE_PLATFORM_MACOS)
			" macos-arm64";
#endif

		// `path` in its native format as UTF-8, for log lines and the log file's name.
		static std::string PathToUtf8(const std::filesystem::path& path)
		{
			const std::u8string text = path.u8string();
			return std::string(reinterpret_cast<const char*>(text.data()), text.size());
		}

	}

	ProcessContext::ProcessContext(ConstructionKey /*key*/, ProcessContextSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	ProcessContext::~ProcessContext()
	{
		while (!m_Steps.empty())
		{
			const ProcessContextStep step = m_Steps.back();
			m_Steps.pop_back();
			ShutDownStep(step);
		}
		// Create allows one context at a time, so the live one is this one.
		Utils::s_CurrentProcessContext.store(nullptr);
	}

	Result<Scope<ProcessContext>> ProcessContext::Create(const ProcessContextSpecification& specification)
	{
		ENGINE_CORE_VERIFY(Utils::s_CurrentProcessContext.load() == nullptr, "a ProcessContext already exists");

		Scope<ProcessContext> context = CreateScope<ProcessContext>(ConstructionKey(), specification);
		Utils::s_CurrentProcessContext.store(context.get());
		for (const ProcessContextStep step : Utils::ProcessContextSteps)
		{
			Status initialized = context->InitializeStep(step);
			if (!initialized.has_value())
			{
				// Destroying the context tears the completed steps down in reverse order.
				std::string stepContext = std::format("while initializing the process context ({})", ProcessContextStepToString(step));
				return std::unexpected(std::move(initialized).error().WithContext(std::move(stepContext)));
			}
		}
		ENGINE_CORE_INFO("Process context ready: {} on GLFW's {} platform", WindowModeToString(specification.Window),
			GlfwPlatformToString(GlfwLibrary::GetPlatform()));
		return context;
	}

	ProcessContext* ProcessContext::GetCurrent()
	{
		return Utils::s_CurrentProcessContext.load();
	}

	void ProcessContext::SetFatalErrorHook(FatalErrorHook hook)
	{
		std::scoped_lock lock(m_FatalErrorHookMutex);
		m_FatalErrorHook = hook.Function != nullptr ? hook : FatalErrorHook();
	}

	Status ProcessContext::InitializeStep(ProcessContextStep step)
	{
		switch (step)
		{
			case ProcessContextStep::Log:
			{
				// Before the log exists: the user-data folders, and the executable's name for the log file (§4.4).
				ENGINE_TRY_ASSIGN(m_UserDataPaths, Paths::GetUserDataPaths(m_Specification.AppName, m_Specification.UserDataRoot));
				ENGINE_TRY(Paths::CreateUserDataDirectories(m_UserDataPaths));
				std::filesystem::path logFile;
				if (m_Specification.LogToFile)
				{
					ENGINE_TRY_ASSIGN(const std::filesystem::path executable, Process::GetCurrentExecutablePath());
					logFile = m_UserDataPaths.GetLogFile(Utils::PathToUtf8(executable.stem()));
				}
				ENGINE_TRY(Log::Initialize({
					.FilePath = logFile,
					.Console = m_Specification.LogToConsole,
					.ConsoleLevel = m_Specification.ConsoleLevel,
				}));
				m_LogFilePath = std::move(logFile);
				ENGINE_CORE_INFO("Starting {} ({}): user data in '{}', log file '{}'", m_Specification.AppName, GetBuildDescription(),
					Utils::PathToUtf8(m_UserDataPaths.Root), m_LogFilePath.empty() ? "none" : Utils::PathToUtf8(m_LogFilePath));
				break;
			}
			case ProcessContextStep::Profiler:
			{
				Profiler::Initialize();
				Profiler::SetThreadName("Main");
				break;
			}
			case ProcessContextStep::CrashHandler:
			{
				// Without crash reports the handler still turns a crash into exit code 4, but writes no file.
				ENGINE_TRY(CrashHandler::Install({
					.ReportDirectory = m_Specification.WriteCrashReports ? m_UserDataPaths.Crashes : std::filesystem::path(),
					.AppName = m_Specification.AppName,
					.BuildInfo = std::string(GetBuildDescription()),
				}));
				m_PreviousFatalErrorHandler = SetFatalErrorHandler(&ProcessContext::HandleFatalError);
				break;
			}
			case ProcessContextStep::Glfw:
			{
				// The Vulkan loader step of the Graphics milestone runs before this one and hands GLFW its
				// vkGetInstanceProcAddr through VulkanLoader (§4.1); until then GLFW is told about no loader.
				ENGINE_TRY(GlfwLibrary::Initialize({ .Mode = m_Specification.Window, .VulkanLoader = nullptr }));
				break;
			}
		}
		m_Steps.push_back(step);
		ENGINE_CORE_INFO("Process context: {} initialized", ProcessContextStepToString(step));
		return {};
	}

	void ProcessContext::ShutDownStep(ProcessContextStep step)
	{
		// For the Log step this is the last line the log prints.
		ENGINE_CORE_INFO("Process context: shutting down {}", ProcessContextStepToString(step));
		switch (step)
		{
			case ProcessContextStep::Log:
				Log::Shutdown();
				break;
			case ProcessContextStep::Profiler:
				Profiler::Shutdown();
				break;
			case ProcessContextStep::CrashHandler:
			{
				SetFatalErrorHandler(m_PreviousFatalErrorHandler);
				m_PreviousFatalErrorHandler = nullptr;
				std::scoped_lock lock(m_FatalErrorHookMutex);
				m_FatalErrorHook = FatalErrorHook();
				CrashHandler::Uninstall();
				break;
			}
			case ProcessContextStep::Glfw:
				GlfwLibrary::Shutdown();
				break;
		}
	}

	void ProcessContext::HandleFatalError(FatalErrorKind kind, std::string_view message)
	{
		// 1. A failed assertion stops in an attached debugger (ADR 0003 decision 21); FatalError's log line is already out.
		if (kind == FatalErrorKind::Assert && Process::IsDebuggerAttached())
			Process::BreakIntoDebugger();

		ProcessContext* context = Utils::s_CurrentProcessContext.load();
		if (context == nullptr)
			return;

		// 2. The application's hook (the editor's autosave), under the lock that SetFatalErrorHook takes.
		{
			std::scoped_lock lock(context->m_FatalErrorHookMutex);
			if (context->m_FatalErrorHook.Function != nullptr)
				context->m_FatalErrorHook.Function(context->m_FatalErrorHook.UserData, kind, message);
		}

		// 3. The crash report; its last log lines include whatever the hook logged.
		if (context->m_Specification.WriteCrashReports)
		{
			const Result<std::filesystem::path> report = CrashHandler::WriteFatalErrorReport(kind, message);
			if (report.has_value())
				ENGINE_CORE_WARN("Crash report written to '{}'", Utils::PathToUtf8(*report));
			else
				ENGINE_CORE_ERROR("Cannot write the crash report: {}", report.error());
		}

		// 4. The message box of a windowed process joins here with the Graphics milestone, which brings the Platform call
		//    that shows it (§4.6, §8.1, §14.3; ADR 0005 decision 9).
	}

	std::string_view ProcessContextStepToString(ProcessContextStep step)
	{
		switch (step)
		{
			case ProcessContextStep::Log:          return "Log";
			case ProcessContextStep::Profiler:     return "Profiler";
			case ProcessContextStep::CrashHandler: return "CrashHandler";
			case ProcessContextStep::Glfw:         return "Glfw";
		}

		ENGINE_CORE_ASSERT(false, "Unknown ProcessContextStep {}", std::to_underlying(step));
		return "Unknown";
	}

	std::string_view GetBuildDescription()
	{
		return Utils::BuildDescription;
	}

}
