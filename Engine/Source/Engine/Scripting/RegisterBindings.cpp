#include "EnginePCH.h"
#include "Engine/Scripting/RegisterBindings.h"

namespace Engine {

	Status RegisterBindings(ScriptApiRegistry& /*api*/, const TypeRegistry& /*types*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Built-in script bindings are not implemented");
	}

}
