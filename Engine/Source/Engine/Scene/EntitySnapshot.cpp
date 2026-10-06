#include "EnginePCH.h"
#include "Engine/Scene/Scene.h"

#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"

// Scene::CaptureEntitySnapshot lives with the serializer that defines the snapshot (stream C), like
// Scene::ComputeStateHash in SceneStateHash.cpp, so stream B's Scene.cpp and stream C's files stay disjoint. M3 contract
// stub (Roadmap rule 3).

namespace Engine {

	Ref<const Json> Scene::CaptureEntitySnapshot(entt::entity /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
