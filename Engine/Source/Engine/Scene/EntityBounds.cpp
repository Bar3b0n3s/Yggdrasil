#include "EnginePCH.h"
#include "Engine/Scene/EntityBounds.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Private/SpriteDrawing.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <array>
#include <optional>
#include <vector>

namespace Engine {

	namespace Utils {

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
			bounds.Extend(mesh->Bounds.Transformed(TransformSystem::ComputeWorldMatrix(entity)));
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
