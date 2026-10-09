#include "EditorPCH.h"
#include "EditorCore/Automation/ScreenshotMethods.h"

#include "Engine/Automation/Protocol/PendingOperation.h"

namespace Engine {

	Result<Scope<PendingOperation>> Automation::BeginEditorScreenshot(EditorMethodContext& /*context*/, const EditorScreenshotParams& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
