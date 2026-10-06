#include "EnginePCH.h"
#include "Engine/Platform/CrashHandler.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements the handler on
// Windows (SetUnhandledExceptionFilter, MiniDumpWriteDump, the CRT abort and invalid-parameter handlers). Until then
// Install and WriteFatalErrorReport fail with Unsupported, and SimulateCrash ends the process through
// FatalError(InitFailed), exit code 3, so a test that expects the crash path's exit code 4 cannot pass by accident.

#if defined(ENGINE_PLATFORM_WINDOWS)

namespace Engine {

	Status CrashHandler::Install(const CrashHandlerSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CrashHandler::Install is not implemented yet");
	}

	void CrashHandler::Uninstall()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<std::filesystem::path> CrashHandler::WriteFatalErrorReport(FatalErrorKind /*kind*/, std::string_view /*message*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CrashHandler::WriteFatalErrorReport is not implemented yet");
	}

	void CrashHandler::SimulateCrash()
	{
		ENGINE_CONTRACT_STUB();
		FatalError(FatalErrorKind::InitFailed, "CrashHandler::SimulateCrash is not implemented yet");
	}

}

#endif
