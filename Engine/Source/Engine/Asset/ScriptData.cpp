#include "EnginePCH.h"
#include "Engine/Asset/ScriptData.h"

namespace Engine {

	struct ScriptFieldSchemaSource::State
	{
	};

	ScriptFieldSchemaSource::ScriptFieldSchemaSource(ConstructionKey /*key*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	ScriptFieldSchemaSource::~ScriptFieldSchemaSource() = default;

	Result<Buffer> CookScript(const ScriptData& /*script*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script cooking is not implemented");
	}

	Result<AssetRef<ScriptData>> LoadCookedScript(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script loading is not implemented");
	}

	Result<Ref<const ScriptFieldSchemaSource>> ScriptFieldSchemaSource::Create(std::map<AssetHandle, AssetRef<ScriptData>> /*scripts*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script field schema construction is not implemented");
	}

	Result<const FieldInfo*> ScriptFieldSchemaSource::FindField(UUID /*owner*/, std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script field schema lookup is not implemented");
	}

	std::vector<std::string> ScriptFieldSchemaSource::GetFieldNames(UUID /*owner*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<const ScriptFieldSchema*> ScriptFieldSchemaSource::FindSchema(UUID /*owner*/, std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script field descriptor lookup is not implemented");
	}

}
