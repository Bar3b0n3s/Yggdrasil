#include "TestsPCH.h"

#include "Engine/Scene/TerrainValidation.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

// The terrain checks of a scene (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 9, stream T-A):
// TERRAIN_NO_HEIGHTFIELD and TERRAIN_SPLAT_NOT_RAW over a project's assets.

namespace Engine {

	namespace {

		constexpr AssetHandle FlatHeightfield{ 0x7e44000000000001ull };
		constexpr AssetHandle RawSplat{ 0x7e44000000000002ull };
		constexpr AssetHandle ColorSplat{ 0x7e44000000000003ull };
		constexpr AssetHandle AutoSplat{ 0x7e44000000000004ull };

		// The .meta of a Texture imported with `settings` (a JSON object's members).
		std::string TextureMeta(AssetHandle handle, std::string_view settings)
		{
			return std::format(R"({{"Format": "AssetMeta", "Version": 1, "Handle": "{}", "Type": "Texture", "Importer": "Texture", "ImporterVersion": 2,
				"Settings": {{{}}}, "SubAssets": []}})",
				handle.ToString(), settings);
		}

		// A Terrain on `heightfield` with `splatMap` (null handles for none).
		TerrainComponent MakeTerrain(AssetHandle heightfield, AssetHandle splatMap)
		{
			TerrainComponent terrain;
			terrain.Heightfield = TypedAssetHandle<AssetType::Heightfield>(heightfield);
			terrain.SplatMap = TypedAssetHandle<AssetType::Texture>(splatMap);
			return terrain;
		}

	}

	TEST_SUITE("Scene")
	{
		// Decision 9: a Terrain whose heightfield is null or does not load as one draws nothing (TERRAIN_NO_HEIGHTFIELD), and a
		// splat map is a data texture, the Texture importer's Usage Linear and Compression None (TERRAIN_SPLAT_NOT_RAW
		// otherwise: Usage Color, or a .meta without Compression, which reads as Auto); a built-in texture and disabled
		// entities are not checked.
		TEST_CASE("TerrainValidation: a Terrain without a heightfield and a splat map that is not a raw data texture are reported")
		{
			Test::AssetTestFixture assets;
			assets.WriteProjectFile("Assets/Terrain/Flat.heightfield", WriteHeightfieldSource(MakeFlatHeightfield(3)));
			assets.WriteProjectText("Assets/Terrain/Flat.heightfield.meta",
				std::format(R"({{"Format": "AssetMeta", "Version": 1, "Handle": "{}", "Type": "Heightfield", "Importer": "Heightfield",
					"ImporterVersion": 1, "Settings": null, "SubAssets": []}})",
					FlatHeightfield.ToString()));
			const Result<Buffer> png = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Textures/Rgba.png"));
			REQUIRE(png.has_value());
			for (const std::string_view name : { "Raw", "Color", "Auto" })
				assets.WriteProjectFile(std::format("Assets/Terrain/{}.png", name), *png);
			assets.WriteProjectText("Assets/Terrain/Raw.png.meta", TextureMeta(RawSplat, R"("Usage": "Linear", "GenerateMips": true, "Compression": "None")"));
			assets.WriteProjectText("Assets/Terrain/Color.png.meta", TextureMeta(ColorSplat, R"("Usage": "Color", "GenerateMips": true, "Compression": "None")"));
			assets.WriteProjectText("Assets/Terrain/Auto.png.meta", TextureMeta(AutoSplat, R"("Usage": "Linear", "GenerateMips": true)"));
			static_cast<void>(assets.OpenProject(false));

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity raw = scene.CreateEntity("Raw");
			raw.AddComponent<TerrainComponent>(MakeTerrain(FlatHeightfield, RawSplat));
			Entity builtin = scene.CreateEntity("Builtin");
			builtin.AddComponent<TerrainComponent>(MakeTerrain(FlatHeightfield, BuiltinAssetHandles::WhiteTexture));
			Entity bare = scene.CreateEntity("Bare");
			bare.AddComponent<TerrainComponent>();
			Entity mistyped = scene.CreateEntity("Mistyped");
			mistyped.AddComponent<TerrainComponent>(MakeTerrain(RawSplat, ColorSplat));
			Entity automatic = scene.CreateEntity("Automatic");
			automatic.AddComponent<TerrainComponent>(MakeTerrain(FlatHeightfield, AutoSplat));
			Entity disabled = scene.CreateEntity("Disabled");
			disabled.AddComponent<TerrainComponent>(MakeTerrain(AssetHandle(), ColorSplat));
			disabled.SetActive(false);

			const std::vector<TerrainSceneIssue> issues = FindTerrainSceneIssues(scene, assets.GetManager());
			REQUIRE(issues.size() == 4);
			CHECK(issues[0].Code == TerrainNoHeightfieldCode);
			CHECK(issues[0].Entity == bare.GetUUID());
			CHECK(issues[0].Component == "Terrain");
			CHECK(issues[0].Field == "Heightfield");
			CHECK(issues[0].Severity == DiagnosticSeverity::Warning);
			CHECK(issues[1].Code == TerrainNoHeightfieldCode);
			CHECK(issues[1].Entity == mistyped.GetUUID());
			CHECK(issues[2].Code == TerrainSplatNotRawCode);
			CHECK(issues[2].Entity == mistyped.GetUUID());
			CHECK(issues[2].Field == "SplatMap");
			CHECK_FALSE(issues[2].Hint.empty());
			CHECK(issues[3].Code == TerrainSplatNotRawCode);
			CHECK(issues[3].Entity == automatic.GetUUID());

			// A scene without a Terrain has nothing to report.
			Test::SceneTestFixture empty;
			CHECK(FindTerrainSceneIssues(empty.GetScene(), assets.GetManager()).empty());
		}
	}

}
