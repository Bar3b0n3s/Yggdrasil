#include "TestsPCH.h"

#include "Engine/Scene/SpriteAnimationSystem.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Scene/Components/SpriteAnimatorComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Sprite animation driven by ticks (Docs/Decisions/0032-m21-contract.md decision 5, stream B; Roadmap M21 "a tick-driven
// SpriteAnimator playing SpriteSheet clips").

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
		TEST_CASE("SpriteAnimation: a clip shows each step for its ticks, then loops or holds its last frame")
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

		TEST_CASE("SpriteAnimation: the phase advances playing animators in canonical order and writes their frames")
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

		// Decision 5's rule for what is missing: an unknown clip, a sheet that does not load and a renderer without a sheet each
		// change nothing and raise SPRITE_UNKNOWN_CLIP once per entity and clip name (logged at Warn), however many ticks pass;
		// a stopped animator, an empty Clip and an effectively disabled entity are not looked at.
		TEST_CASE("SpriteAnimation: a clip the phase cannot play changes nothing and is reported once per entity and clip")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle sheetHandle(0x5a1d0c77e3b14f24);
			assets.Publish(sheetHandle, MakeAnimatedSheet(), "Assets/Sheets/Hero.spritesheet");
			const TypedAssetHandle<AssetType::SpriteSheet> sheet(sheetHandle);

			Entity run = scene.CreateEntity("Run");
			run.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet, .Frame = 1 });
			run.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Run" });
			Entity missing = scene.CreateEntity("Missing");
			missing.AddComponent<SpriteRendererComponent>(
				SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(AssetHandle(0x5a1d0c77e3b14f25)), .Frame = 2 });
			missing.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Walk" });
			Entity sheetless = scene.CreateEntity("Sheetless");
			sheetless.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Frame = 3 });
			sheetless.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Walk" });
			Entity stopped = scene.CreateEntity("Stopped");
			stopped.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet, .Frame = 3 });
			stopped.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Jump", .Playing = false });
			Entity idle = scene.CreateEntity("Idle");
			idle.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet, .Frame = 3 });
			idle.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{});
			Entity parent = scene.CreateEntity("Disabled");
			Entity hidden = scene.CreateEntity("Hidden", parent);
			hidden.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = sheet, .Frame = 3 });
			hidden.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Walk" });
			parent.SetActive(false);

			Result<Scope<SpriteAnimationSystem>> system = SpriteAnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			{
				Test::ExpectLog warned(LogLevel::Warn, std::string(SpriteUnknownClipCode));
				for (uint64_t tick = 0; tick < 4; ++tick)
					(*system)->Step(SimStep::FromTick(tick, 1.0 / 60.0));
				CHECK(warned.GetMatchCount() == 3);
			}
			for (const Entity entity : { run, missing, sheetless, stopped, idle, hidden })
			{
				CAPTURE(entity.GetName());
				const SpriteAnimatorComponent& animator = entity.GetComponent<SpriteAnimatorComponent>();
				CHECK(animator.Step == 0);
				CHECK(animator.StepTicks == 0);
			}
			CHECK(run.GetComponent<SpriteRendererComponent>().Frame == 1);
			CHECK(missing.GetComponent<SpriteRendererComponent>().Frame == 2);
			CHECK(sheetless.GetComponent<SpriteRendererComponent>().Frame == 3);
			CHECK(hidden.GetComponent<SpriteRendererComponent>().Frame == 3);
			const std::span<const SpriteSceneIssue> issues = (*system)->GetDiagnostics();
			REQUIRE(issues.size() == 3);
			CHECK(issues[0].Entity == run.GetUUID());
			CHECK(issues[1].Entity == missing.GetUUID());
			CHECK(issues[2].Entity == sheetless.GetUUID());
			for (const SpriteSceneIssue& issue : issues)
			{
				CHECK(issue.Code == SpriteUnknownClipCode);
				CHECK(issue.Severity == DiagnosticSeverity::Warning);
				CHECK(issue.Component == "SpriteAnimator");
				CHECK(issue.Field == "Clip");
			}

			// Another clip name of a reported entity is a finding of its own.
			run.Patch<SpriteAnimatorComponent>([](SpriteAnimatorComponent& animator)
			{
				animator.Clip = "Fly";
			});
			(*system)->Step(SimStep::FromTick(4, 1.0 / 60.0));
			REQUIRE((*system)->GetDiagnostics().size() == 4);
			CHECK((*system)->GetDiagnostics()[3].Entity == run.GetUUID());

			// The system needs its scene and an asset manager.
			CHECK(SpriteAnimationSystem::Create({ .RuntimeScene = &scene, .Assets = nullptr }).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(SpriteAnimationSystem::Create({ .RuntimeScene = nullptr, .Assets = &assets }).error().GetCode() == ErrorCode::InvalidArgument);
		}

		// A script can write a new clip name every tick, so the findings kept are bounded: past the limit one warning says so,
		// and later findings are neither kept nor logged.
		TEST_CASE("SpriteAnimation: the unknown-clip findings kept are bounded")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle sheetHandle(0x5a1d0c77e3b14f26);
			assets.Publish(sheetHandle, MakeAnimatedSheet(), "Assets/Sheets/Hero.spritesheet");
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle) });
			hero.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{});
			Result<Scope<SpriteAnimationSystem>> system = SpriteAnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());

			Test::ExpectLog limit(LogLevel::Warn, "neither kept nor logged");
			const uint64_t names = MaxSpriteAnimationDiagnostics + 16;
			for (uint64_t tick = 0; tick < names; ++tick)
			{
				hero.Patch<SpriteAnimatorComponent>([tick](SpriteAnimatorComponent& animator)
				{
					animator.Clip = std::format("Missing{}", tick);
				});
				(*system)->Step(SimStep::FromTick(tick, 1.0 / 60.0));
			}
			CHECK((*system)->GetDiagnostics().size() == MaxSpriteAnimationDiagnostics);
			CHECK(limit.GetMatchCount() == 1);
		}

		// Decision 10: the session creates the system with its asset manager and enters the SpriteAnimation phase every tick,
		// in Play and Simulate alike; a Scene.Load replaces the system with one over the loaded scene.
		TEST_CASE("SpriteAnimation: a play session advances its animators in Play and Simulate and in the scene a Scene.Load starts")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			const AssetHandle sheetHandle(0x5a1d0c77e3b14f22);
			assets.Publish(sheetHandle, MakeAnimatedSheet(), "Assets/Sheets/Hero.spritesheet");
			Entity walker = fixture.GetScene().CreateEntity("Walker");
			walker.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle) });
			walker.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Walk" });
			Result<Json> document = SceneSerializer::ToJson(fixture.GetScene());
			REQUIRE(document.has_value());
			Ref<SceneData> walkScene = CreateRef<SceneData>();
			walkScene->Document = CreateRef<const Json>(std::move(*document));
			assets.Publish(AssetHandle(0x5a1d0c77e3b14f23), walkScene, "Assets/Scenes/Walk.scene");

			const auto start = [&fixture, &assets](PlayMode mode)
			{
				PlaySessionSpecification specification;
				specification.Registry = &fixture.GetRegistry();
				specification.Assets = &assets;
				specification.Seed = 42;
				specification.Mode = mode;
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				(*session)->SetExtractionEnabled(false);
				return std::move(*session);
			};
			const auto tickFrames = [](PlaySession& session, int ticks)
			{
				std::vector<uint32_t> frames;
				for (int tick = 0; tick < ticks; ++tick)
				{
					session.Tick();
					frames.push_back(session.GetScene().FindEntityByPath("/Walker").GetComponent<SpriteRendererComponent>().Frame);
				}
				return frames;
			};
			// Walk: frame 0 for 2 ticks, frame 1 for 1, frame 2 for 3, then again.
			const std::vector<uint32_t> walk = { 0, 0, 1, 2, 2, 2, 0 };
			CHECK(tickFrames(*start(PlayMode::Simulate), 7) == walk);
			const Scope<PlaySession> play = start(PlayMode::Play);
			CHECK(tickFrames(*play, 3) == std::vector<uint32_t>(walk.begin(), walk.begin() + 3));
			// The scene a Scene.Load starts animates from its own state, from the tick after the load: the load's tick still
			// advanced the replaced scene.
			const Result<ScriptEvaluation> load = play->GetScripts()->Evaluate(R"(Scene.Load(Assets.Load("Assets/Scenes/Walk.scene")))", {});
			REQUIRE_MESSAGE(load.has_value(), (load ? std::string() : load.error().ToString()));
			CHECK(tickFrames(*play, 1) == std::vector<uint32_t>{ 0 });
			CHECK(play->GetSceneGeneration() == 2);
			CHECK(tickFrames(*play, 3) == std::vector<uint32_t>(walk.begin(), walk.begin() + 3));
			CHECK(play->GetScriptErrors().GetErrors().empty());
			// The edit scene is only the sessions' source.
			CHECK(walker.GetComponent<SpriteRendererComponent>().Frame == 0);
			CHECK(walker.GetComponent<SpriteAnimatorComponent>().Step == 0);
		}
	}

}
