#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Time.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The animation script surface (M23, C23.3, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 31, stream B): the
// Animator's methods, the joint query and OnAnimationEvent. Skipped until stream B binds them.

namespace Engine {

	namespace {

		const AssetHandle SkeletonHandle(0x5ce1e70000000011);
		const AssetHandle WalkHandle(0xa41c000000000011);

		void PublishRig(Test::InMemoryAssetManager& assets)
		{
			Ref<SkeletonData> skeleton = CreateRef<SkeletonData>();
			skeleton->Joints.push_back({ .Name = "Hips", .Translation = { 0.0f, 1.0f, 0.0f }, .InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)) });
			assets.Publish(SkeletonHandle, skeleton, "Assets/Models/Hero.gltf");
			Ref<AnimationClipData> walk = CreateRef<AnimationClipData>();
			walk->Duration = 1.0f;
			walk->Channels.push_back(AnimationChannel{ .Target = "Hips", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 2.0f } });
			walk->Events = { AnimationEvent{ .Name = "Step", .Time = 0.25f } };
			assets.Publish(WalkHandle, walk, "Assets/Models/Hero.gltf");
		}

		// The "Trace:" entities of `scene` in canonical order, without the prefix.
		std::vector<std::string> ReadTrace(Scene& scene)
		{
			std::vector<std::string> trace;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const Entity entity = scene.FindEntityByID(id);
				if (entity && entity.GetName().starts_with("Trace:"))
					trace.push_back(entity.GetName().substr(6));
			}
			return trace;
		}

		void CheckAnimationScript(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const Result<ScriptEvaluation> result = fixture.Evaluate(source);
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
		}

		// Records each event as a "Trace:<slot>:<name>:<layer>" entity, so the trace keeps the dispatch order.
		constexpr std::string_view EventTraceSource = R"(
local T = { Fields = { Slot = Field.String("") } }
function T:OnAnimationEvent(event)
	Scene.CreateEntity("Trace:" .. self.Slot .. ":" .. event.Name .. ":" .. tostring(event.Layer))
end
return Script.Define("EventTrace", T)
)";

	}

	TEST_SUITE("Scripting")
	{
		// Play(layer, clip, time?) fills a layer (1-based; the next index appends one), SetWeight sets its weight, and
		// GetJointTransform and GetRootMotion read the last AnimationSystem phase's pose; a layer beyond the next index, a weight
		// outside [0, 1] and an unknown joint raise or return nil as decision 31 states.
		TEST_CASE("ScriptAnimation: Play, SetWeight, GetJointTransform and GetRootMotion drive and read the Animator" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			PublishRig(fixture.GetAssetManager());
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle),
				.RootMotion = AnimatorRootMotion::Extract });
			Entity holder = scene.CreateEntity("Holder");
			holder.AddComponent<AnimatorComponent>(AnimatorComponent{ .Layers = { AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(WalkHandle) } } });
			REQUIRE(fixture.Start());

			CheckAnimationScript(fixture, R"(
local hero = Scene.FindByName("Hero")
local walk = Scene.FindByName("Holder").Animator.Layers[1].Clip
local animator = hero.Animator
animator:Play(1, walk)
assert(#animator.Layers == 1 and animator.Layers[1].Clip == walk and animator.Layers[1].Time == 0)
animator:Play(2, walk, 0.5)
animator:SetWeight(2, 0.25)
assert(#animator.Layers == 2 and animator.Layers[2].Time == 0.5 and animator.Layers[2].Weight == 0.25)
assert(not pcall(function() animator:Play(4, walk) end))
assert(not pcall(function() animator:SetWeight(1, 1.5) end))
assert(animator:GetJointTransform("Hips") == nil)
return true
)");

			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &fixture.GetAssetManager() });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			TransformSystem::Update(scene);
			(*system)->Step(SimStep::FromTick(0, 0.25));
			CheckAnimationScript(fixture, R"(
local animator = Scene.FindByName("Hero").Animator
local position, rotation = animator:GetJointTransform("Hips")
assert(position ~= nil and rotation ~= nil)
assert(math.abs(position.y - 1) < 1e-5)
assert(animator:GetJointTransform("Tail") == nil)
local moved, turned = animator:GetRootMotion()
assert(math.abs(moved.z - 0.5) < 1e-4, "the extracted delta of a quarter second's walk")
return true
)");
			CHECK(fixture.Errors.empty());
		}

		// OnAnimationEvent reaches every slot of the Animator's entity, in slot order, in the AnimationSystem phase of Play; a
		// Simulate session runs no scripts, so nothing is called.
		TEST_CASE("ScriptAnimation: OnAnimationEvent reaches every slot of the Animator's entity in slot order" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture;
			PublishRig(fixture.GetAssetManager());
			const AssetHandle script(0x2b0101);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/EventTrace.luau", std::string(EventTraceSource)));
			Scene& editScene = fixture.GetScene();
			Entity hero = editScene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle),
				.Layers = { AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(WalkHandle) } } });
			ScriptSlot first{};
			first.Script.SetHandle(script);
			first.Fields["Slot"] = VariantValue(Json("A"));
			ScriptSlot second = first;
			second.Fields["Slot"] = VariantValue(Json("B"));
			hero.AddComponent<ScriptComponent>(ScriptComponent{ { first, second } });

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 7;
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = RunModes::Editor;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			Scope<PlaySession> session = std::move(*started);
			session->SetExtractionEnabled(false);
			// 30 ticks of 1/60 s pass the event at 0.25 s once.
			Test::RunTicks(*session, 30);
			CHECK(session->GetScriptErrors().GetErrors().empty());
			CHECK(ReadTrace(session->GetScene()) == std::vector<std::string>{ "A:Step:1", "B:Step:1" });
		}

		// The session creates its AnimationSystem before its ScriptEngine and replaces both at a scene transition, so it sets
		// the system's listener wherever it sets the physics system's (PrepareScripts) and clears it before the engine is
		// destroyed (decision 29): events reach the scripts from the session's first ticks and again after Scene.Reload, whose
		// new engine receives them, and never the destroyed one (which a sanitized run would report as a use after free).
		TEST_CASE("ScriptAnimation: OnAnimationEvent reaches the session's scripts from its start and after Scene.Reload" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture;
			PublishRig(fixture.GetAssetManager());
			const AssetHandle script(0x2b0102);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/EventTrace.luau", std::string(EventTraceSource)));
			Scene& editScene = fixture.GetScene();
			Entity hero = editScene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle),
				.Layers = { AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(WalkHandle) } } });
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			slot.Fields["Slot"] = VariantValue(Json("A"));
			hero.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 7;
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = RunModes::Editor;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			Scope<PlaySession> session = std::move(*started);
			session->SetExtractionEnabled(false);
			REQUIRE(session->GetAnimation() != nullptr);
			Test::RunTicks(*session, 30);
			CHECK(ReadTrace(session->GetScene()) == std::vector<std::string>{ "A:Step:1" });

			// The reload restarts the scene from its start document (no trace, the walk at 0) with a new ScriptEngine.
			REQUIRE(session->GetScripts()->GetHost().RequestSceneReload(std::nullopt).has_value());
			session->Tick();
			REQUIRE_FALSE(session->GetFatalError().has_value());
			CHECK(session->GetSceneGeneration() == 2);
			CHECK(ReadTrace(session->GetScene()).empty());
			Test::RunTicks(*session, 30);
			CHECK(ReadTrace(session->GetScene()) == std::vector<std::string>{ "A:Step:1" });
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}
	}

}
