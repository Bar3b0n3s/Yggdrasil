#include "EnginePCH.h"
#include "Engine/Scripting/ScriptAssetLoads.h"

#include <format>

namespace Engine {

	Result<std::vector<ScriptAssetLoad>> FindScriptAssetLoads(std::string_view source, std::string_view chunkName)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("scanning the {} bytes of '{}' for Assets.Load (Docs/Decisions/0030-m19-contract.md decision 19, stream D)", source.size(), chunkName)));
	}

}
