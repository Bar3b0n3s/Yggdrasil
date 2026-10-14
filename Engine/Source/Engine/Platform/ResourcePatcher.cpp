#include "EnginePCH.h"
#include "Engine/Platform/ResourcePatcher.h"

#include "Engine/Core/Error.h"

namespace Engine {

	Result<Buffer> ResourcePatcher::BuildVersionInfo(const ExecutableVersionInfo& /*info*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 resource patcher contract (stream A)"));
	}

	Result<Buffer> ResourcePatcher::BuildIconGroup(std::span<const ExecutableIconImage> /*icons*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 resource patcher contract (stream A)"));
	}

}
