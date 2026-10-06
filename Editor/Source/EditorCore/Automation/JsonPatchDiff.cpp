#include "EditorPCH.h"
#include "EditorCore/Automation/JsonPatchDiff.h"

#include <nlohmann/json.hpp>

// M4 contract stub (Roadmap rule 3): stream C (methods) implements RFC 6902 patch generation.

namespace Engine {

	Json DiffJson(const Json& /*from*/, const Json& /*to*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json::array();
	}

}
