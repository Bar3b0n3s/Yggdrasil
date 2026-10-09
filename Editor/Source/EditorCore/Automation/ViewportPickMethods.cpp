#include "EditorPCH.h"
#include "EditorCore/Automation/ViewportPickMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"

namespace Engine {

	Result<ViewportPickResult> Automation::ViewportPick(EditorMethodContext&, const ViewportPickParams&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<ViewportPixelSize> EditorMethodContext::GetViewportPixelSize(ViewportView) const
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	void RegisterViewportPickMethodTypes(TypeRegistry&)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterViewportPickMethods(MethodRegistry&)
	{
		ENGINE_CONTRACT_STUB();
	}

}
