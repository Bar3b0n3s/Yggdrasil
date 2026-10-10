#include "EnginePCH.h"
#include "Engine/Session/PlayInputEventCodec.h"

#include "Engine/Core/Error.h"

namespace Engine {

	Result<StampedPlayInputEvent> ParsePlayInputEvent(const Json& /*value*/, const InputActionMap& /*actions*/,
		std::string_view /*pointer*/, bool /*allowTick*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 common input event parser contract"));
	}

	Result<StampedPlayInputEvent> DecodeReplayEvent(const ReplayEvent& /*event*/, const InputActionMap& /*actions*/,
		std::string_view /*pointer*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay input adaptation contract"));
	}

	Result<ReplayEvent> EncodeReplayEvent(uint64_t /*tick*/, const PlayInputEvent& /*event*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 applied input encoding contract"));
	}

}
