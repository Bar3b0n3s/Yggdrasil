#include "EnginePCH.h"
#include "Engine/Automation/Methods/PlayMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Reflection/TypeRegistry.h"

namespace Engine {

	namespace Automation {

		Result<PlayStateResult> PlayStart(AutomationMethodContext& /*context*/, const PlayStartParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "play.start is not implemented yet (M7 stream A)");
		}

		Result<PlayStopResult> PlayStop(AutomationMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "play.stop is not implemented yet (M7 stream A)");
		}

		Result<PlayStateResult> PlayPause(AutomationMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "play.pause is not implemented yet (M7 stream A)");
		}

		Result<PlayStateResult> PlayResume(AutomationMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "play.resume is not implemented yet (M7 stream A)");
		}

		Result<Scope<PendingOperation>> PlayStep(AutomationMethodContext& /*context*/, const PlayStepParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "play.step is not implemented yet (M7 stream A)");
		}

		Result<PlayStateResult> PlayState(AutomationMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "play.state is not implemented yet (M7 stream A)");
		}

		Result<PlayStateResult> PlaySetTimeScale(AutomationMethodContext& /*context*/, const PlaySetTimeScaleParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "play.setTimeScale is not implemented yet (M7 stream A)");
		}

		PlayStateResult MakePlayStateResult(AutomationMethodContext& /*context*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

	}

	void RegisterPlayMethodTypes(TypeRegistry& /*registry*/)
	{
		// Registers nothing until stream A lands the play methods with the catalogue update (ADR 0012 decision 13).
		ENGINE_CONTRACT_STUB();
	}

	void RegisterPlayMethods(MethodRegistry& /*methods*/, bool /*includeEditorMethods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
