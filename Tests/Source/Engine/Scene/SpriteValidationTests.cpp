#include "TestsPCH.h"

#include "Engine/Scene/SpriteValidation.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/ParticleLookComponent.h"
#include "Engine/Scene/Components/SpriteAnimatorComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <string>
#include <string_view>
#include <vector>

// The sprite validation codes (Docs/Decisions/0032-m21-contract.md decision 12): stream A's two (the sorting layer and the
// frame) and stream B's clip, each in a case of its own so each stream proves its checks alone; C21.2's stream T1 extends the
// sorting-layer check to TilemapRenderer (decision 30); M33 adds a ParticleLook's flipbook range
// (Docs/Decisions/0048-m33-particle-look.md decision 9).

namespace Engine {

	namespace {

		// A sheet of one 8 x 8 frame and one clip, "Idle", published in `assets`.
		TypedAssetHandle<AssetType::SpriteSheet> PublishHeroSheet(Test::InMemoryAssetManager& assets)
		{
			const AssetHandle sheetHandle(0x2b9f3a654a1c0e7d);
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			sheet->Frames.push_back(SpriteFrame{ .Rect = { .Width = 8, .Height = 8 } });
			sheet->Clips.push_back(SpriteClip{ .Name = "Idle", .Frames = { { .Frame = 0, .Ticks = 4 } } });
			assets.Publish(sheetHandle, sheet, "Assets/Sheets/Hero.spritesheet");
			return TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle);
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("SpriteValidation: an unknown sorting layer and an out-of-range frame are reported")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const TypedAssetHandle<AssetType::SpriteSheet> sheet = PublishHeroSheet(assets);

