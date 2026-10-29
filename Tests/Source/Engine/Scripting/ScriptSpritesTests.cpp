#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Core/Time.h"
#include "Engine/Scene/Components/SpriteAnimatorComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SpriteAnimationSystem.h"
#include "Support/ScriptTestFixture.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The sprite script surface (Docs/Decisions/0032-m21-contract.md decision 13, stream B): SpriteRenderer:SetFrameByName, and
// SpriteAnimator:Play and Stop with the SpriteAnimation phase that plays what they set (decision 5). Skipped until stream B
// binds them.

namespace Engine {

	namespace {

		void CheckSpriteScript(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const Result<ScriptEvaluation> result = fixture.Evaluate(source);
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptSprites: SetFrameByName sets the named frame, and Play and Stop drive the SpriteAnimation phase" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			const AssetHandle sheetHandle(0x5a1d0c77e3b14f21);
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			for (const std::string_view name : { "Idle", "Run0", "Run1" })
				sheet->Frames.push_back(SpriteFrame{ .Name = std::string(name), .Rect = { .Width = 8, .Height = 8 } });
			sheet->Clips.push_back(SpriteClip{ .Name = "Run", .Frames = { { .Frame = 1, .Ticks = 2 }, { .Frame = 2, .Ticks = 1 } }, .Loop = true });
			fixture.GetAssetManager().Publish(sheetHandle, sheet, "Assets/Sheets/Hero.spritesheet");
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(sheetHandle) });
			// A stopped animator left in the middle of another clip: Play must reset its step.
			hero.AddComponent<SpriteAnimatorComponent>(SpriteAnimatorComponent{ .Clip = "Jump", .Playing = false, .Step = 4, .StepTicks = 9 });
			REQUIRE(fixture.Start());

			// SetFrameByName sets Frame to the named frame's index; a name the sheet lacks raises an error and changes nothing.
			// Play sets the clip and resets the playback state; the frame changes only in the SpriteAnimation phase.
			CheckSpriteScript(fixture, R"(
local hero = Scene.FindByName("Hero")
hero.SpriteRenderer:SetFrameByName("Run1")
assert(hero.SpriteRenderer.Frame == 2)
assert(not pcall(function() hero.SpriteRenderer:SetFrameByName("Jump") end))
assert(hero.SpriteRenderer.Frame == 2)
hero.SpriteAnimator:Play("Run")
local animator = hero.SpriteAnimator
assert(animator.Clip == "Run" and animator.Playing and animator.Step == 0 and animator.StepTicks == 0)
assert(hero.SpriteRenderer.Frame == 2)
return true
)");

			// The clip's first step shows at the next SpriteAnimation phase, for its two ticks, then the second step.
			Result<Scope<SpriteAnimationSystem>> system = SpriteAnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &fixture.GetAssetManager() });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			std::vector<uint32_t> frames;
			for (uint64_t tick = 0; tick < 3; ++tick)
			{
				(*system)->Step(SimStep::FromTick(tick, 1.0 / 60.0));
				frames.push_back(hero.GetComponent<SpriteRendererComponent>().Frame);
			}
			CHECK(frames == std::vector<uint32_t>{ 1, 1, 2 });

			// Stop keeps the frame shown, and the phase no longer advances it.
			CheckSpriteScript(fixture, R"(
local animator = Scene.FindByName("Hero").SpriteAnimator
animator:Stop()
assert(not animator.Playing)
return true
)");
			(*system)->Step(SimStep::FromTick(3, 1.0 / 60.0));
			CHECK(hero.GetComponent<SpriteRendererComponent>().Frame == 2);
			CHECK(fixture.Errors.empty());
		}
	}

}
