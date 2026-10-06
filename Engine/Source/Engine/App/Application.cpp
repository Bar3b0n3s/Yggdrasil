#include "EnginePCH.h"
#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Assert.h"

// M2 contract stub (Roadmap rule 3): stream D (application and frame loop) implements Run, exit requests, the frame
// hooks and the engine command-line options. Until then Run returns ExitCode::Failed without initializing anything.

namespace Engine {

	std::span<const CommandLineOption> GetEngineCommandLineOptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status ApplyEngineCommandLine(const CommandLine& /*commandLine*/, ApplicationSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ApplyEngineCommandLine is not implemented yet");
	}

	Application::Application(ApplicationSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	Application::~Application() = default;

	int Application::Run()
	{
		ENGINE_CONTRACT_STUB();
		return ExitCode::Failed;
	}

	void Application::RequestExit(int /*exitCode*/)
	{
		ENGINE_CONTRACT_STUB();
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

	void Application::OnFrameEvent(Event& /*event*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Application::OnFrameFixedStep(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Application::OnFrameUpdate(const FrameTime& /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
