#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Core/Aabb.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <optional>
#include <vector>

// M21's place in the play session (ADR 0028's phase-2 execution order; Docs/Decisions/0032-m21-contract.md decisions 5 and
// 10): the phase names, and the order in which the session enters them (streams B and P2; PlaySessionTests.cpp's lists name
// them at the same places).

namespace Engine {

	namespace {

		class Recording2DPhases final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& /*session*/, PlaySessionPhase phase, uint64_t /*tick*/) override { Phases.push_back(phase); }

			std::vector<PlaySessionPhase> Phases{};
		};

		// At the start of the 3D post-step of tick `Tick`, writes `Translation` into the local Transform of `Target` through the
		// registry, without counting a scene revision, as the 3D write-back and a 3D contact callback write transforms, after
		// the step's transform update.
		class PostStepWriter final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				if (phase != PlaySessionPhase::PhysicsPostStep || tick != Tick)
					return;
				const Entity target = session.GetScene().FindEntityByID(Target);
				REQUIRE(target.IsValid());
				session.GetScene().GetRegistry().patch<TransformComponent>(target.GetHandle(), [this](TransformComponent& transform)
				{
					transform.Translation = Translation;
				});
			}

			UUID Target{};
			uint64_t Tick = 0;
			glm::vec3 Translation = glm::vec3(0.0f);
		};

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: M21's phases have their enumerator names")
		{
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::SpriteAnimation) == "SpriteAnimation");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::Physics2DPreStep) == "Physics2DPreStep");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::Physics2DStep) == "Physics2DStep");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::Physics2DPostStep) == "Physics2DPostStep");
			// The ledger's order: SpriteAnimation after NativeStep, the 2D phases after the 3D post-step.
			CHECK(static_cast<int>(PlaySessionPhase::SpriteAnimation) == static_cast<int>(PlaySessionPhase::NativeStep) + 1);
			CHECK(static_cast<int>(PlaySessionPhase::Physics2DPreStep) == static_cast<int>(PlaySessionPhase::PhysicsPostStep) + 1);
			CHECK(static_cast<int>(PlaySessionPhase::DestroyFlush) == static_cast<int>(PlaySessionPhase::Physics2DPostStep) + 1);
		}

		TEST_CASE("PlaySession: SpriteAnimation runs before the transform update and the 2D phases after the 3D post-step, in Simulate too")
		{
			Test::SceneTestFixture fixture;
			Recording2DPhases observer;
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = 42;
			specification.Observer = &observer;
			specification.Mode = PlayMode::Simulate;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			(*session)->Tick();
			// M23's AnimationSystem follows SpriteAnimation on phase 2's merged tree (ADR 0028's phase-2 execution order;
			// Docs/Decisions/0034-m23-contract.md decision 29 and its Landing).
			const std::vector<PlaySessionPhase> expected = {
				PlaySessionPhase::InterpolationSnapshot,
				PlaySessionPhase::ApplyInput,
				PlaySessionPhase::TextLatch, // M22's phases, at the ledger's places (they run in Simulate too)
				PlaySessionPhase::UiInput,
				PlaySessionPhase::SpriteAnimation,
				PlaySessionPhase::AnimationSystem,
				PlaySessionPhase::PreStepTransformUpdate,
				PlaySessionPhase::PhysicsPreStep,
				PlaySessionPhase::PhysicsStep,
				PlaySessionPhase::PhysicsPostStep,
				PlaySessionPhase::Physics2DPreStep,
				PlaySessionPhase::Physics2DStep,
				PlaySessionPhase::Physics2DPostStep,
				PlaySessionPhase::DestroyFlush,
				PlaySessionPhase::PostStepTransformUpdate,
				PlaySessionPhase::UiLayout,
				PlaySessionPhase::LatchFrame,
				PlaySessionPhase::FrameDestroyFlush,
				PlaySessionPhase::FrameTransformUpdate,
				PlaySessionPhase::FrameUiLayout,
				PlaySessionPhase::Particles, // M26's presentation phase (ADR 0028's phase-3 execution order)
				PlaySessionPhase::RenderExtraction,
			};
			CHECK(observer.Phases == expected);
			CHECK((*session)->GetPhysics2D() != nullptr);
		}

		// Decision 10 and the phase-2 ledger's execution order (W21.1's review): Physics2DPreStep reads the cached world
		// matrices, so when the scene holds 2D components and the 3D world holds a body it runs the transform update first,
		// and a transform the 3D post-step wrote (its write-back, a 3D contact callback) moves the 2D body in the same tick.
		TEST_CASE("PlaySession: the 2D pre-step sees a transform the 3D post-step wrote in the same tick")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity crate = scene.CreateEntity("Crate3D");
			crate.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			crate.AddComponent<BoxColliderComponent>();
			Entity lift = scene.CreateEntity("Lift2D");
			lift.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Kinematic });
			lift.AddComponent<BoxCollider2DComponent>();
			PostStepWriter writer;
			writer.Target = lift.GetUUID();
			writer.Tick = 2;
			writer.Translation = glm::vec3(4.0f, 3.0f, 0.0f);
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = 42;
			specification.Observer = &writer;
			specification.Mode = PlayMode::Simulate;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Physics2DSystem& physics2D = *(*session)->GetPhysics2D();
			for (int tick = 0; tick < 3; ++tick)
				(*session)->Tick();
			// Tick 2's 2D step swept the kinematic lift to the written pose.
			const std::optional<Aabb> bounds = physics2D.GetBodyBounds(lift.GetUUID());
			REQUIRE(bounds.has_value());
			CHECK(bounds->GetCenter().x == doctest::Approx(4.0f).epsilon(0.001));
			CHECK(bounds->GetCenter().y == doctest::Approx(3.0f).epsilon(0.001));
		}
	}

}
