#include "EnginePCH.h"
#include "Engine/Scripting/LoadTimeVm.h"

namespace Engine {

	Result<Ref<const ScriptData>> LoadTimeVm::Extract(const ScriptCheckRequest& request,
		const LoadTimeVmSpecification& specification)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(request);
		static_cast<void>(specification);
		return MakeError(ErrorCode::Unsupported, "LoadTimeVm is an M13 contract stub");
	}

}
