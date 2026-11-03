#include "TestsPCH.h"

#include "Engine/Scene/EntityBounds.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <cstddef>

namespace Engine {

	namespace {

		Entity AddCube(Scene& scene, std::string_view name, const glm::vec3& translation, const glm::vec3& scale)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([&translation, &scale](TransformComponent& transform)
			{
				transform.Translation = translation;
				transform.Scale = scale;
			});
			entity.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			return entity;
		}

		// A `width` x `height` opaque white texture: the bounds take only its size.
		Ref<TextureData> MakeBoundsTexture(uint32_t width, uint32_t height)
		{
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = width;
			texture->Height = height;
			texture->Mips = { TextureMip{ .Width = width, .Height = height, .Offset = 0, .Size = static_cast<uint64_t>(width) * height * 4 } };
			texture->Pixels.assign(static_cast<size_t>(width) * height * 4, std::byte{ 255 });
			return texture;
		}

		Entity AddSprite(Scene& scene, std::string_view name, const glm::vec3& translation, const glm::vec3& scale, const SpriteRendererComponent& sprite)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([&translation, &scale](TransformComponent& transform)
			{
				transform.Translation = translation;
				transform.Scale = scale;
			});
			entity.AddComponent<SpriteRendererComponent>(sprite);
			return entity;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("EntityBounds: a translated and scaled unit cube has the transformed world AABB")
		{
			Test::AssetTestFixture assets;
			Test::SceneTestFixture scene;
			const Entity box = AddCube(scene.GetScene(), "Box", glm::vec3(10.0f, 1.0f, 0.0f), glm::vec3(2.0f, 1.0f, 4.0f));
			const std::optional<Aabb> bounds = ComputeEntityWorldBounds(box, assets.GetManager());
			REQUIRE(bounds.has_value());
			CHECK(bounds->Min == glm::vec3(9.0f, 0.5f, -2.0f));
			CHECK(bounds->Max == glm::vec3(11.0f, 1.5f, 2.0f));

			const Entity empty = scene.GetScene().CreateEntity("Empty");
			CHECK_FALSE(ComputeEntityWorldBounds(empty, assets.GetManager()).has_value());
		}

		TEST_CASE("EntityBounds: descendants are included and disabled ones skipped")
		{
			Test::AssetTestFixture assets;
			Test::SceneTestFixture scene;
			Entity root = scene.GetScene().CreateEntity("Track");
			Entity first = AddCube(scene.GetScene(), "Piece", glm::vec3(0.0f), glm::vec3(1.0f));
			Entity second = AddCube(scene.GetScene(), "Piece", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f));
			Entity hidden = AddCube(scene.GetScene(), "Hidden", glm::vec3(-50.0f, 0.0f, 0.0f), glm::vec3(1.0f));
			REQUIRE(scene.GetScene().SetParent(first, root).has_value());
			REQUIRE(scene.GetScene().SetParent(second, root).has_value());
			REQUIRE(scene.GetScene().SetParent(hidden, root).has_value());
			hidden.SetActive(false);

