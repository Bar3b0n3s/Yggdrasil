#include "TestsPCH.h"

#include "Engine/Asset/AssetReference.h"

#include <array>

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetReference: handles, project paths, sub-asset paths and engine paths parse")
		{
			Result<AssetReference> handle = ParseAssetReference("3C9F2E7A11D04B88");
			REQUIRE_MESSAGE(handle.has_value(), handle.error().ToString());
			CHECK(handle->Kind == AssetReferenceKind::Handle);
			CHECK(handle->Handle == AssetHandle(0x3c9f2e7a11d04b88ull));

			Result<AssetReference> path = ParseAssetReference("Assets/Materials/Red.material");
			REQUIRE(path.has_value());
			CHECK(path->Kind == AssetReferenceKind::ProjectPath);
			CHECK(path->Path.ToString() == "project://Assets/Materials/Red.material");
			CHECK(path->SubAssetKey.empty());

			Result<AssetReference> scheme = ParseAssetReference("project://Assets/Materials/Red.material");
			REQUIRE(scheme.has_value());
			CHECK(*scheme == *path);

			Result<AssetReference> subAsset = ParseAssetReference("Assets/Models/Track.glb#mesh:0:Straight");
			REQUIRE(subAsset.has_value());
			CHECK(subAsset->Path.ToString() == "project://Assets/Models/Track.glb");
			CHECK(subAsset->SubAssetKey == "mesh:0:Straight");

			Result<AssetReference> engine = ParseAssetReference("engine://Meshes/Cube");
			REQUIRE(engine.has_value());
			CHECK(engine->Kind == AssetReferenceKind::EnginePath);
			CHECK(engine->Path.ToString() == "engine://Meshes/Cube");
		}

		TEST_CASE("AssetReference: malformed references are rejected with a hint")
		{
			constexpr std::array<std::string_view, 9> Malformed = {
				"",
				"0000000000000000",
				"Textures/Wood.png",
				"user://Notes.txt",
				"Assets/../Secret.png",
				"Assets/Track.glb#",
				"Assets\\Track.glb",
				"/Assets/Track.glb",
				"3c9f2e7a11d04b8",
			};
			for (const std::string_view text : Malformed)
			{
				CAPTURE(std::string(text));
				Result<AssetReference> parsed = ParseAssetReference(text);
				REQUIRE_FALSE(parsed.has_value());
				CHECK(parsed.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK_FALSE(parsed.error().GetHint().empty());
			}
		}

		TEST_CASE("AssetReference: formatting round-trips")
		{
			constexpr std::array<std::string_view, 4> Texts = {
				"3c9f2e7a11d04b88",
				"Assets/Materials/Red.material",
				"Assets/Models/Track.glb#mesh:0:Straight",
				"engine://Meshes/Cube",
			};
			for (const std::string_view text : Texts)
			{
				CAPTURE(std::string(text));
				Result<AssetReference> parsed = ParseAssetReference(text);
				REQUIRE(parsed.has_value());
				CHECK(FormatAssetReference(*parsed) == text);
			}
		}
	}

}
