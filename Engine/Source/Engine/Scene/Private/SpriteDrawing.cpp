#include "EnginePCH.h"
#include "Engine/Scene/Private/SpriteDrawing.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/SpriteGeometry.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace Engine {

	namespace Utils {

		uint32_t FindSortingLayerIndex(std::span<const std::string> sortingLayers, std::string_view name)
		{
			const auto found = std::ranges::find(sortingLayers, name);
			return found == sortingLayers.end() ? 0 : static_cast<uint32_t>(found - sortingLayers.begin());
		}

		SpriteItem MakeSpriteItem(const SpriteRendererComponent& sprite, const glm::mat4& world, UUID entity, std::span<const std::string> sortingLayers)
		{
			return SpriteItem{
				.World = world,
				.Sheet = sprite.Sheet.GetHandle(),
				.Frame = sprite.Frame,
				.Texture = sprite.Texture.GetHandle(),
				.Color = sprite.Color,
				.FlipX = sprite.FlipX,
				.FlipY = sprite.FlipY,
				.PixelsPerUnit = sprite.PixelsPerUnit,
				.SortingLayer = FindSortingLayerIndex(sortingLayers, sprite.SortingLayer),
				.OrderInLayer = sprite.OrderInLayer,
				.Lit = sprite.Lit,
				.NormalMap = sprite.NormalMap.GetHandle(),
				.Entity = entity,
			};
		}

		void SortSpritesForDrawing(std::span<SpriteItem> sprites)
		{
			std::ranges::stable_sort(sprites, [](const SpriteItem& left, const SpriteItem& right)
			{
				if (left.SortingLayer != right.SortingLayer)
					return left.SortingLayer < right.SortingLayer;
				return left.OrderInLayer < right.OrderInLayer;
			});
		}

		std::vector<SpriteItem> CollectSprites(const Scene& scene, std::span<const std::string> sortingLayers, float alpha)
		{
			std::vector<SpriteItem> sprites;
			scene.ForEachCanonical([&sprites, sortingLayers, alpha](ConstEntity entity)
			{
				if (entity.HasComponent<HierarchyDisabledTag>())
					return;
				if (const SpriteRendererComponent* sprite = entity.TryGetComponent<SpriteRendererComponent>())
					sprites.push_back(MakeSpriteItem(*sprite, ComputeRenderedWorldMatrix(entity, alpha), entity.GetUUID(), sortingLayers));
			});
			SortSpritesForDrawing(sprites);
			return sprites;
		}

		std::optional<std::array<glm::vec2, 4>> ResolveSpriteCorners(AssetManager& assets, const SpriteItem& sprite)
		{
			if (!std::isfinite(sprite.PixelsPerUnit) || !(sprite.PixelsPerUnit > 0.0f))
				return std::nullopt;
			std::optional<SpriteFrameGeometry> frame;
			if (sprite.Sheet.IsValid())
			{
				// Sheets have no placeholder: a missing or failed sheet draws nothing (the validator and the Sprites stage report
				// it). Only the frame's rectangle and pivot matter here, never the texture's size.
				const Result<AssetRef<Asset>> loaded = assets.Load(sprite.Sheet);
				const AssetRef<SpriteSheetData> sheet = loaded.has_value() ? AssetCast<SpriteSheetData>(*loaded) : nullptr;
				if (sheet == nullptr)
					return std::nullopt;
				frame = GetSpriteFrameGeometry(sheet.get(), sprite.Frame, glm::uvec2(1));
			}
			else
			{
				const AssetHandle handle = sprite.Texture.IsValid() ? sprite.Texture : BuiltinAssetHandles::SpriteSquareTexture;
				const AssetRef<TextureData> texture = assets.GetOrPlaceholder<TextureData>(handle);
				frame = GetSpriteFrameGeometry(nullptr, 0, glm::uvec2(std::max(texture->Width, 1u), std::max(texture->Height, 1u)));
			}
			if (!frame.has_value())
				return std::nullopt;
			return ComputeSpriteLocalQuad(*frame, sprite.PixelsPerUnit, sprite.FlipX, sprite.FlipY).Corners;
		}

	}

}
