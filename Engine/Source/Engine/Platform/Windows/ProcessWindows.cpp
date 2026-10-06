#include "EnginePCH.h"
#include "Engine/Platform/Process.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements child processes
// and the current-process queries on Windows (CreateProcessW, anonymous pipes, TerminateProcess). Until then Spawn
// fails with Unsupported, so no Process exists, and the current-process queries report failures or neutral values.

#if defined(ENGINE_PLATFORM_WINDOWS)

namespace Engine {

	struct Process::Impl
	{
	};

	Process::Process(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	Process::~Process() = default;

	Process::Process(Process&& other) noexcept = default;

	Process& Process::operator=(Process&& other) noexcept = default;

	Result<Process> Process::Spawn(const ProcessSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Process::Spawn is not implemented yet");
	}

	uint32_t Process::GetId() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	bool Process::HasExited()
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

	Result<ProcessResult> Process::Wait(std::chrono::milliseconds /*timeout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Process::Wait is not implemented yet");
	}

	Status Process::WaitForOutput(ProcessStream /*stream*/, std::string_view /*text*/, std::chrono::milliseconds /*timeout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Process::WaitForOutput is not implemented yet");
	}

	Status Process::Kill()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Process::Kill is not implemented yet");
	}

	uint32_t Process::GetCurrentId()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Result<std::filesystem::path> Process::GetCurrentExecutablePath()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Process::GetCurrentExecutablePath is not implemented yet");
	}

	Result<double> Process::GetCurrentCpuSeconds()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Process::GetCurrentCpuSeconds is not implemented yet");
	}

	bool Process::IsDebuggerAttached()
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void Process::BreakIntoDebugger()
	{
		ENGINE_CONTRACT_STUB();
	}

}

#endif
