#include "EditorPCH.h"
#include "EditorCore/Automation/ViewportMethods.h"

namespace Engine {

	Result<ViewportCameraResult> Automation::ViewportCamera(EditorMethodContext& /*context*/, const ViewportCameraParams& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<ViewportFrameResult> Automation::ViewportFrame(EditorMethodContext& /*context*/, const ViewportFrameParams& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<ViewportSetOptionsResult> Automation::ViewportSetOptions(EditorMethodContext& /*context*/, const ViewportSetOptionsParams& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void RegisterViewportMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterViewportMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
