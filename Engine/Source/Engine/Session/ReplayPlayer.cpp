#include "EnginePCH.h"
#include "Engine/Session/ReplayPlayer.h"

#include "Engine/Core/Error.h"

namespace Engine {

	struct ReplayPlayer::State
	{
	};

	ReplayPlayer::ReplayPlayer()
	{
		ENGINE_CONTRACT_STUB();
	}

	ReplayPlayer::~ReplayPlayer()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ReplayPlayer::Begin(AssetRef<ReplayData> /*replay*/, const PlaySession& /*session*/, const ReplayPlaybackOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay player start contract"));
	}

	Result<bool> ReplayPlayer::Advance(PlaySession& /*session*/, IReplayEvaluator& /*evaluator*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay player step contract"));
	}

	Result<ReplayPlaybackResult> ReplayPlayer::GetResult() const
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 replay player result contract"));
	}

	void ReplayPlayer::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

}
