#include "EnginePCH.h"
#include "Engine/Automation/Methods/AudioMethods.h"

namespace Engine {

	namespace Automation {

		Result<AudioStatsResult> AudioStats(AutomationMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "audio.stats is not implemented yet (M12 stream C)");
		}

	}

	void RegisterAudioMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterAudioMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
