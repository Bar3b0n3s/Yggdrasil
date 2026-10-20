#include "EnginePCH.h"
#include "Engine/Asset/SpriteSheetData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	void RegisterSpriteSheetTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ValidateSpriteSheet(const SpriteSheetData& /*sheet*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 SpriteSheet contract (Docs/Decisions/0029-m18-contract.md decision 15, stream S)"));
	}

	std::optional<uint32_t> FindSpriteFrame(const SpriteSheetData& /*sheet*/, std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	const SpriteClip* FindSpriteClip(const SpriteSheetData& /*sheet*/, std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<AssetHandle> GetSpriteSheetDependencies(const SpriteSheetData& /*sheet*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<Json> SpriteSheetToJson(const SpriteSheetData& /*sheet*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 SpriteSheet contract (Docs/Decisions/0029-m18-contract.md decision 14, stream S)"));
	}

	Result<std::string> SpriteSheetToText(const SpriteSheetData& /*sheet*/, const TypeRegistry& /*registry*/, JsonStyle /*style*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 SpriteSheet contract (Docs/Decisions/0029-m18-contract.md decision 14, stream S)"));
	}

	Result<SpriteSheetData> SpriteSheetFromJson(const Json& /*document*/, const TypeRegistry& /*registry*/, SpriteSheetLoadReport& /*report*/,
		bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 SpriteSheet contract (Docs/Decisions/0029-m18-contract.md decision 14, stream S)"));
	}

	Result<SpriteSheetData> SpriteSheetFromText(std::string_view /*text*/, const TypeRegistry& /*registry*/, SpriteSheetLoadReport& /*report*/,
		bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 SpriteSheet contract (Docs/Decisions/0029-m18-contract.md decision 14, stream S)"));
	}

	Buffer SerializeSpriteSheetPayload(const SpriteSheetData& /*sheet*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<SpriteSheetData> DeserializeSpriteSheetPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 SpriteSheet contract (Docs/Decisions/0029-m18-contract.md decision 16, stream S)"));
	}

	Buffer CookSpriteSheet(const SpriteSheetData& /*sheet*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<SpriteSheetData>> LoadCookedSpriteSheet(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 SpriteSheet contract (Docs/Decisions/0029-m18-contract.md decision 16, stream S)"));
	}

}