			Entity clean = scene.CreateEntity("Clean");
			clean.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet, .SortingLayer = "Background" });
			Entity layer = scene.CreateEntity("Layer");
			layer.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .SortingLayer = "Sky" });
			Entity frame = scene.CreateEntity("Frame");
			frame.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet, .Frame = 1 });

			const std::vector<std::string> layers = { "Default", "Background" };
			const std::vector<SpriteSceneIssue> issues = FindSpriteSceneIssues(scene, &assets, layers);
			REQUIRE(issues.size() == 2);
			CHECK(issues[0].Code == SpriteUnknownSortingLayerCode);
			CHECK(issues[0].Entity == layer.GetUUID());
			CHECK(issues[0].Component == "SpriteRenderer");
			CHECK(issues[0].Field == "SortingLayer");
			CHECK(issues[1].Code == SpriteFrameOutOfRangeCode);
			CHECK(issues[1].Entity == frame.GetUUID());
			CHECK(issues[1].Field == "Frame");
			for (const SpriteSceneIssue& issue : issues)
				CHECK(issue.Severity == DiagnosticSeverity::Warning);

			// Without an asset manager the sheets are not loaded, so only the sorting layers are checked; a disabled entity's
			// sprite is not checked at all.
			const std::vector<SpriteSceneIssue> layersOnly = FindSpriteSceneIssues(scene, nullptr, layers);
			REQUIRE(layersOnly.size() == 1);
			CHECK(layersOnly[0].Code == SpriteUnknownSortingLayerCode);
			layer.SetActive(false);
			frame.SetActive(false);
			CHECK(FindSpriteSceneIssues(scene, &assets, layers).empty());
		}

		TEST_CASE("SpriteValidation: an unknown clip is reported")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const TypedAssetHandle<AssetType::SpriteSheet> sheet = PublishHeroSheet(assets);

			Entity clean = scene.CreateEntity("Clean");
			clean.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet });
			clean.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Idle" });
			Entity idle = scene.CreateEntity("Idle");
			idle.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet });
			idle.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{});
			Entity clip = scene.CreateEntity("Clip");
			clip.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet });
			clip.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Run" });

			const std::vector<std::string> layers = { "Default" };
			const std::vector<SpriteSceneIssue> issues = FindSpriteSceneIssues(scene, &assets, layers);
			REQUIRE(issues.size() == 1);
			CHECK(issues[0].Code == SpriteUnknownClipCode);
			CHECK(issues[0].Entity == clip.GetUUID());
			CHECK(issues[0].Component == "SpriteAnimator");
			CHECK(issues[0].Field == "Clip");
			CHECK(issues[0].Severity == DiagnosticSeverity::Warning);
		}

		// Decision 12's SPRITE_UNKNOWN_CLIP covers a clip with no sheet to look it up in; a sheet that does not load is the
		// asset checks' finding (ASSET_MISSING and the like), and an effectively disabled entity and a check without an asset
		// manager report nothing.
		TEST_CASE("SpriteValidation: a clip without a sheet is reported and a sheet that does not load is left to the asset checks")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const TypedAssetHandle<AssetType::SpriteSheet> sheet = PublishHeroSheet(assets);

			Entity sheetless = scene.CreateEntity("Sheetless");
			sheetless.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{});
			sheetless.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Idle", .Playing = false });
			Entity missing = scene.CreateEntity("Missing");
			missing.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(AssetHandle(0x2b9f3a654a1c0e7e)) });
			missing.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Idle" });
			Entity parent = scene.CreateEntity("Disabled");
			Entity hidden = scene.CreateEntity("Hidden", parent);
			hidden.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet });
			hidden.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Run" });
			parent.SetActive(false);

			const std::vector<std::string> layers = { "Default" };
			const std::vector<SpriteSceneIssue> issues = FindSpriteSceneIssues(scene, &assets, layers);
			REQUIRE(issues.size() == 1);
			CHECK(issues[0].Code == SpriteUnknownClipCode);
			CHECK(issues[0].Entity == sheetless.GetUUID());
			CHECK(issues[0].Field == "Clip");
			CHECK(FindSpriteSceneIssues(scene, nullptr, layers).empty());
		}

		// C21.2 (decision 30, stream T1): a TilemapRenderer's SortingLayer sorts with the sprites, so an undeclared one draws in
		// the first layer and is reported with the sprites' code, naming the TilemapRenderer; the check needs no asset manager.
		TEST_CASE("SpriteValidation: a TilemapRenderer's unknown sorting layer is reported")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity known = scene.CreateEntity("Known");
			known.AddComponent<TilemapRendererComponent>(TilemapRendererComponent{ .SortingLayer = "Background" });
			Entity typo = scene.CreateEntity("Typo");
			typo.AddComponent<TilemapRendererComponent>(TilemapRendererComponent{ .SortingLayer = "Backgroud" });

			const std::vector<std::string> layers = { "Default", "Background" };
			const std::vector<SpriteSceneIssue> issues = FindSpriteSceneIssues(scene, nullptr, layers);
			REQUIRE(issues.size() == 1);
			CHECK(issues[0].Code == SpriteUnknownSortingLayerCode);
			CHECK(issues[0].Severity == DiagnosticSeverity::Warning);
			CHECK(issues[0].Entity == typo.GetUUID());
			CHECK(issues[0].Component == "TilemapRenderer");
			CHECK(issues[0].Field == "SortingLayer");
			// A disabled entity's tilemap is not checked.
			typo.SetActive(false);
			CHECK(FindSpriteSceneIssues(scene, nullptr, layers).empty());
		}

		// M33 (decision 9): a flipbook range must start at one of the sheet's frames and end within them, FrameCount 0 running
		// to its last; the finding is located at FirstFrame when the range starts past the sheet, else at FrameCount.
		TEST_CASE("SpriteValidation: a ParticleLook whose flipbook range starts or ends past its sheet's frames is PARTICLE_FRAME_OUT_OF_RANGE")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const TypedAssetHandle<AssetType::SpriteSheet> sheet = PublishHeroSheet(assets); // one frame
			const auto addLook = [&scene](std::string_view name, ParticleLookComponent look)
			{
				Entity entity = scene.CreateEntity(name);
				entity.AddComponent<ParticleEmitterComponent>();
				entity.AddComponent<ParticleLookComponent>(look);
				return entity;
			};
			static_cast<void>(addLook("Whole", ParticleLookComponent{ .SpriteSheet = sheet }));
			static_cast<void>(addLook("One", ParticleLookComponent{ .SpriteSheet = sheet, .FrameCount = 1 }));
			static_cast<void>(addLook("NoSheet", ParticleLookComponent{ .FirstFrame = 9 }));
			const Entity past = addLook("Past", ParticleLookComponent{ .SpriteSheet = sheet, .FirstFrame = 1 });
			const Entity overlong = addLook("Long", ParticleLookComponent{ .SpriteSheet = sheet, .FrameCount = 2 });

			const std::vector<std::string> layers = { "Default" };
			const std::vector<SpriteSceneIssue> issues = FindSpriteSceneIssues(scene, &assets, layers);
			REQUIRE(issues.size() == 2);
			CHECK(issues[0].Code == ParticleFrameOutOfRangeCode);
			CHECK(issues[0].Entity == past.GetUUID());
			CHECK(issues[0].Component == "ParticleLook");
			CHECK(issues[0].Field == "FirstFrame");
			CHECK(issues[1].Code == ParticleFrameOutOfRangeCode);
			CHECK(issues[1].Entity == overlong.GetUUID());
			CHECK(issues[1].Field == "FrameCount");
			for (const SpriteSceneIssue& issue : issues)
				CHECK(issue.Severity == DiagnosticSeverity::Warning);
			// Without an asset manager no sheet loads, so nothing is checked.
			CHECK(FindSpriteSceneIssues(scene, nullptr, layers).empty());
		}
	}

}
