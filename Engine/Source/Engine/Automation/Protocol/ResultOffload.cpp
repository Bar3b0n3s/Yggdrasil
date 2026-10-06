#include "EnginePCH.h"
#include "Engine/Automation/Protocol/ResultOffload.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the offload helpers.

namespace Engine {

	std::string MakeOffloadServerTag(uint32_t /*processId*/, int64_t /*startSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string MakeOffloadFileName(std::string_view /*serverTag*/, uint64_t /*sequence*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Json MakeOffloadSummary(const Json& /*result*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	Json MakeOffloadedResult(std::string_view /*path*/, const Json& /*summary*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

}
