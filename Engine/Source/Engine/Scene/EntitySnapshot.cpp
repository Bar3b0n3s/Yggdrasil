#include "EnginePCH.h"
#include "Engine/Scene/Scene.h"

#include "Engine/Core/Log.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

// Scene::CaptureEntitySnapshot lives with the serializer that defines the snapshot, like Scene::ComputeStateHash in
// SceneStateHash.cpp, so Scene.cpp never depends on serializer internals (Docs/Decisions/0006-m3-decisions.md, decision 10).

namespace Engine {

	Ref<const Json> Scene::CaptureEntitySnapshot(entt::entity entity) const
	{
		Result<Json> snapshot = SceneSerializer::EntityToJson(ConstEntity(entity, this));
		if (!snapshot)
		{
			ENGINE_CORE_ERROR("Cannot snapshot an entity of scene '{}' for the change tracker: {}", GetName(), snapshot.error());
			return nullptr;
		}
		return CreateRef<const Json>(std::move(*snapshot));
	}

}
