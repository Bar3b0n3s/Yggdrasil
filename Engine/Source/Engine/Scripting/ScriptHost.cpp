#include "EnginePCH.h"
#include "Engine/Scripting/ScriptHost.h"

#include <nlohmann/json.hpp>

namespace Engine {

	// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): out of line, so the header needs only Json's declaration.
	Result<Json> IScriptHost::GetSaveValue(std::string_view /*key*/) const
	{
		return MakeError(ErrorCode::Unsupported, "{}", NoSaveDataReason);
	}

}
