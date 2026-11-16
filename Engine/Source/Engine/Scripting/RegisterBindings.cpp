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
			ENGINE_TRY(ScriptBindings::RegisterSprites(api));
			ENGINE_TRY(ScriptBindings::RegisterScene(api));
			ENGINE_TRY(ScriptBindings::RegisterInput(api));
			ENGINE_TRY(ScriptBindings::RegisterTime(api));
			ENGINE_TRY(ScriptBindings::RegisterPhysics(api));
			ENGINE_TRY(ScriptBindings::RegisterPhysics2D(api));
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
			// M22 (Docs/Decisions/0033-m22-contract.md decision 11); phase 2's modules append in landing order.
			ENGINE_TRY(ScriptBindings::RegisterLoc(api));
			// M22 (decision 28): the Ui module, after Loc.
			ENGINE_TRY(ScriptBindings::RegisterUi(api));
			// M23 (Docs/Decisions/0034-m23-contract.md decision 31): the Animator and SkinnedMesh proxies' methods, after M21's
			// and M22's registrations (landing order).
			ENGINE_TRY(ScriptBindings::RegisterAnimation(api));
			// M26 (Docs/Decisions/0037-m26-contract.md decision 26): the ParticleEmitter proxy's methods, after the earlier
			// milestones' registrations (landing order).
			ENGINE_TRY(ScriptBindings::RegisterParticles(api));
			// M26 (Docs/Decisions/0037-m26-contract.md decision 26): the Terrain proxy's methods, after the earlier milestones'
			// registrations (landing order).
			ENGINE_TRY(ScriptBindings::RegisterTerrain(api));
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): the SaveData module, after RegisterTerrain (phase 5's
			// ledger: before M30's RegisterNavigation).
			ENGINE_TRY(ScriptBindings::RegisterSaveData(api));
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
