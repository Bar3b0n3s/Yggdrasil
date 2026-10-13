#include "EditorPCH.h"
#include "EditorCore/Reference/DescriptionInventory.h"

#include "Engine/Core/Error.h"

namespace Engine {

	Result<std::vector<DescriptionEntry>> CollectDescriptions(const TypeRegistry& /*types*/, const ScriptApiRegistry& /*scripts*/,
		const MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M14 description inventory contract"));
	}

	std::vector<std::string> FindMissingDescriptions(std::span<const DescriptionEntry> /*entries*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
