#include "EnginePCH.h"
#include "Engine/Graphics/GpuOutOfMemory.h"

#include "Engine/Graphics/GraphicsDevice.h"

namespace Engine {

	bool IsGpuOutOfMemory(const Error& error, const GraphicsDevice& device)
	{
		return error.GetCode() == ErrorCode::Gpu && !device.IsLost();
	}

	Status RetryAfterTrim(GraphicsDevice& /*device*/, std::string_view /*request*/, const std::function<Status()>& /*attempt*/,
		const std::function<bool(const Error&)>& /*isOutOfMemory*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 out-of-memory contract (Docs/Decisions/0029-m18-contract.md decision 42, stream B)"));
	}

}
