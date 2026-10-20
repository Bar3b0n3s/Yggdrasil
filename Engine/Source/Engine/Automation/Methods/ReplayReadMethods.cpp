#include "EnginePCH.h"
#include "Engine/Automation/Methods/ReplayMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"

#include <format>

// input.readReplay (Engine/Automation/Methods/ReplayMethods.h; Docs/Decisions/0030-m19-contract.md decision 14, A-RREAD):
// its own file, stream B's, so the recording verification's ReplayMethods.cpp (stream A) and the replay reader never share
// a file.

namespace Engine {

	namespace Automation {

		Result<InputReadReplayResult> InputReadReplay(AutomationMethodContext& /*context*/, const InputReadReplayParams& params)
		{
			ENGINE_CONTRACT_STUB();
			return std::unexpected(Error(ErrorCode::Unsupported, std::format("input.readReplay of '{}' (Docs/Decisions/0030-m19-contract.md decision 14, stream B)", params.Path)));
		}

	}

}
