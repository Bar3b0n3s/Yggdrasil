#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Scene/Entity.h"

#include <optional>

// World-space bounds of entities (Architecture §13.5 entity.bounds "world AABBs", §13.7 layout feedback; from M13 also
// Entity:GetWorldBounds, §11.5). Collider bounds (Physics.GetColliderBounds) are M11's and come from the physics world.

namespace Engine {

	class AssetManager;

	struct EntityBoundsOptions
	{
		// Also include every descendant (the whole visual extent of a prefab instance whose meshes sit on children).
		bool IncludeDescendants = true;
	};

	// The world AABB of `entity`'s MeshRenderer mesh (and, with IncludeDescendants, of every descendant's), each mesh's local
	// bounds (MeshData::Bounds) transformed by its entity's world matrix (TransformSystem::ComputeWorldMatrix, so it is
	// correct between TransformSystem updates) and merged. Effectively disabled entities (HierarchyDisabledTag) are skipped,
	// as rendering skips them; MeshRenderer::Visible does not matter (an invisible mesh still occupies its space for layout).
	// A mesh that is missing or fails to load counts with its placeholder's bounds (the unit cube, AssetManager::
	// GetOrPlaceholder, which records the diagnostic). nullopt when no included entity has a MeshRenderer with a mesh.
	// Main thread (the scene and the manager).
	[[nodiscard]] std::optional<Aabb> ComputeEntityWorldBounds(ConstEntity entity, AssetManager& assets, const EntityBoundsOptions& options = {});

}
