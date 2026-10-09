#include "EditorPCH.h"
#include "Editor/Drawers/ReflectedDrawers.h"

namespace Engine {

	Result<ReflectedDrawerResult> DrawReflectedValue(const FieldInfo& /*field*/, Value& /*value*/, const ReflectedDrawerContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
