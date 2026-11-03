#include "TestsPCH.h"

#include "Engine/Scene/SpritePicking.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
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
	}

}
