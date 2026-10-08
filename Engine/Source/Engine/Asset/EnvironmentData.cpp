#include "EnginePCH.h"
#include "Engine/Asset/EnvironmentData.h"

#include "Engine/Core/Error.h"

namespace Engine {

	Status ValidateEnvironmentData(const EnvironmentData& /*environment*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the environment payload is not implemented yet (M8 stream B)");
	}

	Buffer SerializeEnvironmentPayload(const EnvironmentData& /*environment*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<EnvironmentData> DeserializeEnvironmentPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the environment payload is not implemented yet (M8 stream B)");
	}

	Buffer CookEnvironment(const EnvironmentData& /*environment*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<EnvironmentData>> LoadCookedEnvironment(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the environment loader is not implemented yet (M8 stream B)");
	}

}
