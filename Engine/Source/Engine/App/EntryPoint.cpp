#include "EnginePCH.h"
#include "Engine/App/EntryPoint.h"

#include "Engine/App/ExitCode.h"

// M2 contract stub (Roadmap rule 3): stream D (application and frame loop) implements the process lifecycle and the
// last-resort exception boundary. Until then it starts nothing and returns ExitCode::Failed.

namespace Engine {

	int RunApplication(int /*argc*/, char** /*argv*/, ApplicationFactory /*factory*/)
	{
		ENGINE_CONTRACT_STUB();
		return ExitCode::Failed;
	}

}
