#include "EnginePCH.h"
#include "Engine/Native/NativeComponents.h"

#include "Engine/Core/Error.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <format>

namespace Engine {

	Result<NativeSceneReport> CanonicalizeNativeComponents(Scene& scene, const NativeModuleTypes* /*types*/, bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("canonicalizing the native components of the scene '{}' (Docs/Decisions/0030-m19-contract.md decision 27, stream N-B)", scene.GetName())));
	}

	NativeEntityComponents ReadNativeComponents(ConstEntity /*entity*/, const NativeModuleTypes* /*types*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<bool> PatchNativeComponents(Entity /*entity*/, const NativeModuleTypes& /*types*/, const Json& /*patch*/, std::string_view pointer)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("patching native components at '{}' (Docs/Decisions/0030-m19-contract.md decision 27, stream N-B)", pointer)));
	}

}
