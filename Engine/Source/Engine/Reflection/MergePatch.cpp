#include "EnginePCH.h"
#include "Engine/Reflection/MergePatch.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements RFC 7386.

namespace Engine {

	Json ApplyMergePatch(const Json& target, const Json& /*patch*/)
	{
		ENGINE_CONTRACT_STUB();
		return target;
	}

	Json CreateMergePatch(const Json& /*source*/, const Json& /*target*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json::object();
	}

}
