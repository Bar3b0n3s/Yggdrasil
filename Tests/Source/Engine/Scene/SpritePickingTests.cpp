#include "TestsPCH.h"

#include "Engine/Scene/SpritePicking.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

// 2D picking (Docs/Decisions/0032-m21-contract.md decision 14, stream A).

namespace Engine {

	namespace {

		// 16 x 16 opaque white texels: one unit at the default 16 pixels per unit. The test's own texture, so the case needs
		// neither stream B's built-in sprite textures nor its catalogue.
		const AssetHandle PickTextureHandle = UUID(8930);

		Ref<TextureData> MakePickTexture()
		{
			constexpr uint32_t Texels = 16;
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = Texels;
			texture->Height = Texels;
			texture->Mips = { TextureMip{ .Width = Texels, .Height = Texels, .Offset = 0, .Size = Texels * Texels * 4 } };
			texture->Pixels.assign(static_cast<size_t>(Texels) * Texels * 4, std::byte{ 255 });
			return texture;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("SpritePicking: the last-drawn sprite under the ray is picked, whatever its distance")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			assets.Publish(PickTextureHandle, MakePickTexture(), "Assets/Textures/Pick.png");
			const TypedAssetHandle<AssetType::Texture> texture(PickTextureHandle);
			const auto addSprite = [&scene, texture](std::string_view name, glm::vec3 position, std::string layer, int32_t order)
			{
				Entity entity = scene.CreateEntity(name);
				entity.Patch<TransformComponent>([position](TransformComponent& transform)
				{
					transform.Translation = position;
				});
				entity.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Texture = texture, .SortingLayer = std::move(layer), .OrderInLayer = order });
				return entity.GetUUID();
			};
			// The nearest sprite draws first, so the farther one, drawn over it, is what the pixel shows.
			static_cast<void>(addSprite("Near", glm::vec3(0.0f, 0.0f, 1.0f), std::string(DefaultSortingLayerName), 0));
			const UUID far = addSprite("Far", glm::vec3(0.0f, 0.0f, -1.0f), std::string(DefaultSortingLayerName), 1);
			static_cast<void>(addSprite("Aside", glm::vec3(5.0f, 0.0f, 0.0f), std::string(DefaultSortingLayerName), 9));
			TransformSystem::Update(scene);

			SpritePickRequest request;
			request.Ray = RenderRay{ .Origin = glm::vec3(0.1f, 0.2f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, -1.0f) };
			request.SortingLayers = { std::string(DefaultSortingLayerName) };
			const Result<std::optional<SceneRaycastHit>> hit = PickSprite(scene, assets, request);
			REQUIRE_MESSAGE(hit.has_value(), hit.error().ToString());
			REQUIRE(hit->has_value());
			CHECK((*hit)->Entity == far);
			CHECK((*hit)->Distance == doctest::Approx(11.0f));

			// A ray beside every sprite hits none.
			request.Ray.Origin = glm::vec3(2.5f, 0.0f, 10.0f);
			const Result<std::optional<SceneRaycastHit>> miss = PickSprite(scene, assets, request);
			REQUIRE(miss.has_value());
			CHECK_FALSE(miss->has_value());

