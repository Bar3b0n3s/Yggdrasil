#include "TestsPCH.h"

#include "EditorCore/Viewport/SpriteDrop.h"

#include <nlohmann/json.hpp>

#include <optional>

// Dropping sprites into the scene view (Docs/Decisions/0032-m21-contract.md decision 14, stream D): skipped until stream D
// implements it.

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("SpriteDrop: a dropped sheet or texture becomes an entity.create of a sprite on the plane z = 0" * doctest::skip(true))
		{
			const std::optional<glm::vec3> point = IntersectSpritePlane(RenderRay{ .Origin = glm::vec3(1.0f, 2.0f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, -1.0f) });
			REQUIRE(point.has_value());
			CHECK(*point == glm::vec3(1.0f, 2.0f, 0.0f));
			CHECK_FALSE(IntersectSpritePlane(RenderRay{ .Origin = glm::vec3(0.0f, 0.0f, 10.0f), .Direction = glm::vec3(1.0f, 0.0f, 0.0f) }).has_value());
			CHECK_FALSE(IntersectSpritePlane(RenderRay{ .Origin = glm::vec3(0.0f, 0.0f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, 1.0f) }).has_value());

			const AssetHandle sheet(0x5a1d0c77e3b14f20);
			const Result<Json> sheetParams = MakeSpriteDropParams({ .Asset = sheet, .Type = AssetType::SpriteSheet, .Position = *point, .Revision = 7 });
			REQUIRE_MESSAGE(sheetParams.has_value(), sheetParams.error().ToString());
			CHECK((*sheetParams)["name"] == Json("Sprite"));
			CHECK((*sheetParams)["target"] == Json("edit"));
			CHECK((*sheetParams)["ifRevision"] == Json(7));
			CHECK((*sheetParams)["components"]["Transform"]["Translation"] == Json::array({ 1.0, 2.0, 0.0 }));
			CHECK((*sheetParams)["components"]["SpriteRenderer"]["Sheet"] == Json(sheet.ToString()));

			const Result<Json> textureParams = MakeSpriteDropParams({ .Asset = sheet, .Type = AssetType::Texture, .PlayScene = true });
			REQUIRE(textureParams.has_value());
			CHECK((*textureParams)["target"] == Json("play"));
			CHECK((*textureParams)["components"]["SpriteRenderer"]["Texture"] == Json(sheet.ToString()));
			CHECK_FALSE(MakeSpriteDropParams({ .Asset = sheet, .Type = AssetType::Mesh }).has_value());
		}
	}

}
