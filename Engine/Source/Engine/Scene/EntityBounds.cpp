#include "EnginePCH.h"
#include "Engine/Scene/EntityBounds.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/MeshDeform.h"
#include "Engine/Asset/TilemapDrawOrder.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Private/AnimationTargets.h"
#include "Engine/Scene/Private/SpriteDrawing.h"
#include "Engine/Scene/Private/TilemapDrawing.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Engine {

	namespace Utils {

		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27): the world bounds of `entity`'s
		// deformed `mesh` (one with a skin or morph targets) as the renderer draws it, from what render extraction gives its
		// item (RenderSnapshot.h's MeshDrawItem): with a SkinnedMesh, its MorphWeights, each replaced by the weight the driving
		// Animator's pose (the nearest Animator on the entity or an ancestor) holds at its index in the channel whose Target is
		// the entity's path from that Animator ('/'-separated names, empty for the Animator's own entity), as long as the
		// longer of the two; and, when that pose covers the skin, the skin matrices carried into world space by the Animator
		// entity's world matrix. ComputeDeformedBounds of that world palette is in world space; without one, the mesh's
		// deformed bounds (its targets' defaults for missing weights) are carried by the entity's world matrix.
		static Aabb GetDeformedWorldBounds(ConstEntity entity, const MeshData& mesh, const glm::mat4& world)
		{
			std::vector<glm::mat4> palette;
			std::vector<float> weights;
			if (const SkinnedMeshComponent* skinned = entity.TryGetComponent<SkinnedMeshComponent>(); skinned != nullptr)
			{
				std::span<const float> animated;
				const ConstEntity animator = FindDrivingAnimator(entity);
				const AnimationPoseComponent* pose = animator.IsValid() ? animator.TryGetComponent<AnimationPoseComponent>() : nullptr;
				if (pose != nullptr)
				{
					if (const std::optional<std::string> target = Detail::GetAnimationTargetPath(animator, entity))
					{
						if (const AnimationMorphWeights* morph = Detail::FindPoseMorphWeights(pose->Morphs, *target))
							animated = morph->Weights;
					}
					if (mesh.SkinJointCount > 0 && !mesh.Skin.empty() && pose->Skin.size() >= mesh.SkinJointCount)
					{
						const glm::mat4 animatorWorld = TransformSystem::ComputeWorldMatrix(animator);
						palette.reserve(pose->Skin.size());
						for (const glm::mat4& skin : pose->Skin)
							palette.push_back(animatorWorld * skin);
					}
				}
				weights = Detail::MergeAnimatedMorphWeights(skinned->MorphWeights, animated);
			}
			const MeshDeformBounds deformBounds = ComputeMeshDeformBounds(mesh);
			if (!palette.empty())
				return ComputeDeformedBounds(deformBounds, palette, weights);
			return ComputeDeformedBounds(deformBounds, {}, weights).Transformed(world);
		}

		// Extends `bounds` by the world box of `entity`'s mesh, when it has a MeshRenderer with a mesh that has vertices.
		// Returns whether it did.
		static bool AddMeshBounds(ConstEntity entity, AssetManager& assets, Aabb& bounds)
		{
			const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>();
			if (renderer == nullptr || !renderer->Mesh.GetHandle().IsValid())
				return false;
			// Never null: a missing or failed mesh is the unit cube placeholder, with its diagnostic recorded.
			const AssetRef<MeshData> mesh = assets.GetOrPlaceholder<MeshData>(renderer->Mesh.GetHandle());
			if (mesh->Bounds.IsEmpty())
				return false;
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(entity);
			if ((!mesh->Skin.empty() && mesh->SkinJointCount > 0) || !mesh->MorphTargets.empty())
				bounds.Extend(GetDeformedWorldBounds(entity, *mesh, world));
			else
				bounds.Extend(mesh->Bounds.Transformed(world));
			return true;
		}

		// M21 (Docs/Decisions/0032-m21-contract.md decision 3): extends `bounds` by the world box of the quad `entity`'s
		// SpriteRenderer is drawn with, when it has one with a quad. Returns whether it did.
		static bool AddSpriteBounds(ConstEntity entity, AssetManager& assets, Aabb& bounds)
		{
			const SpriteRendererComponent* sprite = entity.TryGetComponent<SpriteRendererComponent>();
			if (sprite == nullptr)
				return false;
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(entity);
			// Only the quad's corners matter, so the sorting layers are left out.
			const std::optional<std::array<glm::vec2, 4>> corners = ResolveSpriteCorners(assets, MakeSpriteItem(*sprite, world, entity.GetUUID(), {}));
			if (!corners.has_value())
				return false;
			for (const glm::vec2& corner : *corners)
				bounds.Extend(glm::vec3(world * glm::vec4(corner, 0.0f, 1.0f)));
			return true;
		}

		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24): extends `bounds` by the world box of every tile
		// `entity`'s TilemapRenderer draws, each tile's quad as its sprite's at its cell's centre (TilemapDrawing.h). Returns
		// whether it did.
		static bool AddTilemapBounds(ConstEntity entity, AssetManager& assets, Aabb& bounds)
		{
			const TilemapRendererComponent* renderer = entity.TryGetComponent<TilemapRendererComponent>();
			if (renderer == nullptr || !renderer->Tilemap.IsValid())
				return false;
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(entity);
			// Only the tiles' quads matter, so the sorting layers are left out.
			const std::optional<TilemapTiles> tiles = ResolveTilemapTiles(assets, MakeTilemapItem(*renderer, world, entity.GetUUID(), {}));
			if (!tiles.has_value())
				return false;
			bool found = false;
			const auto includeChunk = [](size_t /*chunk*/)
			{
				return true;
			};
			ForEachTilemapTileInDrawOrder(*tiles->Tilemap, false, includeChunk, [&tiles, &world, &bounds, &found](size_t /*chunk*/, glm::ivec2 cell, uint32_t value)
			{
				if (value > tiles->Palette.size() || !tiles->Palette[value - 1].has_value())
					return true;
				const glm::mat4 tileWorld = GetTilemapTileWorld(world, *tiles->Tilemap, cell);
				for (const glm::vec2& corner : *tiles->Palette[value - 1])
					bounds.Extend(glm::vec3(tileWorld * glm::vec4(corner, 0.0f, 1.0f)));
				found = true;
				return true;
			});
			return found;
		}

	}

	std::optional<Aabb> ComputeEntityWorldBounds(ConstEntity entity, AssetManager& assets, const EntityBoundsOptions& options)
	{
		Aabb bounds;
		bool found = false;
		// Depth first over the subtree; an effectively disabled entity is skipped, and so are its descendants, which are
		// effectively disabled too (§5.2).
		std::vector<ConstEntity> pending = { entity };
		const Scene* scene = entity.GetScene();
		while (!pending.empty())
		{
			const ConstEntity current = pending.back();
			pending.pop_back();
			if (!current.IsReadable() || !current.IsActive())
				continue;
			found = Utils::AddMeshBounds(current, assets, bounds) || found;
			found = Utils::AddSpriteBounds(current, assets, bounds) || found;
			found = Utils::AddTilemapBounds(current, assets, bounds) || found;
			if (!options.IncludeDescendants)
				break;
			for (const UUID child : current.GetChildren())
				pending.push_back(scene->FindReadableEntityByID(child));
		}
		if (!found)
			return std::nullopt;
		return bounds;
	}

}
