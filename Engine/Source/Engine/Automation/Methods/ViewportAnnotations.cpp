#include "EnginePCH.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"

namespace Engine {

	Result<ViewportAnnotationOptions> ParseViewportAnnotations(const VariantValue&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<RenderAnnotations> ResolveViewportAnnotations(AutomationMethodContext&, Scene&, const ViewportAnnotationOptions&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 annotation resolution is not implemented"));
	}

}
