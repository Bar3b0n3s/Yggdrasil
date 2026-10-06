#include "EnginePCH.h"
#include "Engine/Scene/Scene.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/SceneSerializer.h"

// Scene::ComputeStateHash lives with the serializer that defines it, so Scene.cpp never depends on serializer internals
// (Docs/Decisions/0006-m3-decisions.md, decision 10).

namespace Engine {

	uint64_t Scene::ComputeStateHash() const
	{
		const Result<std::string> canonical = SceneSerializer::SaveToString(*this, JsonStyle::Minified);
		if (!canonical)
		{
			ENGINE_CORE_ERROR("Cannot compute the state hash of scene '{}': {}", GetName(), canonical.error());
			return 0;
		}
		return XXH64(*canonical);
	}

}
