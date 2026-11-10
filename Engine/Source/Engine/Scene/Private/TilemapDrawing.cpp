#include "EnginePCH.h"
#include "Engine/Scene/Private/TilemapDrawing.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/SpriteDrawing.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace Engine {

	namespace Utils {

		TilemapItem MakeTilemapItem(const TilemapRendererComponent& tilemap, const glm::mat4& world, UUID entity, std::span<const std::string> sortingLayers)
		{
			return TilemapItem{
				.World = world,
				.Tilemap = tilemap.Tilemap.GetHandle(),
				.Color = tilemap.Color,
				.PixelsPerUnit = tilemap.PixelsPerUnit,
				.SortingLayer = FindSortingLayerIndex(sortingLayers, tilemap.SortingLayer),
				.OrderInLayer = tilemap.OrderInLayer,
				.Lit = tilemap.Lit,
				.DrawPosition = 0,
				.Entity = entity,
			};
		}

		void PlaceTilemapsForDrawing(std::span<TilemapItem> tilemaps, std::span<const uint32_t> canonical, std::vector<SpriteDrawKey> spriteKeys)
		{
			ENGINE_CORE_ASSERT(tilemaps.size() == canonical.size(), "every tilemap has its canonical position");
			const auto key = [](const SpriteDrawKey& sprite)
			{
				return std::tuple(sprite.SortingLayer, sprite.OrderInLayer, sprite.Canonical);
			};
			std::ranges::sort(spriteKeys, {}, key);
			for (size_t index = 0; index < tilemaps.size(); ++index)
			{
				const TilemapItem& tilemap = tilemaps[index];
				// The sprites before it: those of a lower key, and an entity's own sprite, which draws before its tilemap.
				const SpriteDrawKey place{ .SortingLayer = tilemap.SortingLayer, .OrderInLayer = tilemap.OrderInLayer, .Canonical = canonical[index] };
				const auto after = std::ranges::upper_bound(spriteKeys, key(place), {}, key);
				tilemaps[index].DrawPosition = static_cast<uint32_t>(after - spriteKeys.begin());
			}
			std::ranges::stable_sort(tilemaps, [](const TilemapItem& left, const TilemapItem& right)
			{
				if (left.SortingLayer != right.SortingLayer)
					return left.SortingLayer < right.SortingLayer;
				return left.OrderInLayer < right.OrderInLayer;
			});
		}

		std::vector<TilemapItem> CollectTilemaps(const Scene& scene, std::span<const std::string> sortingLayers, float alpha)
		{
			std::vector<TilemapItem> tilemaps;
			std::vector<uint32_t> canonical;
			std::vector<SpriteDrawKey> spriteKeys;
			uint32_t position = 0;
			scene.ForEachCanonical([&tilemaps, &canonical, &spriteKeys, &position, sortingLayers, alpha](ConstEntity entity)
			{
				if (entity.HasComponent<HierarchyDisabledTag>())
					return;
				if (const SpriteRendererComponent* sprite = entity.TryGetComponent<SpriteRendererComponent>())
				{
					spriteKeys.push_back(SpriteDrawKey{
						.SortingLayer = FindSortingLayerIndex(sortingLayers, sprite->SortingLayer), .OrderInLayer = sprite->OrderInLayer, .Canonical = position });
				}
				if (const TilemapRendererComponent* tilemap = entity.TryGetComponent<TilemapRendererComponent>(); tilemap != nullptr && tilemap->Tilemap.IsValid())
				{
					tilemaps.push_back(MakeTilemapItem(*tilemap, ComputeRenderedWorldMatrix(entity, alpha), entity.GetUUID(), sortingLayers));
					canonical.push_back(position);
				}
				++position;
			});
			PlaceTilemapsForDrawing(tilemaps, canonical, std::move(spriteKeys));
			return tilemaps;
		}

		std::optional<TilemapTiles> ResolveTilemapTiles(AssetManager& assets, const TilemapItem& item)
		{
			if (!item.Tilemap.IsValid() || !std::isfinite(item.PixelsPerUnit) || !(item.PixelsPerUnit > 0.0f))
				return std::nullopt;
			// Tilemaps have no placeholder: a missing or failed one draws nothing (the validator and the Sprites stage report it).
			const Result<AssetRef<Asset>> loaded = assets.Load(item.Tilemap);
			AssetRef<TilemapData> tilemap = loaded.has_value() ? AssetCast<TilemapData>(*loaded) : nullptr;
			if (tilemap == nullptr)
				return std::nullopt;
			TilemapTiles tiles;
			tiles.Palette.reserve(tilemap->Palette.size());
			for (const TilemapTile& entry : tilemap->Palette)
			{
				// A null Sheet draws nothing (a sprite without a sheet would draw a texture instead).
				if (!entry.Sheet.GetHandle().IsValid())
				{
					tiles.Palette.emplace_back();
					continue;
				}
				const SpriteItem sprite{ .Sheet = entry.Sheet.GetHandle(), .Frame = entry.Frame, .PixelsPerUnit = item.PixelsPerUnit, .Entity = item.Entity };
				tiles.Palette.push_back(ResolveSpriteCorners(assets, sprite));
			}
			tiles.Tilemap = std::move(tilemap);
			return tiles;
		}

	}

}
