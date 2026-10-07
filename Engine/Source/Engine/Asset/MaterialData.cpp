#include "EnginePCH.h"
#include "Engine/Asset/MaterialData.h"

#include <nlohmann/json.hpp>

// M6 contract stub (Roadmap rule 3): stream B (texture, material and font importers) implements the material format.

namespace Engine {

	void RegisterMaterialTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<AssetHandle> GetMaterialTextureHandles(const MaterialData& /*material*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<Json> MaterialToJson(const MaterialData& /*material*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "MaterialToJson is an M6 contract stub");
	}

	Result<std::string> MaterialToText(const MaterialData& /*material*/, const TypeRegistry& /*registry*/, JsonStyle /*style*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "MaterialToText is an M6 contract stub");
	}

	Result<MaterialData> MaterialFromJson(const Json& /*document*/, const TypeRegistry& /*registry*/, MaterialLoadReport& /*report*/,
		bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "MaterialFromJson is an M6 contract stub");
	}

	Result<MaterialData> MaterialFromText(std::string_view /*text*/, const TypeRegistry& /*registry*/, MaterialLoadReport& /*report*/,
		bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "MaterialFromText is an M6 contract stub");
	}

	Result<Buffer> CookMaterial(const MaterialData& /*material*/, const TypeRegistry& /*registry*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CookMaterial is an M6 contract stub");
	}

	Result<AssetRef<MaterialData>> LoadCookedMaterial(std::span<const std::byte> /*cooked*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "LoadCookedMaterial is an M6 contract stub");
	}

}
