#include "EnginePCH.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

namespace Engine {

	namespace Automation {

		Result<ViewportScreenshotResult> ViewportScreenshot(AutomationMethodContext& /*context*/, const ViewportScreenshotParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "the shared viewport.screenshot is not implemented yet (M7 stream B)");
		}

	}

	void RegisterViewportScreenshotMethodTypes(TypeRegistry& /*registry*/)
	{
		// The editor registers these types through EditorCore's RegisterScreenshotMethodTypes until stream B moves the
		// registration here (Docs/Decisions/0012-m7-decisions.md decision 9).
		ENGINE_CONTRACT_STUB();
	}

	void RegisterViewportScreenshotMethods(MethodRegistry& /*methods*/)
	{
		// The editor registers viewport.screenshot through EditorCore's RegisterScreenshotMethods until stream B moves it here.
		ENGINE_CONTRACT_STUB();
	}

}
