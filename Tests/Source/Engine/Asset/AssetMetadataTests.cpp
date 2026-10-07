#include "TestsPCH.h"

#include "Engine/Asset/AssetMetadata.h"

#include "Engine/Core/Hash.h"
#include "Support/AssetTestFixture.h"

namespace Engine {

	namespace {

		// The §6.4 example with every member canonical.
		constexpr std::string_view GltfMetaText = R"({
	"Format": "AssetMeta",
	"Version": 1,
	"Handle": "3c9f2e7a11d04b88",
	"Type": "Prefab",
	"Importer": "Gltf",
	"ImporterVersion": 1,
	"Settings": {
		"Scale": 1,
		"GenerateMissingTangents": true,
		"ImportMaterials": true,
		"MergeMeshes": false
	},
	"SubAssets": [
		{
			"Key": "material:0:TrackPBR",
			"Handle": "%MATERIAL%",
			"Type": "Material"
		},
		{
			"Key": "mesh:0:Straight",
			"Handle": "%MESH%",
			"Type": "Mesh"
		}
	]
}
)";

		constexpr std::string_view DependencyMetaText = R"({
	"Format": "AssetMeta",
	"Version": 1,
	"Handle": "51aa0c7e00f1b2c3",
	"Type": "Dependency",
	"Owner": "3c9f2e7a11d04b88"
}
)";

		std::string MakeGltfMetaText()
		{
			const AssetHandle source(0x3c9f2e7a11d04b88ull);
			std::string text(GltfMetaText);
			const auto replace = [&text](std::string_view token, AssetHandle handle)
			{
				text.replace(text.find(token), token.size(), handle.ToString());
			};
			replace("%MATERIAL%", DeriveSubAssetHandle(source, "material:0:TrackPBR"));
			replace("%MESH%", DeriveSubAssetHandle(source, "mesh:0:Straight"));
			return text;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetMetadata: sub-asset handles are Hash64 of the source handle and key")
		{
			const AssetHandle source(0x3c9f2e7a11d04b88ull);
			CHECK(DeriveSubAssetHandle(source, "mesh:0:Straight").GetValue() == Hash64(source.GetValue(), std::string_view("mesh:0:Straight")));
			CHECK(DeriveSubAssetHandle(source, "mesh:0:Straight") != DeriveSubAssetHandle(source, "mesh:1:Straight"));
			const AssetMetadata dependency = MakeDependencyMetadata(AssetHandle(7), source);
			CHECK(dependency.Kind == AssetMetaKind::Dependency);
			CHECK(dependency.Owner == source);
			CHECK(dependency.Type == AssetType::None);
		}

		TEST_CASE("AssetMetadata: asset and dependency metas round-trip byte-identically")
		{
			const std::string gltfText = MakeGltfMetaText();
			Result<AssetMetadata> gltf = ParseAssetMetadata(gltfText, "Assets/Track.glb.meta");
			REQUIRE_MESSAGE(gltf.has_value(), gltf.error().ToString());
			CHECK(gltf->Kind == AssetMetaKind::Asset);
			CHECK(gltf->Type == AssetType::Prefab);
			CHECK(gltf->Importer == "Gltf");
			REQUIRE(gltf->SubAssets.size() == 2);
			REQUIRE(gltf->FindSubAsset("mesh:0:Straight") != nullptr);
			CHECK(gltf->FindSubAsset("mesh:0:Straight")->Type == AssetType::Mesh);
			CHECK(gltf->FindSubAsset("texture:9") == nullptr);
			CHECK(SerializeAssetMetadata(*gltf) == gltfText);

			Result<AssetMetadata> dependency = ParseAssetMetadata(DependencyMetaText);
			REQUIRE_MESSAGE(dependency.has_value(), dependency.error().ToString());
			CHECK(dependency->Kind == AssetMetaKind::Dependency);
			CHECK(dependency->Owner == AssetHandle(0x3c9f2e7a11d04b88ull));
			CHECK(SerializeAssetMetadata(*dependency) == DependencyMetaText);
		}

		TEST_CASE("AssetMetadata: malformed metas are located Validation errors")
		{
			struct Case
			{
				std::string_view Text;
				std::string_view Pointer;
			};
			const Case cases[] = {
				{ R"({"Format": "AssetMeta", "Version": 1, "Type": "Texture", "Importer": "Texture", "ImporterVersion": 1, "Settings": {}, "SubAssets": []})", "" },
				{ R"({"Format": "AssetMeta", "Version": 1, "Handle": "0000000000000001", "Type": "Sound", "Importer": "Texture", "ImporterVersion": 1, "Settings": {}, "SubAssets": []})", "/Type" },
				{ R"({"Format": "AssetMeta", "Version": 1, "Handle": "0000000000000001", "Type": "Dependency"})", "" },
				{ R"({"Format": "AssetMeta", "Version": 1, "Handle": "0000000000000001", "Type": "Texture", "Importer": "Texture", "ImporterVersion": 1, "Settings": {}, "SubAssets": [], "Extra": 1})", "/Extra" },
			};
			for (const Case& testCase : cases)
			{
				CAPTURE(std::string(testCase.Text));
				Result<AssetMetadata> parsed = ParseAssetMetadata(testCase.Text, "Assets/Bad.png.meta");
				REQUIRE_FALSE(parsed.has_value());
				CHECK(parsed.error().GetCode() == ErrorCode::Validation);
				REQUIRE(parsed.error().GetLocation().JsonPointer.has_value());
				CHECK(*parsed.error().GetLocation().JsonPointer == testCase.Pointer);
			}
		}

		TEST_CASE("AssetMetadata: sub-asset entries must be sorted, unique and derived from the source handle")
		{
			const AssetHandle source(0x0000000000000001ull);
			const auto makeText = [](std::string_view subAssets)
			{
				return std::format(R"({{"Format": "AssetMeta", "Version": 1, "Handle": "0000000000000001", "Type": "Prefab", "Importer": "Gltf",
					"ImporterVersion": 1, "Settings": {{}}, "SubAssets": [{}]}})",
					subAssets);
			};
			const auto entry = [](std::string_view key, AssetHandle handle)
			{
				return std::format(R"({{"Key": "{}", "Handle": "{}", "Type": "Mesh"}})", key, handle.ToString());
			};
			const AssetHandle a = DeriveSubAssetHandle(source, "mesh:0");
			const AssetHandle b = DeriveSubAssetHandle(source, "mesh:1");
			struct Case
			{
				std::string Text;
				std::string_view Pointer;
			};
			const Case cases[] = {
				{ makeText(entry("mesh:1", b) + ", " + entry("mesh:0", a)), "/SubAssets/1/Key" },
				{ makeText(entry("mesh:0", a) + ", " + entry("mesh:0", a)), "/SubAssets/1/Key" },
				{ makeText(entry("mesh:0", b)), "/SubAssets/0/Handle" },
				{ makeText(entry("", a)), "/SubAssets/0/Key" },
				{ makeText(R"({"Key": "mesh:0", "Handle": ")" + a.ToString() + R"(", "Type": "None"})"), "/SubAssets/0/Type" },
				{ R"({"Format": "AssetMeta", "Version": 1, "Handle": "0000000000000001", "Type": "Dependency", "Owner": "0000000000000002", "Importer": "Gltf"})",
					"/Importer" },
				{ R"({"Format": "AssetMeta", "Version": 1, "Handle": "0000000000000001", "Type": "Texture", "Importer": "Texture", "ImporterVersion": 1, "Settings": {}, "SubAssets": [], "Owner": "0000000000000002"})",
					"/Owner" },
				{ R"({"Format": "AssetMeta", "Version": 1, "Handle": "0000000000000000", "Type": "Texture", "Importer": "Texture", "ImporterVersion": 1, "Settings": {}, "SubAssets": []})",
					"/Handle" },
			};
			for (const Case& testCase : cases)
			{
				CAPTURE(testCase.Text);
				Result<AssetMetadata> parsed = ParseAssetMetadata(testCase.Text, "Assets/Bad.glb.meta");
				REQUIRE_FALSE(parsed.has_value());
				CHECK(parsed.error().GetCode() == ErrorCode::Validation);
				CHECK(parsed.error().GetLocation().File == "Assets/Bad.glb.meta");
				REQUIRE(parsed.error().GetLocation().JsonPointer.has_value());
				CHECK(*parsed.error().GetLocation().JsonPointer == testCase.Pointer);
			}
		}

		TEST_CASE("AssetMetadata: a newer version is UnsupportedVersion")
		{
			Result<AssetMetadata> parsed = ParseAssetMetadata(R"({"Format": "AssetMeta", "Version": 2, "Handle": "0000000000000001"})");
			REQUIRE_FALSE(parsed.has_value());
			CHECK(parsed.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("AssetMetadata: meta paths are sibling sidecars")
		{
			const VfsPath source = Test::ParseVfsPath("project://Assets/Models/Track.glb");
			Result<VfsPath> meta = GetMetaPath(source);
			REQUIRE(meta.has_value());
			CHECK(meta->ToString() == "project://Assets/Models/Track.glb.meta");
			Result<VfsPath> back = GetSourcePathOfMeta(*meta);
			REQUIRE(back.has_value());
			CHECK(*back == source);
			CHECK_FALSE(GetMetaPath(*meta).has_value());
			CHECK_FALSE(GetSourcePathOfMeta(source).has_value());
		}
	}

}
