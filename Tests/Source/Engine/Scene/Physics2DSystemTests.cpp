#include "TestsPCH.h"

#include "Engine/Scene/Physics2DSystem.h"

#include "Engine/Core/Hash.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DValidation.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <optional>
#include <vector>

// The play session's 2D physics (ADR 0028 decision 6; Docs/Decisions/0032-m21-contract.md decisions 9 to 12, stream P2;
// Roadmap M21 "2D contact events reach scripts in canonical order", "An entity holding both 2D and 3D physics components is
// a validation error"): skipped until stream P2 implements it.

namespace Engine {

	namespace {

		constexpr double FixedDelta2DSystem = 1.0 / 60.0;

		class Recording2DListener final : public IPhysics2DEventListener
		{
		public:
			void OnPhysics2DEvent(const PhysicsEvent& event) override { Events.push_back(event); }

			std::vector<PhysicsEvent> Events{};
		};

		Entity Create2DEntity(Scene& scene, std::string_view name, const glm::vec2& position)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([position](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(position, 0.0f);
			});
			return entity;
		}

		void StepBoth(Physics2DSystem& system, uint64_t tick)
		{
			const SimStep step = SimStep::FromTick(tick, FixedDelta2DSystem);
			system.PreStep(step);
			system.Step(step);
			system.PostStep(step);
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Physics2DSystem: contact events reach the listener sorted by the event order key, in the plane" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity floor = Create2DEntity(scene, "Floor", glm::vec2(0.0f, -0.5f));
			floor.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
			floor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(10.0f, 0.5f) });
			for (int index = 0; index < 6; ++index)
			{
				Entity crate = Create2DEntity(scene, "Crate", glm::vec2(static_cast<float>(index) - 2.5f, 0.6f + 0.2f * static_cast<float>(index)));
				crate.AddComponent<RigidBody2DComponent>();
				if (index % 2 == 0)
					crate.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.4f) });
				else
					crate.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.4f });
			}
			TransformSystem::Update(scene);

			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			Recording2DListener listener;
			(*system)->SetEventListener(&listener);
			for (uint64_t tick = 0; tick < 120; ++tick)
				StepBoth(**system, tick);
			(*system)->SetEventListener(nullptr);

			REQUIRE(listener.Events.size() >= 6);
			for (const PhysicsEvent& event : listener.Events)
			{
				CHECK(event.Contact.Point.z == 0.0f);
				CHECK(event.Contact.Normal.z == 0.0f);
			}
			// Within each PostStep (one tick), the events follow M20's canonical order key.
			for (size_t index = 1; index < listener.Events.size(); ++index)
			{
				if (listener.Events[index - 1].Tick == listener.Events[index].Tick)
					CHECK(GetPhysicsEventOrderKey(listener.Events[index - 1]) <= GetPhysicsEventOrderKey(listener.Events[index]));
			}
			// Every crate ends at rest on the floor, on the plane.
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const ConstEntity entity = scene.FindEntityByID(id);
				CHECK(entity.GetComponent<TransformComponent>().Translation.z == 0.0f);
			}
		}

		TEST_CASE("Physics2DSystem: an entity holding 2D and 3D physics components keeps its 3D body only" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity mixed = Create2DEntity(scene, "Mixed", glm::vec2(0.0f, 2.0f));
			mixed.AddComponent<RigidBodyComponent>();
			mixed.AddComponent<BoxColliderComponent>();
			mixed.AddComponent<RigidBody2DComponent>();
			mixed.AddComponent<BoxCollider2DComponent>();
			TransformSystem::Update(scene);

			Result<Scope<PhysicsSystem>> physics = PhysicsSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(physics.has_value(), physics.error().ToString());
			Result<Scope<Physics2DSystem>> physics2D = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(physics2D.has_value(), physics2D.error().ToString());
			CHECK((*physics)->GetBodyInfo(mixed.GetUUID()).has_value());
			CHECK_FALSE((*physics2D)->GetBodyBounds(mixed.GetUUID()).has_value());
			CHECK((*physics2D)->GetStats().BodyCount == 0);
			const std::span<const PhysicsDiagnostic> diagnostics = (*physics2D)->GetDiagnostics();
			REQUIRE(diagnostics.size() == 1);
			CHECK(diagnostics[0].Code == Physics2DMixedWorldsCode);
			CHECK(diagnostics[0].Severity == DiagnosticSeverity::Error);
			CHECK(diagnostics[0].Entity == mixed.GetUUID());
		}

		TEST_CASE("Physics2DSystem: the state hash block is empty without 2D bodies and covers them otherwise" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity box3D = Create2DEntity(scene, "Box3D", glm::vec2(0.0f));
			box3D.AddComponent<RigidBodyComponent>();
			box3D.AddComponent<BoxColliderComponent>();
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> empty = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(empty.has_value(), empty.error().ToString());
			XXH64Hasher untouched(0);
			XXH64Hasher appended(0);
			(*empty)->AppendStateHash(appended);
			CHECK(appended.Digest() == untouched.Digest());

			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(0.0f, 3.0f));
			ball.AddComponent<RigidBody2DComponent>();
			ball.AddComponent<CircleCollider2DComponent>();
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> withBall = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE(withBall.has_value());
			XXH64Hasher covered(0);
			(*withBall)->AppendStateHash(covered);
			CHECK(covered.Digest() != untouched.Digest());
		}

		TEST_CASE("Physics2DSystem: the 2D world is created at the first 2D body, so a scene without one holds no second world" * doctest::skip(true))
		{
			// Every 3D game's session keeps its one PhysicsWorld ("PhysicsSystem: destroying the session destroys every body and
			// the world"): no second Jolt world, temp allocator or caches until a 2D body exists.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity box3D = Create2DEntity(scene, "Box3D", glm::vec2(0.0f));
			box3D.AddComponent<RigidBodyComponent>();
			box3D.AddComponent<BoxColliderComponent>();
			TransformSystem::Update(scene);
			const uint32_t worldsBefore = PhysicsEngine::GetLiveWorldCount();
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Gravity = glm::vec2(0.0f, -2.0f) });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			StepBoth(**system, 0);
			CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore);
			// An empty world's answers.
			CHECK((*system)->GetStats().BodyCount == 0);
			const Result<std::optional<Physics2DRaycastHit>> miss = (*system)->Raycast(glm::vec2(-5.0f, 0.0f), glm::vec2(1.0f, 0.0f), 10.0f);
			REQUIRE(miss.has_value());
			CHECK_FALSE(miss->has_value());
			CHECK((*system)->GetGravity() == glm::vec2(0.0f, -2.0f));
			REQUIRE((*system)->SetGravity(glm::vec2(1.0f, -3.0f)).has_value());

			// The first 2D body creates the world, which takes the gravity set before.
			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(4.0f, 3.0f));
			ball.AddComponent<RigidBody2DComponent>();
			ball.AddComponent<CircleCollider2DComponent>();
			TransformSystem::Update(scene);
			StepBoth(**system, 1);
			CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore + 1);
			CHECK((*system)->GetStats().BodyCount == 1);
			CHECK((*system)->GetGravity() == glm::vec2(1.0f, -3.0f));
			system->reset();
			CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore);
		}
	}

}
