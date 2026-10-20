#include "EnginePCH.h"
#include "Engine/Core/BuildId.h"

#include <format>

namespace Engine {

	const BuildId& GetBuildId()
	{
		ENGINE_CONTRACT_STUB();
		static const BuildId Unstamped{ .Version = std::string(EngineVersionString), .Commit = {}, .Dirty = false }; // immutable after its thread-safe initialization
		return Unstamped;
	}

	std::string FormatBuildId(const BuildId& id)
	{
		ENGINE_CONTRACT_STUB();
		return id.Version;
	}

	Result<BuildId> ParseBuildId(std::string_view text)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, std::format("parsing the build id '{}' (Docs/Decisions/0030-m19-contract.md decision 13, stream D)", text)));
	}

}
