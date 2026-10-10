#include "EnginePCH.h"
#include "Engine/Session/ReplayRecorder.h"

#include "Engine/Core/Error.h"

namespace Engine {

	struct ReplayRecorder::State
	{
	};

	ReplayRecorder::ReplayRecorder()
	{
		ENGINE_CONTRACT_STUB();
	}

	ReplayRecorder::~ReplayRecorder()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ReplayRecorder::Begin(const ReplayHeader& /*header*/, const PlaySession& /*session*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay recorder start contract"));
	}

	Status ReplayRecorder::CaptureAppliedInput(uint64_t /*tick*/, std::span<const PlayInputEvent> /*events*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 applied input recording contract"));
	}

	void ReplayRecorder::Invalidate(std::string_view /*reason*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<ReplayDocument> ReplayRecorder::Finish(const PlaySession& /*session*/, std::span<const ReplayExpectation> /*expectations*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay recorder finish contract"));
	}

	void ReplayRecorder::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool ReplayRecorder::IsRecording() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
