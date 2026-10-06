#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Runtime-only components (Architecture §5.2, §5.3, §5.7): never registered, never serialized, invisible to scripts and
	// automation. They are rebuilt from the authored components, so the serializer copy that starts a play session never
	// needs them. Adding, removing or patching them through Entity neither increments the scene's revision nor reaches
	// the change tracker (Scene::PrepareEntityChange ignores unregistered types other than DisabledTag), so rendering an
	// edit scene, which runs TransformSystem::Update, leaves its revision unchanged. The physics, character, audio and
	// script runtime components of §5.3 are defined by the milestones that own their contents (M11, M12, M13;
	// Docs/Decisions/0006-m3-decisions.md, decision 8).

	// The entity's world matrix, recomputed for every entity by TransformSystem::Update in canonical order.
	struct WorldTransformComponent
	{
		glm::mat4 Matrix = glm::mat4(1.0f);
	};

	// The world matrix at the previous fixed step: copied from WorldTransformComponent for every entity at step 0 of each
	// fixed step (§5.7), and interpolated towards the current one by render extraction (M7).
	struct PreviousWorldTransformComponent
	{
		glm::mat4 Matrix = glm::mat4(1.0f);
	};

	// The entity renders at its current world pose this frame instead of interpolating (§5.2): its transform was written
	// in the frame phase, it was created or enabled since the last step, or it was teleported. Cleared at step 0.
	struct InterpolationResetTag
	{
	};

	// The entity is effectively disabled: it or an ancestor has DisabledTag (§5.2). Render extraction, physics, audio and
	// script updates skip it.
	struct HierarchyDisabledTag
	{
	};

	// Scene::DestroyEntity on a runtime scene: the entity is no longer valid (Entity::IsValid is false) and is destroyed at
	// the next flush point (§5.7 step 8, Scene::FlushPendingDestroys).
	struct PendingDestroyTag
	{
	};

	// The entity's script instance has run OnCreate but not yet OnStart; the next start flush runs it (§5.7, M13).
	struct PendingStartTag
	{
	};

}
