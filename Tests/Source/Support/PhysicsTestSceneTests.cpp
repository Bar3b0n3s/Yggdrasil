#include "TestsPCH.h"
#include "Support/PhysicsTestScene.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/GlmApprox.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("PhysicsTestScene: bodies are placed with their collider and the requested RigidBody")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity ground = Test::AddGround(scene);
			const Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.25f), BodyType::Dynamic);
			const Entity child = Test::AddSphereBody(scene, "Child", glm::vec3(0.0f, 1.0f, 0.0f), 0.5f, std::nullopt, box);

			CHECK(ground.GetComponent<RigidBodyComponent>().Type == BodyType::Static);
			CHECK(Test::ApproxEqual(ground.GetComponent<TransformComponent>().Translation, glm::vec3(0.0f, -0.5f, 0.0f)));
			CHECK(Test::ApproxEqual(ground.GetComponent<BoxColliderComponent>().HalfExtents, glm::vec3(50.0f, 0.5f, 50.0f)));
			CHECK(box.GetComponent<RigidBodyComponent>().Type == BodyType::Dynamic);
			CHECK(Test::ApproxEqual(box.GetComponent<TransformComponent>().Translation, glm::vec3(1.0f, 2.0f, 3.0f)));
			CHECK(Test::ApproxEqual(box.GetComponent<BoxColliderComponent>().HalfExtents, glm::vec3(0.25f)));
			// A collider-only child.
			CHECK_FALSE(child.HasComponent<RigidBodyComponent>());
			CHECK(child.GetComponent<SphereColliderComponent>().Radius == doctest::Approx(0.5f));
			CHECK(child.GetParent() == box);
			CHECK(Test::GetEntityId(scene, "/Box/Child") == child.GetUUID());
			CHECK_FALSE(Test::GetEntityId(scene, "/Nobody").IsValid());

			Test::PatchRigidBody(box, [](RigidBodyComponent& body)
			{
				body.Mass = 3.0f;
			});
			CHECK(box.GetComponent<RigidBodyComponent>().Mass == doctest::Approx(3.0f));
		}

		TEST_CASE("PhysicsTestScene: a session over the fixture's scene steps and reports world positions")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Marker", glm::vec3(4.0f, 5.0f, 6.0f), glm::vec3(0.5f), std::nullopt));
			const PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, 7, PlayMode::Simulate);
			CHECK(specification.Seed == 7);
			CHECK(specification.Mode == PlayMode::Simulate);
			CHECK(specification.Registry == &fixture.GetRegistry());

			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Test::RunTicks(**session, 3);
			CHECK((*session)->GetTick() == 3);
			// A collider without a RigidBody is an implicit static body (§5.3): it does not move.
			CHECK(Test::ApproxEqual(Test::GetWorldPosition(**session, "/Marker"), glm::vec3(4.0f, 5.0f, 6.0f)));
		}

		TEST_CASE("PhysicsTestScene: the recording listener keeps every event and diagnostic in order and finds events by entity and type")
		{
			Test::RecordingPhysicsListener listener;
			const UUID a(0x1111);
			const UUID b(0x2222);
			int callbacks = 0;
			listener.OnEvent = [&callbacks](const PhysicsEvent& /*event*/)
			{
				++callbacks;
			};
			listener.OnPhysicsEvent(PhysicsEvent{ .Type = PhysicsEventType::CollisionEnter, .Self = a, .Other = b });
			listener.OnPhysicsEvent(PhysicsEvent{ .Type = PhysicsEventType::CollisionEnter, .Self = b, .Other = a });
			listener.OnPhysicsEvent(PhysicsEvent{ .Type = PhysicsEventType::CollisionExit, .Self = a, .Other = b });
			CHECK(listener.Events.size() == 3);
			CHECK(callbacks == 3);
			CHECK(listener.Find(a, PhysicsEventType::CollisionEnter).size() == 1);
			CHECK(listener.Find(a, PhysicsEventType::CollisionExit).size() == 1);
			CHECK(listener.Find(b, PhysicsEventType::CollisionExit).empty());
			// Diagnostics are recorded apart from the events.
			listener.OnPhysicsDiagnostic(PhysicsDiagnostic{ .Code = std::string(PhysicsAllDofsLockedCode), .Entity = a });
			REQUIRE(listener.Diagnostics.size() == 1);
			CHECK(listener.Diagnostics[0].Code == PhysicsAllDofsLockedCode);
			CHECK(listener.Diagnostics[0].Entity == a);
			CHECK(listener.Events.size() == 3);
			CHECK(callbacks == 3);
		}
	}

}
