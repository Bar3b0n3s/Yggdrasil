#include "EnginePCH.h"
#include "Engine/Scene/PrefabInstantiator.h"

#include "Engine/Core/Hash.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream E (prefabs, after B and C land) implements instantiation, updates and
// overrides.

namespace Engine {

	UUID PrefabInstantiator::DeriveInstanceID(UUID /*instanceRootID*/, UUID /*prefabEntityID*/)
	{
		ENGINE_CONTRACT_STUB();
		return UUID();
	}

	Result<Entity> PrefabInstantiator::Instantiate(Scene& /*scene*/, const Prefab& /*prefab*/, const PrefabInstantiateOptions& /*instance*/,
		const PrefabOptions& /*options*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PrefabInstantiator::Instantiate is an M3 contract stub");
	}

	Status PrefabInstantiator::UpdateInstance(Scene& /*scene*/, Entity /*instanceRoot*/, const Prefab& /*prefab*/, const PrefabOptions& /*options*/,
		LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PrefabInstantiator::UpdateInstance is an M3 contract stub");
	}

	Result<std::vector<PrefabOverride>> PrefabInstantiator::ComputeOverrides(Entity /*instanceRoot*/, const Prefab& /*prefab*/,
		const PrefabOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PrefabInstantiator::ComputeOverrides is an M3 contract stub");
	}

	Status PrefabInstantiator::RefreshOverrides(Entity /*instanceRoot*/, const Prefab& /*prefab*/, const PrefabOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PrefabInstantiator::RefreshOverrides is an M3 contract stub");
	}

	Status PrefabInstantiator::Revert(Scene& /*scene*/, Entity /*instanceRoot*/, const Prefab& /*prefab*/, const PrefabOptions& /*options*/,
		LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PrefabInstantiator::Revert is an M3 contract stub");
	}

	Status PrefabInstantiator::Unpack(Entity /*instanceRoot*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PrefabInstantiator::Unpack is an M3 contract stub");
	}

	Result<Prefab> PrefabInstantiator::ApplyOverrides(Entity /*instanceRoot*/, const Prefab& /*prefab*/, const PrefabOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PrefabInstantiator::ApplyOverrides is an M3 contract stub");
	}

}
