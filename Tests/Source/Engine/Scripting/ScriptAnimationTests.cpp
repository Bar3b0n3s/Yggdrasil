#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Time.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
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
// Animator's methods, the joint query and OnAnimationEvent.

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

		// Probe.Record(message): a "Trace:<message>" entity of the host's scene, created without the script's Scene API.
		int RecordHeldEvent(ScriptCall& call)
		{
			const std::string message = Lua::Check<std::string>(call, 1);
			static_cast<void>(Lua::GetEngine(call)->GetHost().GetScene().CreateEntity("Trace:" + message));
			return 0;
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

		// Checks every field of the event it receives (the walk's "Step" of layer 1 at 0.25 s) and records it.
		constexpr std::string_view EventFieldsSource = R"(
local T = {}
function T:OnAnimationEvent(event)
	assert(event.Name == "Step" and event.Layer == 1 and event.Time == 0.25)
	assert(event.Clip == self.Entity.Animator.Layers[1].Clip)
	Scene.CreateEntity("Trace:" .. event.Name)
end
return Script.Define("EventFields", T)
)";

		// A face of one triangle with the morph targets "Smile" (default weight 0.25) and "Blink" (default 0.75).
		Ref<MeshData> MakeFace()
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			mesh->Vertices = { { .Position = { 0.0f, 0.0f, 0.0f } }, { .Position = { 1.0f, 0.0f, 0.0f } }, { .Position = { 0.0f, 1.0f, 0.0f } } };
			for (const MeshVertex& vertex : mesh->Vertices)
				mesh->Bounds.Extend(vertex.Position);
			mesh->Indices = { 0, 1, 2 };
			mesh->Submeshes.push_back({ .IndexCount = 3, .Bounds = mesh->Bounds });
			mesh->Slots.push_back({ .Name = "Default" });
			mesh->MorphTargets.push_back({ .Name = "Smile", .DefaultWeight = 0.25f, .Deltas = std::vector<MeshMorphDelta>(3) });
			mesh->MorphTargets.push_back({ .Name = "Blink", .DefaultWeight = 0.75f, .Deltas = std::vector<MeshMorphDelta>(3) });
			return mesh;
		}

		// The Animator's and the SkinnedMesh's methods in `mode`, their refusals included, whose API coverage it adds to
		// `snapshots` ("ScriptApi: every registered function, method, property, operator and constructor has a test").
		void AnimationMethodsScenario(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			Scene& scene = fixture.GetScene();
			PublishRig(fixture.GetAssetManager());
			const AssetHandle faceHandle(0x3e5000000000011);
			fixture.GetAssetManager().Publish(faceHandle, MakeFace(), "Assets/Models/Face.gltf");
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle),
				.RootMotion = AnimatorRootMotion::Apply });
			Entity holder = scene.CreateEntity("Holder");
			holder.AddComponent<AnimatorComponent>(AnimatorComponent{ .Layers = { AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(WalkHandle) } } });
			Entity face = scene.CreateEntity("Face");
			MeshRendererComponent renderer;
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(faceHandle);
			face.AddComponent<MeshRendererComponent>(renderer);
			face.AddComponent<SkinnedMeshComponent>();
			Entity bare = scene.CreateEntity("Bare");
			bare.AddComponent<MeshRendererComponent>();
			bare.AddComponent<SkinnedMeshComponent>();
			REQUIRE(fixture.Start());

			// Play replaces a layer's clip and time and keeps its Speed, Weight and Loop; a layer 0, a gap and a negative
			// time are refused; SetWeight refuses a layer the Animator lacks. Without a pose there is no joint and no motion.
			CheckAnimationScript(fixture, R"(
local walk = Scene.FindByName("Holder").Animator.Layers[1].Clip
local animator = Scene.FindByName("Hero").Animator
animator:Play(1, walk, 0.25)
animator:SetWeight(1, 0.5)
local layers = animator.Layers
layers[1].Speed = 2
layers[1].Loop = false
animator.Layers = layers
animator:Play(1, nil)
local layer = animator.Layers[1]
assert(layer.Clip == nil and layer.Time == 0 and layer.Speed == 2 and layer.Weight == 0.5 and layer.Loop == false)
animator:Play(1, walk)
assert(not pcall(function() animator:Play(0, walk) end))
assert(not pcall(function() animator:Play(3, walk) end))
assert(not pcall(function() animator:Play(1, walk, -1) end))
assert(not pcall(function() animator:SetWeight(2, 0.5) end))
assert(not pcall(function() animator:SetWeight(1, -0.5) end))
assert(#animator.Layers == 1)
local position, rotation = animator:GetJointTransform("Hips")
assert(position == nil and rotation == nil)
local moved, turned = animator:GetRootMotion()
assert(moved == vector.zero and turned.w == 1)
return true
)");

			// SetMorphWeight grows the weights with the targets' defaults up to the named one; an unknown target, a weight
			// outside [-1, 1] and a renderer without a mesh are refused.
			CheckAnimationScript(fixture, R"(
local face = Scene.FindByName("Face").SkinnedMesh
face:SetMorphWeight("Blink", 0.5)
assert(#face.MorphWeights == 2 and face.MorphWeights[1] == 0.25 and face.MorphWeights[2] == 0.5)
face:SetMorphWeight("Smile", -0.5)
assert(face.MorphWeights[1] == -0.5 and face.MorphWeights[2] == 0.5)
assert(not pcall(function() face:SetMorphWeight("Frown", 0.1) end))
assert(not pcall(function() face:SetMorphWeight("Smile", 1.5) end))
assert(not pcall(function() Scene.FindByName("Bare").SkinnedMesh:SetMorphWeight("Smile", 0) end))
assert(face.MorphWeights[1] == -0.5)
return true
)");

			// After a step the pose answers: the hips held at the walk's start (Apply moved the entity instead), its root
			// motion in the parent space.
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &fixture.GetAssetManager() });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			TransformSystem::Update(scene);
			(*system)->Step(SimStep::FromTick(0, 0.25));
			TransformSystem::Update(scene);
			CheckAnimationScript(fixture, R"(
local hero = Scene.FindByName("Hero")
local position, rotation = hero.Animator:GetJointTransform("Hips")
assert(math.abs(position.z - 0.5) < 1e-5 and math.abs(rotation.w - 1) < 1e-6)
local moved = hero.Animator:GetRootMotion()
assert(math.abs(moved.z - 0.5) < 1e-5 and math.abs(hero.Transform.Translation.z - 0.5) < 1e-5)
return true
)");
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

		// OnAnimationEvent in a play session of `mode` (Play, test mode): the walk's event reaches the Animator entity's
		// behaviour with its fields; its API coverage is added to `snapshots`.
		void AnimationCallbackScenario(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			PublishRig(fixture.GetAssetManager());
			const AssetHandle script(0x2b0103);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/EventFields.luau", std::string(EventFieldsSource)));
			Scene& editScene = fixture.GetScene();
			Entity hero = editScene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle),
				.Layers = { AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(WalkHandle) } } });
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			hero.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 7;
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = mode;
			specification.TestMode = true;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			Scope<PlaySession> session = std::move(*started);
			session->SetExtractionEnabled(false);
			Test::RunTicks(*session, 30);
			CHECK(session->GetScriptErrors().GetErrors().empty());
			CHECK(ReadTrace(session->GetScene()) == std::vector<std::string>{ "Step" });
			const auto coverage = session->GetScripts()->GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	namespace Test {

		// The Animator's and SkinnedMesh's methods; OnAnimationEvent is the Behaviour scenario's (RunAnimationCallbackCoverage).
		void RunAnimationBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			AnimationMethodsScenario(mode, snapshots);
		}

		void RunAnimationCallbackCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			AnimationCallbackScenario(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		// Play(layer, clip, time?) fills a layer (1-based; the next index appends one), SetWeight sets its weight, and
		// GetJointTransform and GetRootMotion read the last AnimationSystem phase's pose; a layer beyond the next index, a weight
		// outside [0, 1] and an unknown joint raise or return nil as decision 31 states.
		TEST_CASE("ScriptAnimation: Play, SetWeight, GetJointTransform and GetRootMotion drive and read the Animator")
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
		TEST_CASE("ScriptAnimation: OnAnimationEvent reaches every slot of the Animator's entity in slot order")
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
		TEST_CASE("ScriptAnimation: OnAnimationEvent reaches the session's scripts from its start and after Scene.Reload")
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

		// The methods' refusals and SkinnedMesh:SetMorphWeight, in the Editor run mode (the coverage case runs them in every
		// mode): Play keeps a layer's Speed, Weight and Loop and refuses a layer 0, a gap or a negative time; SetMorphWeight
		// grows the weights with the targets' defaults and refuses an unknown target, a weight outside [-1, 1] and a renderer
		// without a mesh; GetJointTransform and GetRootMotion read the pose, Apply's delta in the parent space.
		TEST_CASE("ScriptAnimation: Play keeps a layer's settings, SetMorphWeight fills the targets' defaults, and bad arguments are refused")
		{
			std::vector<ScriptApiCoverage> snapshots;
			AnimationMethodsScenario(RunModes::Editor, snapshots);
			CHECK(snapshots.size() == 1);
		}

		// An instance that has not started yet (created by another behaviour in the same tick, before the AnimationSystem
		// phase) holds its events, as it holds physics callbacks: they arrive after its OnStart, at the next fixed step's start
		// flush, once.
		TEST_CASE("ScriptAnimation: an instance that has not started holds its animation events until after its OnStart")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle clipHandle(0xa41c000000000012);
			Ref<AnimationClipData> clip = CreateRef<AnimationClipData>();
			clip->Duration = 1.0f;
			clip->Events = { AnimationEvent{ .Name = "Begin", .Time = 0.0f } };
			fixture.GetAssetManager().Publish(clipHandle, clip, "Assets/Models/Hero.gltf");
			PublishRig(fixture.GetAssetManager());
			const AssetHandle held(0x2b0104);
			REQUIRE(fixture.AddScript(held, "Assets/Scripts/HeldTrace.luau", R"(
local T = {}
function T:OnStart() Scene.CreateEntity("Trace:start") end
function T:OnAnimationEvent(event) Scene.CreateEntity("Trace:" .. event.Name) end
return Script.Define("HeldTrace", T)
)"));
			const AssetHandle spawner(0x2b0105);
			REQUIRE(fixture.AddScript(spawner, "Assets/Scripts/Spawner.luau", R"(
local S = {}
function S:OnFixedUpdate()
	if Time.GetTick() == 0 then
		Scene.FindByName("Hero"):AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/HeldTrace.luau"), Fields = {}, ExecutionOrder = 0}}})
	end
end
return Script.Define("Spawner", S)
)"));
			Scene& editScene = fixture.GetScene();
			Entity hero = editScene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle),
				.Layers = { AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(clipHandle) } } });
			ScriptSlot slot{};
			slot.Script.SetHandle(spawner);
			editScene.CreateEntity("Spawner").AddComponent<ScriptComponent>(ScriptComponent{ { slot } });

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
			Test::RunTicks(*session, 3);
			CHECK(session->GetScriptErrors().GetErrors().empty());
			// The event of tick 0 waited for the instance's OnStart (the frame's start flush after tick 0) and arrived once, at
			// tick 1's start flush.
			CHECK(ReadTrace(session->GetScene()) == std::vector<std::string>{ "start", "Begin" });
		}

		// W23.3's review: an unstarted instance holds its animation events and its physics callbacks in one queue, in arrival
		// order, so its start flush delivers them as they arrived (the tick's AnimationSystem phase before its physics
		// post-step); an instance destroyed before its start flush discards its held events, which never arrive.
		TEST_CASE("ScriptAnimation: held animation events and contacts arrive in arrival order, and an instance destroyed before its start drops them")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.TestMode = true;
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Records callback ordering into the test scene.").Function("Record", &RecordHeldEvent, "(message: string) -> ()", "Append one test observation.", { .Mutates = true });
				return {};
			};
			Test::ScriptTestFixture fixture(specification);
			const AssetHandle held(0x2b0106);
			REQUIRE(fixture.AddScript(held, "Assets/Scripts/HeldOrder.luau", R"(
local T = {}
function T:OnStart() Probe.Record(self.Entity.Name .. ":start") end
function T:OnAnimationEvent(event) Probe.Record(self.Entity.Name .. ":" .. event.Name) end
function T:OnTriggerEnter(other) Probe.Record(self.Entity.Name .. ":trigger") end
return Script.Define("HeldOrder", T)
)"));
			REQUIRE(fixture.Start());
			Scene& scene = fixture.GetScene();
			ScriptEngine& engine = *fixture.GetEngine();
			const Entity kept = scene.CreateEntityWithID(UUID(170), "Kept");
			const Entity dropped = scene.CreateEntityWithID(UUID(171), "Dropped");
			for (Entity entity : { kept, dropped })
			{
				ScriptSlot slot{};
				slot.Script.SetHandle(held);
				entity.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			}
			REQUIRE(engine.SynchronizeInstances());

			engine.OnAnimationEvent(AnimationEventRecord{ .Entity = UUID(170), .Name = "Begin" });
			engine.OnPhysicsEvent(PhysicsEvent{ .Type = PhysicsEventType::TriggerEnter, .Self = UUID(170), .Other = UUID(172) });
			engine.OnAnimationEvent(AnimationEventRecord{ .Entity = UUID(170), .Name = "End" });
			engine.OnAnimationEvent(AnimationEventRecord{ .Entity = UUID(171), .Name = "Lost" });
			CHECK(ReadTrace(scene).empty());

			scene.DestroyEntity(dropped);
			CHECK(engine.PrepareDestroyFlush() == 1);
			engine.FinishDestroyFlush();
			scene.FlushPendingDestroys();
			fixture.Frame.Phase = InputPhase::Step;
			engine.StartPending();
			CHECK(ReadTrace(scene) == std::vector<std::string>{ "Kept:start", "Kept:Begin", "Kept:trigger", "Kept:End" });
			CHECK(fixture.Errors.empty());
		}

		// A joint a clip scales to nothing has a position but no rotation to report.
		TEST_CASE("ScriptAnimation: GetJointTransform reports no rotation for a joint a clip scales to nothing")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			PublishRig(fixture.GetAssetManager());
			const AssetHandle shrinkHandle(0xa41c000000000013);
			Ref<AnimationClipData> shrink = CreateRef<AnimationClipData>();
			shrink->Duration = 1.0f;
			shrink->Channels.push_back(AnimationChannel{ .Target = "Hips", .Path = AnimationPath::Scale, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f }, .Values = { 0.0f, 0.0f, 0.0f } });
			fixture.GetAssetManager().Publish(shrinkHandle, shrink, "Assets/Models/Hero.gltf");
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle),
				.Layers = { AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(shrinkHandle) } } });
			REQUIRE(fixture.Start());
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &fixture.GetAssetManager() });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			TransformSystem::Update(scene);
			(*system)->Step(SimStep::FromTick(0, 1.0 / 60.0));
			CheckAnimationScript(fixture, R"(
local position, rotation = Scene.FindByName("Hero").Animator:GetJointTransform("Hips")
assert(position ~= nil and math.abs(position.y - 1) < 1e-6 and rotation == nil)
return true
)");
			CHECK(fixture.Errors.empty());
		}

		// The callback coverage of every run mode (the Behaviour coverage case requires it).
		TEST_CASE("ScriptAnimation: OnAnimationEvent carries the event's name, 1-based layer, clip and time in every run mode")
		{
			for (const RunModes mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				CAPTURE(static_cast<int>(mode));
				std::vector<ScriptApiCoverage> snapshots;
				AnimationCallbackScenario(mode, snapshots);
			}
		}
	}

}
