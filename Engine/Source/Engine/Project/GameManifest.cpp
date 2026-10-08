#include "EnginePCH.h"
#include "Engine/Project/GameManifest.h"

namespace Engine {

	Result<std::string> GameManifestSerializer::SaveToString(const GameManifest& /*manifest*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Game.json is not implemented yet (M7 stream C)");
	}

	Result<GameManifest> GameManifestSerializer::LoadFromString(std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Game.json is not implemented yet (M7 stream C)");
	}

	Result<GameManifest> GameManifestSerializer::LoadFromFile(const std::filesystem::path& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Game.json is not implemented yet (M7 stream C)");
	}

}