			// A sprite of a later sorting layer draws last, whatever its OrderInLayer; until its layer is declared, it sorts in
			// the first layer, where Far's higher OrderInLayer still draws over it.
			const UUID top = addSprite("Top", glm::vec3(0.0f, 0.0f, 0.5f), "Front", -5);
			TransformSystem::Update(scene);
			request.Ray.Origin = glm::vec3(0.1f, 0.2f, 10.0f);
			const Result<std::optional<SceneRaycastHit>> undeclared = PickSprite(scene, assets, request);
			REQUIRE(undeclared.has_value());
			REQUIRE(undeclared->has_value());
			CHECK((*undeclared)->Entity == far);
			request.SortingLayers.push_back("Front");
			const Result<std::optional<SceneRaycastHit>> layered = PickSprite(scene, assets, request);
			REQUIRE(layered.has_value());
			REQUIRE(layered->has_value());
			CHECK((*layered)->Entity == top);
		}

		TEST_CASE("SpritePicking: a hit reports the quad's triangle, normal and position, and an invalid request is InvalidArgument")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			assets.Publish(PickTextureHandle, MakePickTexture(), "Assets/Textures/Pick.png");
			Entity sprite = scene.CreateEntity("Sprite");
			sprite.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Texture = TypedAssetHandle<AssetType::Texture>(PickTextureHandle) });
			// A sprite of zero scale has no quad to hit.
			Entity flat = scene.CreateEntity("Flat");
			flat.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(0.0f);
			});
			flat.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Texture = TypedAssetHandle<AssetType::Texture>(PickTextureHandle), .OrderInLayer = 1 });
			TransformSystem::Update(scene);

			// The unit quad's corners are (+-0.5, +-0.5): (0.25, 0.25) lies in triangle 0 (top-left, top-right, bottom-right) and
			// (-0.25, -0.25) in triangle 1 (top-left, bottom-right, bottom-left); the normal faces the ray.
			SpritePickRequest request;
			request.Ray = RenderRay{ .Origin = glm::vec3(0.25f, 0.25f, -4.0f), .Direction = glm::vec3(0.0f, 0.0f, 2.0f) };
			const Result<std::optional<SceneRaycastHit>> upper = PickSprite(scene, assets, request);
			REQUIRE_MESSAGE(upper.has_value(), upper.error().ToString());
			REQUIRE(upper->has_value());
			CHECK((*upper)->Entity == sprite.GetUUID());
			CHECK((*upper)->Triangle == 0);
			CHECK((*upper)->Submesh == 0);
			CHECK((*upper)->Distance == 4.0f);
			CHECK((*upper)->Position == glm::vec3(0.25f, 0.25f, 0.0f));
			CHECK((*upper)->Normal == glm::vec3(0.0f, 0.0f, -1.0f));
			request.Ray.Origin = glm::vec3(-0.25f, -0.25f, -4.0f);
			const Result<std::optional<SceneRaycastHit>> lower = PickSprite(scene, assets, request);
			REQUIRE(lower.has_value());
			REQUIRE(lower->has_value());
			CHECK((*lower)->Triangle == 1);
			// Outside the distance interval the quad is not hit.
			request.MaxDistance = 3.5f;
			const Result<std::optional<SceneRaycastHit>> tooShort = PickSprite(scene, assets, request);
			REQUIRE(tooShort.has_value());
			CHECK_FALSE(tooShort->has_value());

			const auto refused = [&scene, &assets](SpritePickRequest invalid)
			{
				const Result<std::optional<SceneRaycastHit>> result = PickSprite(scene, assets, invalid);
				return !result.has_value() && result.error().GetCode() == ErrorCode::InvalidArgument;
			};
			const float nan = std::numeric_limits<float>::quiet_NaN();
			CHECK(refused(SpritePickRequest{ .Ray = RenderRay{ .Origin = glm::vec3(nan, 0.0f, 0.0f) } }));
			CHECK(refused(SpritePickRequest{ .Ray = RenderRay{ .Direction = glm::vec3(0.0f) } }));
			CHECK(refused(SpritePickRequest{ .MaxDistance = 1.0f, .MinDistance = 2.0f }));
			CHECK(refused(SpritePickRequest{ .MinDistance = -1.0f }));
			CHECK(refused(SpritePickRequest{ .Alpha = 1.5f }));
		}

		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24, stream T1): a tile is picked as a sprite of its tilemap's
		// entity, in the one draw order of sprites and tilemaps, in the scene and in a snapshot's sprites and tilemaps alike.
		TEST_CASE("SpritePicking: a tile is picked as its tilemap's entity in the draw order" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			assets.Publish(PickTextureHandle, MakePickTexture(), "Assets/Textures/Pick.png");
			// A one-frame sheet of the 16-texel texture: one unit at 16 pixels per unit, one tile per unit cell.
			const AssetHandle sheetHandle = UUID(8931);
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			sheet->Texture = TypedAssetHandle<AssetType::Texture>(PickTextureHandle);
			sheet->Frames.push_back(SpriteFrame{ .Rect = { .Width = 16, .Height = 16 } });
			assets.Publish(sheetHandle, sheet, "Assets/Sprites/Pick.spritesheet");
			// Cells (0, 0) and (2, 0) hold the tile: the second tile covers x in [2, 3], y in [0, 1].
			const AssetHandle tilemapHandle = UUID(8932);
			Ref<TilemapData> tilemap = CreateRef<TilemapData>();
			tilemap->ChunkSize = 4;
			tilemap->Palette = { TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle) } };
			tilemap->Chunks = { TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } } };
			assets.Publish(tilemapHandle, tilemap, "Assets/Tilemaps/Pick.tilemap");

			Entity ground = scene.CreateEntity("Ground");
			ground.AddComponent<TilemapRendererComponent>(TilemapRendererComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(tilemapHandle), .OrderInLayer = 1 });
			// A sprite over the second tile, drawn before the tilemap (a lower order in the layer), and one drawn after it.
			const auto addSprite = [&scene](std::string_view name, glm::vec3 position, int32_t order)
			{
				Entity entity = scene.CreateEntity(name);
				entity.Patch<TransformComponent>([position](TransformComponent& transform)
				{
					transform.Translation = position;
				});
				entity.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Texture = TypedAssetHandle<AssetType::Texture>(PickTextureHandle),
					.OrderInLayer = order });
				return entity.GetUUID();
			};
			static_cast<void>(addSprite("Behind", glm::vec3(2.5f, 0.5f, 0.0f), 0));
			const UUID above = addSprite("Above", glm::vec3(0.5f, 0.5f, 0.0f), 2);
			TransformSystem::Update(scene);

			SpritePickRequest request;
			request.Ray = RenderRay{ .Origin = glm::vec3(2.4f, 0.3f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, -1.0f) };
			request.SortingLayers = { std::string(DefaultSortingLayerName) };
			// The tile, drawn after the sprite behind it, is what the pixel shows.
			const Result<std::optional<SceneRaycastHit>> tile = PickSprite(scene, assets, request);
			REQUIRE_MESSAGE(tile.has_value(), tile.error().ToString());
			REQUIRE(tile->has_value());
			CHECK((*tile)->Entity == ground.GetUUID());
			// Over the first tile, the sprite drawn after the tilemap wins; between the tiles, the empty cell (1, 0) picks nothing.
			request.Ray.Origin = glm::vec3(0.6f, 0.4f, 10.0f);
			const Result<std::optional<SceneRaycastHit>> sprite = PickSprite(scene, assets, request);
			REQUIRE(sprite.has_value());
			REQUIRE(sprite->has_value());
			CHECK((*sprite)->Entity == above);
			request.Ray.Origin = glm::vec3(1.5f, 0.5f, 10.0f);
			const Result<std::optional<SceneRaycastHit>> gap = PickSprite(scene, assets, request);
			REQUIRE(gap.has_value());
			CHECK_FALSE(gap->has_value());

			// The snapshot overload picks the same in the image's sprites and tilemaps.
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 64, .Height = 64, .SortingLayers = request.SortingLayers });
			REQUIRE(snapshot.has_value());
			request.Ray.Origin = glm::vec3(2.4f, 0.3f, 10.0f);
			const Result<std::optional<SceneRaycastHit>> fromImage = PickSprite(snapshot->Sprites, snapshot->Tilemaps, assets, request);
			REQUIRE_MESSAGE(fromImage.has_value(), fromImage.error().ToString());
			REQUIRE(fromImage->has_value());
			CHECK((*fromImage)->Entity == ground.GetUUID());
		}
	}

}
