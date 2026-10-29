#include "TestsPCH.h"

#include "Engine/Scene/SpriteValidation.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Scene/Components/SpriteAnimatorComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <string>
#include <vector>

// The sprite validation codes (Docs/Decisions/0032-m21-contract.md decision 12): stream A's two (the sorting layer and the
// frame) and stream B's clip, each in a case of its own so each stream proves its checks alone. Skipped until they land.

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
		TEST_CASE("SpriteValidation: an unknown sorting layer and an out-of-range frame are reported" * doctest::skip(true))
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
		}

		TEST_CASE("SpriteValidation: an unknown clip is reported" * doctest::skip(true))
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
	}

}
