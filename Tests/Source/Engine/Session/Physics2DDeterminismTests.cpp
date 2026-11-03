#include "TestsPCH.h"

#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestTimeout.h"

#include <algorithm>
#include <vector>

// 2D physics determinism through the play session's state hash (ADR 0028 decision 6; Docs/Decisions/0032-m21-contract.md
// decisions 10 and 11, stream P2).

namespace Engine {

	namespace {

		class Recording2DEventListener final : public IPhysics2DEventListener
		{
		public:
			void OnPhysics2DEvent(const PhysicsEvent& event) override { Events.push_back(event); }

			std::vector<PhysicsEvent> Events{};
		};

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
		TEST_CASE("Physics2D: a pile's state hash is identical at 0, 1 and 8 Jolt threads")
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

		// Roadmap M21 "2D contact events reach scripts in canonical order", W21.1's review (the 2D twin of "Physics: contact
		// event order identical at 0, 1 and 8 Jolt threads, collider events included"): 2D boxes and balls falling onto a floor
		// of scripted pieces gathered into one static RigidBody2D, so body events and collider events interleave; the full
		// event lists the 2D system dispatches (what the ScriptEngine delivers to the 2D callbacks) are equal at every thread
		// count. Each piece's Script component names a script, which alone makes it a scripted collider; the session simulates
		// without a VM (Simulate mode), so the handle needs no asset.
		TEST_CASE("Physics2D: contact event order identical at 0, 1 and 8 Jolt threads, collider events included")
		{
			Test::SetTestCaseTimeLimit(150.0);
			const auto run = [](uint32_t workerThreads)
			{
				const Test::ScopedWorkerThreadCount threads(workerThreads);
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				Entity floor = scene.CreateEntity("Floor");
				floor.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
				for (int piece = 0; piece < 16; ++piece)
				{
					Entity child = scene.CreateEntity("Piece");
					child.Patch<TransformComponent>([piece](TransformComponent& transform)
					{
						transform.Translation = glm::vec3(static_cast<float>(piece) * 1.0f - 7.5f, -0.5f, 0.0f);
					});
					child.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.5f) });
					child.AddComponent<ScriptComponent>().Scripts.push_back(
						ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(AssetHandle(0x5c0de21)), .Fields = {}, .ExecutionOrder = 0 });
					REQUIRE(scene.SetParent(child, floor).has_value());
				}
				for (int index = 0; index < 40; ++index)
				{
					Entity body = scene.CreateEntity("Body");
					body.Patch<TransformComponent>([index](TransformComponent& transform)
					{
						transform.Translation = glm::vec3(static_cast<float>(index % 16) * 0.9375f - 7.0625f, 1.0f + static_cast<float>(index / 16) * 1.25f, 0.0f);
					});
					// Thrown sideways, so each body crosses several pieces and its collider pairs with them enter and exit.
					body.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .InitialLinearVelocity = glm::vec2(index % 2 == 0 ? 3.0f : -3.0f, 0.0f) });
					if (index % 2 == 0)
						body.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.25f) });
					else
						body.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.25f });
				}
				PlaySessionSpecification specification;
				specification.Registry = &fixture.GetRegistry();
				specification.Seed = 77;
				specification.Mode = PlayMode::Simulate;
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, scene);
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				REQUIRE((*session)->GetPhysics2D() != nullptr);
				Recording2DEventListener listener;
				(*session)->GetPhysics2D()->SetEventListener(&listener);
				for (int tick = 0; tick < 240; ++tick)
					(*session)->Tick();
				(*session)->GetPhysics2D()->SetEventListener(nullptr);
				return listener.Events;
			};
			const std::vector<PhysicsEvent> none = run(0);
			const std::vector<PhysicsEvent> one = run(1);
			const std::vector<PhysicsEvent> eight = run(8);
			const auto colliderEvents = static_cast<size_t>(std::ranges::count_if(none, [](const PhysicsEvent& event)
			{
				return event.IsColliderEvent();
			}));
			INFO("events: ", none.size(), ", collider events: ", colliderEvents);
			REQUIRE(colliderEvents >= 20);
			CHECK(one == none);
			CHECK(eight == none);
		}
	}

}
