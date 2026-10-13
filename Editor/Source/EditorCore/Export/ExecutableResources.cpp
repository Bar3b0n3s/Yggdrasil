#include "EditorPCH.h"
#include "EditorCore/Export/ExecutableResources.h"

#include "Engine/Core/Error.h"

namespace Engine {

	Result<ExecutableResources> ResolveExecutableResources(const EditorContext& /*editor*/, const ExportSettings& /*settings*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M14 executable resources contract"));
	}

}
