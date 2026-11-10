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
	// GetOrPlaceholder, which records the diagnostic). M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 27): a mesh
	// with a skin or morph targets counts with the bounds of its deformation as the renderer draws it (Asset/MeshDeform.h's
	// ComputeDeformedBounds): its SkinnedMesh's morph weights overridden by the driving Animator's pose (the nearest Animator
	// on the entity or an ancestor, its AnimationPoseComponent), the pose's skin matrices when they cover the skin (without
	// them, the bind pose), and its targets' default weights for the weights missing. nullopt when no included entity has a
	// MeshRenderer with a mesh.
	// `entity` is readable (Entity::IsReadable): an entity pending destruction and its subtree, also pending, still have
	// their bounds until the flush (Entity:GetWorldBounds; M20, decision 4 of Docs/Decisions/0031-m20-contract.md).
	// Main thread (the scene and the manager).
	[[nodiscard]] std::optional<Aabb> ComputeEntityWorldBounds(ConstEntity entity, AssetManager& assets, const EntityBoundsOptions& options = {});

}
