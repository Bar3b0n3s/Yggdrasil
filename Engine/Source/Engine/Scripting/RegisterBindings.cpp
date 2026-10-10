#include "EnginePCH.h"
#include "Engine/Scripting/RegisterBindings.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/Private/BindingRegistration.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

namespace Engine {

	namespace Detail {

		Status RegisterBuiltinBindings(ScriptApiRegistry& api, const TypeRegistry& types)
		{
			if (!types.IsFrozen() || api.IsFrozen() || !api.GetModules().empty() || !api.GetTypes().empty() || !api.GetEnums().empty() || !api.GetAliases().empty())
				return MakeError(ErrorCode::InvalidState, "built-in registration requires frozen reflected types and an empty mutable API registry");
			ENGINE_TRY(ScriptBindings::RegisterScript(api));
			ENGINE_TRY(ScriptBindings::RegisterEntity(api));
			ENGINE_TRY(ScriptBindings::RegisterComponents(api));
			ENGINE_TRY(ScriptBindings::RegisterScene(api));
			ENGINE_TRY(ScriptBindings::RegisterInput(api));
			ENGINE_TRY(ScriptBindings::RegisterTime(api));
			ENGINE_TRY(ScriptBindings::RegisterPhysics(api));
			ENGINE_TRY(ScriptBindings::RegisterAudio(api));
			ENGINE_TRY(ScriptBindings::RegisterAssets(api));
			ENGINE_TRY(ScriptBindings::RegisterTask(api));
			ENGINE_TRY(ScriptBindings::RegisterRandom(api));
			ENGINE_TRY(ScriptBindings::RegisterMath(api));
			ENGINE_TRY(ScriptBindings::RegisterQuat(api));
			ENGINE_TRY(ScriptBindings::RegisterColor(api));
			ENGINE_TRY(ScriptBindings::RegisterDebug(api));
			ENGINE_TRY(ScriptBindings::RegisterLog(api));
			ENGINE_TRY(ScriptBindings::RegisterApplication(api));
			ENGINE_TRY(ScriptBindings::RegisterTest(api));
			return {};
		}

	}

	Status RegisterBindings(ScriptApiRegistry& api, const TypeRegistry& types, std::function<Status(ScriptApiRegistry&)> configure)
	{
		ENGINE_TRY(Detail::RegisterBuiltinBindings(api, types));
		if (configure)
			ENGINE_TRY(configure(api));
		return api.Freeze(types);
	}

}
