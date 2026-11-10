#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"
#include "Support/ScriptTestFixture.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The play session's animation (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 29): the AnimationSystem phase's
// place in the step order (ADR 0028's phase-2 execution order) and the live guard that a scene without the animation
// components keeps its state hash exactly.

namespace Engine {

	namespace {

		// A MeshRenderer drawing `mesh` with its default materials.
		MeshRendererComponent MakeMeshRenderer(AssetHandle mesh)
		{
			MeshRendererComponent renderer;
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
			return renderer;
		}

		class PhaseRecorder final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& /*session*/, PlaySessionPhase phase, uint64_t /*tick*/) override { Phases.push_back(phase); }

			std::vector<PlaySessionPhase> Phases;
		};

		// The guard's gameplay script: every fixed step it moves its entity by a random fraction of its Step, so the tick
		// writes the scene, draws from the session's Random and, every 20 ticks, an id (Scene.CreateEntity).
		constexpr std::string_view PatrolSource = R"(
local Patrol = { Fields = { Step = Field.Number(0.25) } }
function Patrol:OnFixedUpdate()
	local position = self.Entity.Transform.Translation
	self.Entity.Transform.Translation = vector.create(position.x + self.Step * Random.Number(), position.y, position.z)
	if Time.GetTick() % 20 == 0 then
		Scene.CreateEntity("Marker")
	end
end
return Script.Define("Patrol", Patrol)
)";

		// The index of `phase` in `phases`, or the list's size when it is absent.
		size_t FindPhase(const std::vector<PlaySessionPhase>& phases, PlaySessionPhase phase)
		{
			return static_cast<size_t>(std::distance(phases.begin(), std::find(phases.begin(), phases.end(), phase)));
		}

	}

	TEST_SUITE("Session")
	{
		// The ledger fixes the order, never an adjacency: M21's SpriteAnimation lies between NativeStep and AnimationSystem on
		// phase 2's merged tree.
		TEST_CASE("PlaySessionPhase: AnimationSystem has its enumerator name and follows NativeStep, before PreStepTransformUpdate")
		{
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::AnimationSystem) == "AnimationSystem");
			CHECK(std::to_underlying(PlaySessionPhase::AnimationSystem) > std::to_underlying(PlaySessionPhase::NativeStep));
			CHECK(std::to_underlying(PlaySessionPhase::AnimationSystem) < std::to_underlying(PlaySessionPhase::PreStepTransformUpdate));
		}

		// The live guard (decision 29; the phase's lesson from M22's C22.2 review): a scene without an Animator or a SkinnedMesh
		// gains no runtime component, and its state hash after 60 ticks of a falling box over a ground, with a camera that a
		// script moves every tick (drawing from the session's Random, creating an entity every 20 ticks) and a mesh, is the one
		// it had at the contract (the session then entered no AnimationSystem phase), in Play, where the script runs, and in
		// Simulate, where it does not. So stream B's phase, with its view of Animators, its listener on the session's
		// ScriptEngine and the scripts' new callback, adds no hash block, no stored state and no write for it. The determinism
		// suite's committed replays (FeatureTest's and the demos') prove the same at the integration.
		TEST_CASE("PlaySession: a scene without the animation components gains no pose and hashes as at the contract")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle patrol(0x2b0201);
			REQUIRE(fixture.AddScript(patrol, "Assets/Scripts/Patrol.luau", std::string(PatrolSource)));
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			ScriptSlot slot{};
			slot.Script.SetHandle(patrol);
			camera.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			scene.CreateEntity("Statue").AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x0101)));
			for (const auto& [mode, expected] : { std::pair{ PlayMode::Play, std::string_view("57ee17bda3dc85c6") },
					 std::pair{ PlayMode::Simulate, std::string_view("8edb674979907a3c") } })
			{
				CAPTURE(static_cast<int>(mode));
				PlaySessionSpecification specification{};
				specification.Registry = &fixture.GetTypes();
				specification.Assets = &fixture.GetAssetManager();
				specification.ScriptSchemas = fixture.GetSchemaSnapshot();
				specification.Seed = 42;
				specification.Mode = mode;
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, scene);
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				Test::RunTicks(**session, 60);
				CHECK((*session)->GetScriptErrors().GetErrors().empty());
				const auto poses = (*session)->GetScene().GetRegistry().view<AnimationPoseComponent>();
				CHECK(poses.begin() == poses.end());
				CHECK(std::format("{:016x}", (*session)->ComputeStateHash()) == expected);
			}
		}

		// Stream B enters the phase: after the tick's gameplay phases (scripts, tasks, the native step) and before the
		// transform update, in Simulate too, and the session holds its AnimationSystem. The streams that enter a phase add it to
		// PlaySessionTests.cpp's phase lists at its place (ADR 0028's phase-2 ledger).
		TEST_CASE("PlaySession: AnimationSystem runs after the gameplay phases and before the transform update, in Simulate too")
		{
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				CAPTURE(static_cast<int>(mode));
				Test::SceneTestFixture fixture;
				PhaseRecorder recorder;
				PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, 42, mode);
				specification.Observer = &recorder;
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				REQUIRE((*session)->GetAnimation() != nullptr);
				(*session)->FixedStep();
				const size_t animation = FindPhase(recorder.Phases, PlaySessionPhase::AnimationSystem);
				REQUIRE(animation < recorder.Phases.size());
				CHECK(FindPhase(recorder.Phases, PlaySessionPhase::ApplyInput) < animation);
				if (mode == PlayMode::Play)
					CHECK(FindPhase(recorder.Phases, PlaySessionPhase::Tasks) < animation);
				CHECK(animation + 1 == FindPhase(recorder.Phases, PlaySessionPhase::PreStepTransformUpdate));
			}
		}
	}

}
