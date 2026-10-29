#include "TestsPCH.h"

#include "Engine/Scene/SpriteAnimationSystem.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Scene/Components/SpriteAnimatorComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <vector>

// Sprite animation driven by ticks (Docs/Decisions/0032-m21-contract.md decision 5, stream B; Roadmap M21 "a tick-driven
// SpriteAnimator playing SpriteSheet clips"): skipped until stream B implements it.

namespace Engine {

	namespace {

		// A sheet of four frames with a looping clip "Walk" (frames 0, 1, 2 for 2, 1 and 3 ticks) and a held clip "Die"
		// (frames 3, 2 for 1 and 2 ticks).
		Ref<SpriteSheetData> MakeAnimatedSheet()
		{
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			for (uint32_t index = 0; index < 4; ++index)
				sheet->Frames.push_back(SpriteFrame{ .Rect = { .X = index * 8, .Y = 0, .Width = 8, .Height = 8 } });
			sheet->Clips.push_back(SpriteClip{ .Name = "Walk", .Frames = { { .Frame = 0, .Ticks = 2 }, { .Frame = 1, .Ticks = 1 }, { .Frame = 2, .Ticks = 3 } }, .Loop = true });
			sheet->Clips.push_back(SpriteClip{ .Name = "Die", .Frames = { { .Frame = 3, .Ticks = 1 }, { .Frame = 2, .Ticks = 2 } }, .Loop = false });
			return sheet;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("SpriteAnimation: a clip shows each step for its ticks, then loops or holds its last frame" * doctest::skip(true))
		{
			const Ref<SpriteSheetData> sheet = MakeAnimatedSheet();
			SpriteAnimatorComponent walk{ .Clip = "Walk" };
			std::vector<uint32_t> frames;
			for (int tick = 0; tick < 8; ++tick)
				frames.push_back(AdvanceSpriteAnimator(walk, sheet->Clips[0]));
			CHECK(frames == std::vector<uint32_t>{ 0, 0, 1, 2, 2, 2, 0, 0 });
			CHECK(walk.Playing);

			SpriteAnimatorComponent die{ .Clip = "Die" };
			frames.clear();
			for (int tick = 0; tick < 5; ++tick)
				frames.push_back(AdvanceSpriteAnimator(die, sheet->Clips[1]));
			CHECK(frames.front() == 3);
			CHECK(frames[1] == 2);
			CHECK(frames[2] == 2);
			// Past its last step a clip without Loop stops on its last frame.
			CHECK_FALSE(die.Playing);
			CHECK(die.Step == 1);

			// A playback state left by another clip (Clip written through the field, not SpriteAnimator:Play): a StepTicks past
			// the step's Ticks shows that step for one tick, then moves on, and a Step past the clip's last step is clamped.
			SpriteAnimatorComponent stale{ .Clip = "Walk", .Step = 0, .StepTicks = 5 };
			frames.clear();
			for (int tick = 0; tick < 3; ++tick)
				frames.push_back(AdvanceSpriteAnimator(stale, sheet->Clips[0]));
			CHECK(frames == std::vector<uint32_t>{ 0, 1, 2 });
			CHECK(stale.Step == 2);
			CHECK(stale.StepTicks == 1);
			SpriteAnimatorComponent past{ .Clip = "Walk", .Step = 7 };
			frames.clear();
			for (int tick = 0; tick < 4; ++tick)
				frames.push_back(AdvanceSpriteAnimator(past, sheet->Clips[0]));
			CHECK(frames == std::vector<uint32_t>{ 2, 2, 2, 0 });
		}

		TEST_CASE("SpriteAnimation: the phase advances playing animators in canonical order and writes their frames" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle sheetHandle(0x5a1d0c77e3b14f20);
			assets.Publish(sheetHandle, MakeAnimatedSheet(), "Assets/Sheets/Hero.spritesheet");

			Entity walker = scene.CreateEntity("Walker");
			walker.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle) });
			walker.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Walk" });
			Entity paused = scene.CreateEntity("Paused");
			paused.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle), .Frame = 3 });
			paused.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Walk", .Playing = false });
			Entity unknown = scene.CreateEntity("Unknown");
			unknown.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle), .Frame = 1 });
			unknown.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Run" });

			Result<Scope<SpriteAnimationSystem>> system = SpriteAnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			for (uint64_t tick = 0; tick < 3; ++tick)
				(*system)->Step(SimStep::FromTick(tick, 1.0 / 60.0));
			// Three ticks of Walk: frame 0 twice, then frame 1.
			CHECK(walker.GetComponent<SpriteRendererComponent>().Frame == 1);
			CHECK(walker.GetComponent<SpriteAnimatorComponent>().Step == 2);
			CHECK(paused.GetComponent<SpriteRendererComponent>().Frame == 3);
			CHECK(unknown.GetComponent<SpriteRendererComponent>().Frame == 1);
			REQUIRE((*system)->GetDiagnostics().size() == 1);
			CHECK((*system)->GetDiagnostics()[0].Code == SpriteUnknownClipCode);
			CHECK((*system)->GetDiagnostics()[0].Entity == unknown.GetUUID());
		}
	}

}
