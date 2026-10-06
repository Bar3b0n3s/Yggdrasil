#include "EnginePCH.h"
#include "Engine/Scene/Scene.h"

#include "Engine/Core/Hash.h"
#include "Engine/Scene/SceneSerializer.h"

// Scene::ComputeStateHash lives with the serializer it is defined by (stream C), so stream B's Scene.cpp and stream C's
// files stay disjoint. M3 contract stub (Roadmap rule 3).

namespace Engine {

	uint64_t Scene::ComputeStateHash() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