			const std::optional<Aabb> all = ComputeEntityWorldBounds(root, assets.GetManager());
			REQUIRE(all.has_value());
			CHECK(all->Min == glm::vec3(-0.5f, -0.5f, -0.5f));
			CHECK(all->Max == glm::vec3(5.5f, 0.5f, 0.5f));
			// Without descendants the root has no mesh of its own.
			CHECK_FALSE(ComputeEntityWorldBounds(root, assets.GetManager(), { .IncludeDescendants = false }).has_value());
		}

		TEST_CASE("EntityBounds: a missing mesh counts with the placeholder cube and records the diagnostic")
		{
			Test::AssetTestFixture assets;
			Test::SceneTestFixture scene;
			const AssetHandle missing(0x123456789ull);
			Entity box = scene.GetScene().CreateEntity("Box");
			box.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 2.0f, 0.0f);
			});
			box.AddComponent<MeshRendererComponent>().Mesh.SetHandle(missing);
			std::optional<Aabb> bounds;
			{
				const Test::ExpectLog logged(LogLevel::Error, missing.ToString());
				bounds = ComputeEntityWorldBounds(box, assets.GetManager());
			}
			REQUIRE(bounds.has_value());
			CHECK(bounds->Min == glm::vec3(-0.5f, 1.5f, -0.5f));
			CHECK(bounds->Max == glm::vec3(0.5f, 2.5f, 0.5f));
			CHECK(std::ranges::any_of(assets.GetManager().GetDiagnostics(), [missing](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == missing && diagnostic.Code == AssetMissingCode;
			}));
			// MeshRenderer.Visible does not matter: an invisible mesh still occupies its space.
			box.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Visible = false;
				renderer.Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			});
			CHECK(ComputeEntityWorldBounds(box, assets.GetManager()).has_value());
		}

		// M21 (Docs/Decisions/0032-m21-contract.md decision 3, stream A): a sprite is bounded by the quad it is drawn with
		// (Asset/SpriteGeometry.h at its entity's world matrix): a flat box in the entity's XY plane.
		TEST_CASE("EntityBounds: a sprite counts with the world box of the quad it is drawn with")
		{
			Test::SceneTestFixture scene;
			Test::InMemoryAssetManager assets;
			const AssetHandle textureHandle = UUID(8940);
			assets.Publish(textureHandle, MakeBoundsTexture(16, 8), "Assets/Textures/Bounds.png");
			// 16 x 8 texels at 8 pixels per unit: 2 x 1 units about the centre, then the entity's scale and translation.
			const Entity texture = AddSprite(scene.GetScene(), "Texture", glm::vec3(10.0f, 1.0f, 3.0f), glm::vec3(2.0f, 1.0f, 1.0f),
				SpriteRendererComponent{ .Texture = TypedAssetHandle<AssetType::Texture>(textureHandle), .PixelsPerUnit = 8.0f });
			const std::optional<Aabb> textureBounds = ComputeEntityWorldBounds(texture, assets);
			REQUIRE(textureBounds.has_value());
			CHECK(textureBounds->Min == glm::vec3(8.0f, 0.5f, 3.0f));
			CHECK(textureBounds->Max == glm::vec3(12.0f, 1.5f, 3.0f));

			// A sheet's frame of 16 x 16 texels pivoted at its bottom-left corner, mirrored by FlipX: the quad lies left of the
			// pivot and above it, whatever the sheet's texture (it has none).
			const AssetHandle sheetHandle = UUID(8941);
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			sheet->Frames.push_back(SpriteFrame{ .Rect = { .X = 16, .Y = 0, .Width = 16, .Height = 16 }, .Pivot = glm::vec2(0.0f, 1.0f) });
			assets.Publish(sheetHandle, sheet, "Assets/Sprites/Bounds.spritesheet");
			const Entity frame = AddSprite(scene.GetScene(), "Frame", glm::vec3(0.0f), glm::vec3(1.0f),
				SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle), .FlipX = true });
			const std::optional<Aabb> frameBounds = ComputeEntityWorldBounds(frame, assets);
			REQUIRE(frameBounds.has_value());
			CHECK(frameBounds->Min == glm::vec3(-1.0f, 0.0f, 0.0f));
			CHECK(frameBounds->Max == glm::vec3(0.0f, 1.0f, 0.0f));

			// A parent's bounds merge its children's quads.
			Entity root = scene.GetScene().CreateEntity("Root");
			REQUIRE(scene.GetScene().SetParent(texture, root).has_value());
			REQUIRE(scene.GetScene().SetParent(frame, root).has_value());
			const std::optional<Aabb> both = ComputeEntityWorldBounds(root, assets);
			REQUIRE(both.has_value());
			CHECK(both->Min == glm::vec3(-1.0f, 0.0f, 0.0f));
			CHECK(both->Max == glm::vec3(12.0f, 1.5f, 3.0f));
		}

		// SpriteRendererComponent.h's one rule: a missing texture is its placeholder (the 64 x 64 Missing checker) over the
		// sprite's quad, while a missing sheet or a frame the sheet lacks gives no quad.
		TEST_CASE("EntityBounds: a sprite with a missing texture counts with its placeholder's quad, and a missing sheet or frame adds nothing")
		{
			Test::SceneTestFixture scene;
			Test::InMemoryAssetManager assets;
			const AssetHandle missing(0x89420000ull);
			const Entity placeholder = AddSprite(scene.GetScene(), "Placeholder", glm::vec3(0.0f), glm::vec3(1.0f),
				SpriteRendererComponent{ .Texture = TypedAssetHandle<AssetType::Texture>(missing) });
			std::optional<Aabb> bounds;
			{
				const Test::ExpectLog logged(LogLevel::Error, missing.ToString());
				bounds = ComputeEntityWorldBounds(placeholder, assets);
			}
			REQUIRE(bounds.has_value());
			// 64 texels at the default 16 pixels per unit: 4 units.
			CHECK(bounds->Min == glm::vec3(-2.0f, -2.0f, 0.0f));
			CHECK(bounds->Max == glm::vec3(2.0f, 2.0f, 0.0f));

			const Entity noSheet = AddSprite(scene.GetScene(), "NoSheet", glm::vec3(0.0f), glm::vec3(1.0f),
				SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(AssetHandle(0x89430000ull)) });
			CHECK_FALSE(ComputeEntityWorldBounds(noSheet, assets).has_value());

			const AssetHandle sheetHandle = UUID(8944);
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			sheet->Frames.push_back(SpriteFrame{ .Rect = { .Width = 8, .Height = 8 } });
			assets.Publish(sheetHandle, sheet, "Assets/Sprites/One.spritesheet");
			const Entity noFrame = AddSprite(scene.GetScene(), "NoFrame", glm::vec3(0.0f), glm::vec3(1.0f),
				SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle), .Frame = 1 });
			CHECK_FALSE(ComputeEntityWorldBounds(noFrame, assets).has_value());
		}

		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24, stream T1): a tilemap is bounded by the quads of the tiles it
		// draws, each a sprite of its palette entry's frame at its cell's centre.
		TEST_CASE("EntityBounds: a tilemap's bounds are its drawn tiles' quads" * doctest::skip(true))
		{
			Test::SceneTestFixture scene;
			Test::InMemoryAssetManager assets;
			const AssetHandle sheetHandle = UUID(8950);
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			sheet->Frames.push_back(SpriteFrame{ .Rect = { .Width = 16, .Height = 16 } });
			assets.Publish(sheetHandle, sheet, "Assets/Sprites/Tiles.spritesheet");
			// Cells of 2 units: cell (0, 0) and cell (3, 1) hold the 16-texel frame (2 units at 8 pixels per unit, centred on
			// (1, 1) and (7, 3)); entry 2's frame 5 is not in the sheet, so cell (2, 3) draws nothing and adds no bounds.
			const AssetHandle tilemapHandle = UUID(8951);
			Ref<TilemapData> tilemap = CreateRef<TilemapData>();
			tilemap->CellSize = glm::vec2(2.0f);
			tilemap->ChunkSize = 4;
			tilemap->Palette = { TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle), .Frame = 0 },
				TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle), .Frame = 5 } };
			tilemap->Chunks = { TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 2, 0 } } };
			assets.Publish(tilemapHandle, tilemap, "Assets/Tilemaps/Bounds.tilemap");
			Entity entity = scene.GetScene().CreateEntity("Tiles");
			entity.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 2.0f);
			});
			entity.AddComponent<TilemapRendererComponent>(
				TilemapRendererComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(tilemapHandle), .PixelsPerUnit = 8.0f });
			const std::optional<Aabb> bounds = ComputeEntityWorldBounds(entity, assets);
			REQUIRE(bounds.has_value());
			CHECK(bounds->Min == glm::vec3(10.0f, 0.0f, 2.0f));
			CHECK(bounds->Max == glm::vec3(18.0f, 4.0f, 2.0f));

			// A missing tilemap adds nothing.
			Entity missing = scene.GetScene().CreateEntity("Missing");
			missing.AddComponent<TilemapRendererComponent>(TilemapRendererComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(AssetHandle(0x89600000ull)) });
			CHECK_FALSE(ComputeEntityWorldBounds(missing, assets).has_value());
		}
	}

}
