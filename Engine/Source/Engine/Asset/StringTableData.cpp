#include "EnginePCH.h"
#include "Engine/Asset/StringTableData.h"

#include <nlohmann/json.hpp>

// String tables (StringTableData.h): stubs of the M22 contract (C22.1, Docs/Decisions/0033-m22-contract.md decision 10) that
// stream L implements. GetStringTableDependencies is final: a table references no asset.

namespace Engine {

	void RegisterStringTableTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ValidateStringTable(const StringTableData& /*table*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "string tables (Docs/Decisions/0033-m22-contract.md decision 10, stream L)");
	}

	uint32_t ResolveStringTableLocale(const StringTableData& /*table*/, std::string_view /*locale*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	const StringTableEntry* FindStringTableEntry(const StringTableData& /*table*/, std::string_view /*key*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const std::string* FindLocalizedString(const StringTableData& /*table*/, std::string_view /*key*/, std::string_view /*locale*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<AssetHandle> GetStringTableDependencies(const StringTableData& /*table*/)
	{
		return {};
	}

	Result<Json> StringTableToJson(const StringTableData& /*table*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "string tables (Docs/Decisions/0033-m22-contract.md decision 10, stream L)");
	}

	Result<std::string> StringTableToText(const StringTableData& /*table*/, const TypeRegistry& /*registry*/, JsonStyle /*style*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "string tables (Docs/Decisions/0033-m22-contract.md decision 10, stream L)");
	}

	Result<StringTableData> StringTableFromJson(const Json& /*document*/, const TypeRegistry& /*registry*/, StringTableLoadReport& /*report*/,
		bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "string tables (Docs/Decisions/0033-m22-contract.md decision 10, stream L)");
	}

	Result<StringTableData> StringTableFromText(std::string_view /*text*/, const TypeRegistry& /*registry*/, StringTableLoadReport& /*report*/,
		bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "string tables (Docs/Decisions/0033-m22-contract.md decision 10, stream L)");
	}

	Buffer SerializeStringTablePayload(const StringTableData& /*table*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<StringTableData> DeserializeStringTablePayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "string tables (Docs/Decisions/0033-m22-contract.md decision 10, stream L)");
	}

	Buffer CookStringTable(const StringTableData& /*table*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<StringTableData>> LoadCookedStringTable(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "string tables (Docs/Decisions/0033-m22-contract.md decision 10, stream L)");
	}

}
