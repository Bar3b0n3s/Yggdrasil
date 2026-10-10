#include "EnginePCH.h"
#include "Engine/Asset/ReplayData.h"

#include "Engine/Core/Error.h"

#include <nlohmann/json.hpp>

namespace Engine {

	ReplayData::ReplayData()
		: Asset(StaticType)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ValidateReplayDocument(const ReplayDocument& /*document*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay document validation contract"));
	}

	Result<ReplayDocument> ReplayFromJson(const Json& /*document*/, ReplayLoadReport& /*report*/, bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay JSON reader contract"));
	}

	Result<ReplayDocument> ReplayFromText(std::string_view /*text*/, ReplayLoadReport& /*report*/, bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay text reader contract"));
	}

	Result<Json> ReplayToJson(const ReplayDocument& /*document*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay JSON writer contract"));
	}

	Result<std::string> ReplayToText(const ReplayDocument& /*document*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay text writer contract"));
	}

	Result<Buffer> CookReplay(const ReplayDocument& /*source*/, std::span<const ReplayBytecodeExpectation> /*expectations*/,
		uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay cooker contract"));
	}

	Result<AssetRef<ReplayData>> LoadCookedReplay(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 cooked replay loader contract"));
	}

}
