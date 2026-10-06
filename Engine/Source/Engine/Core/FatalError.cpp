#include "EnginePCH.h"
#include "Engine/Core/FatalError.h"

#include <cstdlib>

// M1 contract stub (Roadmap rule 3): stream A implements logging, the handler and the exit-code mapping. The stub still
// never returns, as the declaration promises.

namespace Engine {

	FatalErrorHandler SetFatalErrorHandler(FatalErrorHandler /*handler*/)
	{
		return nullptr;
	}

	void FatalError(FatalErrorKind /*kind*/, std::string_view /*message*/)
	{
		std::_Exit(FatalCrashExitCode);
	}

	int GetFatalErrorExitCode(FatalErrorKind /*kind*/)
	{
		return FatalCrashExitCode;
	}

	std::string_view FatalErrorKindToString(FatalErrorKind /*kind*/)
	{
		return {};
	}

}
