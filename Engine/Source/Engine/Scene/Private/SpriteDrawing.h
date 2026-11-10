#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Scene's half of the sprites' one rule (M21, Docs/Decisions/0032-m21-contract.md decisions 3, 4 and 14): the draw order
// render extraction gives RenderSnapshot::Sprites, which 2D picking follows, and the quad a sprite is drawn with, which
// sprite bounds and 2D picking take. The Sprites stage (Renderer/SceneRenderer.cpp) resolves the same quad through its
// GpuResourceCache, so what is drawn is what is bounded and picked.

namespace Engine {

	class AssetManager;
	class Scene;
	struct SpriteRendererComponent;

	namespace Utils {

		// The index of `name` in the project's sorting layers `sortingLayers` (SortingLayers.Layers), 0 for a name they lack
		// (SPRITE_UNKNOWN_SORTING_LAYER: the sprite draws in the first layer).
		[[nodiscard]] uint32_t FindSortingLayerIndex(std::span<const std::string> sortingLayers, std::string_view name);

		// The snapshot item of `sprite`, drawn by entity `entity` at the world matrix `world`, its sorting layer resolved
		// against `sortingLayers` (RenderExtraction.h's sprite paragraph).
		[[nodiscard]] SpriteItem MakeSpriteItem(const SpriteRendererComponent& sprite, const glm::mat4& world, UUID entity,
			std::span<const std::string> sortingLayers);

		// Sorts `sprites`, listed in canonical entity order, into draw order: stably by (SortingLayer, OrderInLayer), so equal
		// keys keep canonical order (RenderSnapshot::Sprites).
		void SortSpritesForDrawing(std::span<SpriteItem> sprites);

		// Every effectively enabled SpriteRenderer of `scene` at its rendered world matrix of `alpha` (ComputeRenderedWorldMatrix),
		// in draw order: what ExtractRenderSnapshot lists in RenderSnapshot::Sprites. Main thread.
		[[nodiscard]] std::vector<SpriteItem> CollectSprites(const Scene& scene, std::span<const std::string> sortingLayers, float alpha);

		// The corners, in the entity's local XY plane, of the quad `sprite` is drawn with (SpriteRendererComponent.h's rule),
		// in QuadDraw's order (Asset/SpriteGeometry.h), or nullopt when it draws nothing: a sheet that is missing, fails to load
		// or is not a sprite sheet (loaded through `assets`), a Frame the sheet lacks, or PixelsPerUnit that is not finite and
		// positive. Without a sheet the size is the texture's from its loaded data (a null Texture is the built-in white square,
		// BuiltinAssetHandles::SpriteSquareTexture), a missing or failed texture counting with the Missing placeholder's
		// (AssetManager::GetOrPlaceholder, which records the diagnostic). Main thread.
		[[nodiscard]] std::optional<std::array<glm::vec2, 4>> ResolveSpriteCorners(AssetManager& assets, const SpriteItem& sprite);

	}

}
