#include "EditorPCH.h"
#include "EditorCore/Automation/RecoveryMethods.h"

namespace Engine {

	Result<ProjectOpenResult> OpenProjectWithRecovery(EditorMethodContext& /*context*/, const ProjectOpenParams& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
