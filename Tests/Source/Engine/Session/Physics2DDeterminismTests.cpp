#include "TestsPCH.h"

#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

// 2D physics determinism through the play session's state hash (ADR 0028 decision 6; Docs/Decisions/0032-m21-contract.md
// decisions 10 and 11, stream P2): skipped until stream P2 implements it.

namespace Engine {

	namespace {

		// A floor and a pile of 2D crates and balls.
		void Populate2DPile(Scene& scene)
		{
			Entity floor = scene.CreateEntity("Floor");
			floor.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, -0.5f, 0.0f);
			});
			floor.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
			floor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(8.0f, 0.5f) });
			for (int index = 0; index < 40; ++index)
			{
				Entity body = scene.CreateEntity("Body");
				body.Patch<TransformComponent>([index](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(static_cast<float>(index % 5) * 0.55f - 1.1f, 0.6f + static_cast<float>(index / 5) * 0.7f, 0.0f);
				});
				body.AddComponent<RigidBody2DComponent>();
				if (index % 2 == 0)
					body.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.25f) });
				else
					body.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.25f });
			}
		}

	}

	TEST_SUITE("Session")
	{
		// Roadmap M21 "The Physics2D replay is identical in all four runs": the world-level half, as M11's pile.
		TEST_CASE("Physics2D: a pile's state hash is identical at 0, 1 and 8 Jolt threads" * doctest::skip(true))
		{
			const auto run = [](uint32_t workerThreads)
			{
				const Test::ScopedWorkerThreadCount threads(workerThreads);
				Test::SceneTestFixture fixture;
				Populate2DPile(fixture.GetScene());
				PlaySessionSpecification specification;
				specification.Registry = &fixture.GetRegistry();
				specification.Seed = 7;
				specification.Mode = PlayMode::Simulate;
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				REQUIRE((*session)->GetPhysics2D() != nullptr);
				for (int tick = 0; tick < 300; ++tick)
					(*session)->Tick();
				CHECK((*session)->GetPhysics2D()->GetStats().BodyCount == 41u);
				return (*session)->ComputeStateHash();
			};
			const uint64_t first = run(0);
			CHECK(run(1) == first);
			CHECK(run(8) == first);
		}
	}

}
