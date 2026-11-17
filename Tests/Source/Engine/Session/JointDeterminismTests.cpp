#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/SliderJointComponent.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"
#include "Support/TestTimeout.h"

#include <format>

// Joint determinism through the play session's state hash (M37, Docs/Decisions/0054-m37-joints.md decision 5; Architecture
// §9.7, §9.10): a chain, a motored hinge, a limited slider and a breaking weld stepped 600 times end in one state at Jolt
// thread counts 0, 1 and 8, and in the committed state in every configuration (the Debug and Release unit runs).

namespace Engine {

	namespace {

		// The machines' state hash after MachineTicks ticks, recorded from M37's W37.1 and verified equal in Debug and
		// Release. A change of physics or of the scene that legitimately changes the simulation updates it with the reason in
		// the change's review (§13.6 "Re-recording").
		constexpr uint64_t CommittedMachinesHash = 0x578c12c37e30690b;

		constexpr uint32_t MachineTicks = 600;

		// A ground; a static beam with a chain of four links on distance joints, kicked sideways; a trap door on a hinge to the
		// world with limits and a position motor, a box on it; a slider piston with limits and a velocity motor; and a plank
		// welded to the world that a heavy ball breaks.
		void BuildMachines(Scene& scene)
		{
			static_cast<void>(Test::AddGround(scene));
			Entity beam = Test::AddBoxBody(scene, "Beam", glm::vec3(0.0f, 6.0f, 0.0f), glm::vec3(1.0f, 0.1f, 0.1f), BodyType::Static);
			Entity previous = beam;
			for (int link = 0; link < 4; ++link)
			{
				Entity entity = Test::AddBoxBody(scene, std::format("Link{}", link), glm::vec3(0.0f, 5.5f - 0.6f * static_cast<float>(link), 0.0f),
					glm::vec3(0.1f, 0.25f, 0.1f), BodyType::Dynamic);
				entity.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = previous.GetUUID(),
					.Anchor = glm::vec3(0.0f, 0.25f, 0.0f),
					.ConnectedAnchor = glm::vec3(0.0f, link == 0 ? -0.1f : -0.25f, 0.0f),
					.MinDistance = 0.0f,
					.MaxDistance = link == 0 ? 0.25f : 0.15f });
				if (link == 3)
					Test::PatchRigidBody(entity, [](RigidBodyComponent& body)
					{
						body.InitialLinearVelocity = glm::vec3(3.0f, 0.0f, 1.0f);
					});
				previous = entity;
			}

			Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(5.0f, 2.0f, 0.0f), glm::vec3(1.0f, 0.05f, 0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(door, [](RigidBodyComponent& body)
			{
				body.Mass = 5.0f;
			});
			door.AddComponent<HingeJointComponent>(HingeJointComponent{ .Anchor = glm::vec3(-1.0f, 0.0f, 0.0f),
				.Axis = glm::vec3(0.0f, 0.0f, 1.0f),
				.UseLimits = true,
				.LowerAngle = -80.0f,
				.UpperAngle = 0.0f,
				.Motor = JointMotorMode::Position,
				.MotorTargetAngle = -60.0f,
				.MotorMaxTorque = 20.0f });
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(5.5f, 2.3f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));

			Entity piston = Test::AddBoxBody(scene, "Piston", glm::vec3(-5.0f, 1.0f, 0.0f), glm::vec3(0.5f, 0.25f, 0.25f), BodyType::Dynamic);
			piston.AddComponent<SliderJointComponent>(SliderJointComponent{ .UseLimits = true,
				.LowerLimit = -1.0f,
				.UpperLimit = 1.5f,
				.Motor = JointMotorMode::Velocity,
				.MotorTargetVelocity = 1.5f,
				.MotorMaxForce = 200.0f });
			static_cast<void>(Test::AddBoxBody(scene, "Block", glm::vec3(-3.5f, 0.25f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));

			Entity plank = Test::AddBoxBody(scene, "Plank", glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(1.0f, 0.05f, 0.25f), BodyType::Dynamic);
			plank.AddComponent<FixedJointComponent>(FixedJointComponent{ .Anchor = glm::vec3(-1.0f, 0.0f, 0.0f), .BreakForce = 400.0f, .BreakTorque = 300.0f });
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(0.5f, 4.0f, 5.0f), 0.3f, BodyType::Dynamic);
			Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
			{
				body.Mass = 20.0f;
			});
		}

		// The machines' hash after MachineTicks ticks with `workerThreads` Jolt threads; `broke`, when given, receives whether
		// the weld broke. It REQUIREs, so callers keep it out of CHECK expressions.
		uint64_t RunMachines(uint32_t workerThreads, bool* broke = nullptr)
		{
			const Test::ScopedWorkerThreadCount threads(workerThreads);
			Test::SceneTestFixture fixture;
			BuildMachines(fixture.GetScene());
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 1234));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Test::RunTicks(**session, MachineTicks);
			if (broke != nullptr)
				*broke = (*session)->GetScene().FindEntityByPath("/Plank").GetComponent<FixedJointComponent>().Broken;
			return (*session)->ComputeStateHash();
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PhysicsJoints: a chain, a motored hinge, a limited slider and a breaking weld end in one state with 0, 1 and 8 Jolt threads")
		{
			Test::SetTestCaseTimeLimit(240.0);
			bool broke = false;
			const uint64_t none = RunMachines(0, &broke);
			const uint64_t one = RunMachines(1);
			const uint64_t eight = RunMachines(8);
			const uint64_t again = RunMachines(0);
			CHECK(broke);
			CHECK(one == none);
			CHECK(eight == none);
			CHECK(again == none);
		}

		TEST_CASE("PhysicsJoints: the machines' state hash equals the committed constant in every configuration")
		{
			Test::SetTestCaseTimeLimit(120.0);
			// One committed value for every configuration (§9.1: a configuration-dependent result is a bug).
			const uint64_t hash = RunMachines(PhysicsEngine::GetWorkerThreadCount());
			INFO("machines hash: ", std::format("{:016x}", hash));
			CHECK(hash == CommittedMachinesHash);
		}
	}

}
