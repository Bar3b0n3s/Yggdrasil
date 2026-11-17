#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/SliderJointComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/PhysicsJoints.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The play session's joints (M37, Docs/Decisions/0054-m37-joints.md decisions 3 to 6 and 9; Architecture §9.10;
// PhysicsSystem.h's M37 paragraph): joints made after their bodies, the authored pose as a hinge's zero, kept frames across
// body rebuilds and frames formed again, in-place changes, CollideConnected, breaking and mending, every body removal
// path, PHYSICS_JOINT_INVALID at run time, the state hash's joint part, two histories stepping alike, and random joints and
// edits never reaching a Jolt assert. Through PlaySession (lockstep ticks, Play mode).

namespace Engine {

	namespace {

		// Records pair events and joint breaks in the order they arrive.
		class JointListener final : public IPhysicsEventListener
		{
		public:
			void OnPhysicsEvent(const PhysicsEvent& event) override { Log.emplace_back(event); }
			void OnPhysicsJointBreak(const PhysicsJointBreak& jointBreak) override { Log.emplace_back(jointBreak); }

			[[nodiscard]] std::vector<PhysicsJointBreak> GetBreaks() const
			{
				std::vector<PhysicsJointBreak> breaks;
				for (const auto& entry : Log)
				{
					if (const PhysicsJointBreak* jointBreak = std::get_if<PhysicsJointBreak>(&entry))
						breaks.push_back(*jointBreak);
				}
				return breaks;
			}

			// Whether a collision began between `a` and `b`.
			[[nodiscard]] bool HasCollisionEnter(UUID a, UUID b) const
			{
				return std::any_of(Log.begin(), Log.end(), [a, b](const auto& entry)
				{
					const PhysicsEvent* event = std::get_if<PhysicsEvent>(&entry);
					return event != nullptr && event->Type == PhysicsEventType::CollisionEnter && event->Self == a && event->Other == b;
				});
			}

			std::vector<std::variant<PhysicsEvent, PhysicsJointBreak>> Log{};
		};

		Scope<PlaySession> StartSession(Test::SceneTestFixture& fixture, IPhysicsEventListener* listener = nullptr)
		{
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			if (listener != nullptr)
				(*session)->GetPhysics().SetEventListener(listener);
			return std::move(*session);
		}

		Entity Find(PlaySession& session, std::string_view path)
		{
			Entity entity = session.GetScene().FindEntityByPath(path);
			REQUIRE(entity.IsValid());
			return entity;
		}

		PhysicsJointReport GetJoint(PlaySession& session, std::string_view path)
		{
			const std::optional<PhysicsJointReport> report = session.GetPhysics().GetJointInfo(Find(session, path).GetUUID());
			REQUIRE(report.has_value());
			return *report;
		}

		// The world rotation angle of `path` about Y, in degrees, from its world rotation (a door turning about Y).
		float GetYawDegrees(PlaySession& session, std::string_view path)
		{
			const glm::quat rotation = TransformSystem::GetWorldRotation(Find(session, path));
			return glm::degrees(2.0f * std::atan2(rotation.y, rotation.w));
		}

		// A 1 m x 2 m door at x = 0.5 hinged about Y on its left edge (x = 0), to the world, with a position motor.
		Entity AddDoor(Scene& scene, float targetAngle)
		{
			Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(0.5f, 1.0f, 0.0f), glm::vec3(0.5f, 1.0f, 0.05f), BodyType::Dynamic);
			door.AddComponent<HingeJointComponent>(HingeJointComponent{ .Anchor = glm::vec3(-0.5f, 0.0f, 0.0f),
				.Axis = glm::vec3(0.0f, 1.0f, 0.0f),
				.UseLimits = true,
				.LowerAngle = -90.0f,
				.UpperAngle = 90.0f,
				.Motor = JointMotorMode::Position,
				.MotorTargetAngle = targetAngle,
				.MotorFrequency = 2.0f });
			return door;
		}

		// A 10 kg box at (1, 3, 0) welded to the world 1 m to its left.
		Entity AddWeldedLoad(Scene& scene, float breakForce)
		{
			Entity load = Test::AddBoxBody(scene, "Load", glm::vec3(1.0f, 3.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			Test::PatchRigidBody(load, [](RigidBodyComponent& body)
			{
				body.Mass = 10.0f;
			});
			load.AddComponent<FixedJointComponent>(FixedJointComponent{ .Anchor = glm::vec3(-1.0f, 0.0f, 0.0f), .BreakForce = breakForce });
			return load;
		}

		// The digest of the physics block alone (PhysicsSystem::AppendStateHash).
		uint64_t HashPhysics(const PlaySession& session)
		{
			XXH64Hasher hasher(0);
			session.GetPhysics().AppendStateHash(hasher);
			return hasher.Digest();
		}

		// A chain of three links on a hinge, a distance joint held at its length (a rod) and a weld, under a static beam: each joint
		// shares a body with the next, so the order Jolt solves them in shows in the result.
		void AddChain(Scene& scene)
		{
			Entity beam = Test::AddBoxBody(scene, "Beam", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(2.0f, 0.1f, 0.1f), BodyType::Static);
			Entity previous = beam;
			for (int link = 0; link < 3; ++link)
			{
				Entity entity = Test::AddBoxBody(scene, std::format("Link{}", link), glm::vec3(0.0f, 4.5f - 0.6f * static_cast<float>(link), 0.0f),
					glm::vec3(0.1f, 0.25f, 0.1f), BodyType::Dynamic);
				if (link == 0)
					entity.AddComponent<HingeJointComponent>(HingeJointComponent{ .ConnectedBody = previous.GetUUID(), .Anchor = glm::vec3(0.0f, 0.3f, 0.0f), .Axis = glm::vec3(0.0f, 0.0f, 1.0f) });
				else if (link == 1)
					entity.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = previous.GetUUID(),
						.Anchor = glm::vec3(0.0f, 0.25f, 0.0f),
						.ConnectedAnchor = glm::vec3(0.0f, -0.25f, 0.0f),
						.MinDistance = 0.1f,
						.MaxDistance = 0.1f });
				else
					entity.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = previous.GetUUID() });
				Test::PatchRigidBody(entity, [](RigidBodyComponent& body)
				{
					body.InitialLinearVelocity = glm::vec3(1.0f, 0.0f, 0.5f);
				});
				previous = entity;
			}
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsSystem: joints are made after their bodies, and a hinge's authored pose is its angle 0")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddDoor(fixture.GetScene(), 0.0f));
			Scope<PlaySession> session = StartSession(fixture);
			const PhysicsJointReport door = GetJoint(*session, "/Door");
			CHECK(door.Created);
			CHECK_FALSE(door.Broken);
			CHECK(door.Type == PhysicsJointType::Hinge);
			CHECK(door.Body == Find(*session, "/Door").GetUUID());
			CHECK_FALSE(door.Connected.IsValid());
			CHECK(door.Motor == PhysicsJointMotor::Position);
			CHECK(door.Angle == doctest::Approx(0.0).epsilon(1.0e-4));
			Test::RunTicks(*session, 30);
			// Gravity puts no torque on a door turning about Y, so it stays at its zero.
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(0.0).epsilon(1.0e-3));
			CHECK(Test::GetWorldPosition(*session, "/Door").y == doctest::Approx(1.0).epsilon(1.0e-3));
		}

		TEST_CASE("PhysicsJoints: a distance-joint pendulum swings at its analytic period")
		{
			// A small ball on a rigid 2 m rod (MinDistance = MaxDistance) from a static beam, pushed sideways at the bottom: the
			// rod holds the ball's centre, so it swings as a simple pendulum of period 2 pi sqrt(L / g), times 1 + theta^2 / 16 for
			// its amplitude of about 0.07 rad.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity beam = Test::AddBoxBody(scene, "Beam", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(1.0f, 0.1f, 0.1f), BodyType::Static);
			Entity bob = Test::AddSphereBody(scene, "Bob", glm::vec3(0.0f, 3.0f, 0.0f), 0.05f, BodyType::Dynamic);
			Test::PatchRigidBody(bob, [](RigidBodyComponent& body)
			{
				body.LinearDamping = 0.0f;
				body.AngularDamping = 0.0f;
				body.AllowSleeping = false;
				body.InitialLinearVelocity = glm::vec3(0.3f, 0.0f, 0.0f);
			});
			bob.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = beam.GetUUID(), .MinDistance = 2.0f, .MaxDistance = 2.0f });
			Scope<PlaySession> session = StartSession(fixture);

			// The times x crosses 0 going from positive to negative, interpolated within the tick.
			std::vector<double> crossings;
			float previous = Test::GetWorldPosition(*session, "/Bob").x;
			for (uint64_t tick = 1; tick <= 900 && crossings.size() < 3; ++tick)
			{
				Test::RunTicks(*session, 1);
				const float x = Test::GetWorldPosition(*session, "/Bob").x;
				if (previous > 0.0f && x <= 0.0f)
				{
					const double fraction = static_cast<double>(previous) / static_cast<double>(previous - x);
					crossings.push_back((static_cast<double>(tick) - 1.0 + fraction) / 60.0);
				}
				previous = x;
			}
			REQUIRE(crossings.size() == 3);
			const double amplitude = 0.3 / std::sqrt(9.81 * 2.0);
			const double expected = 2.0 * std::numbers::pi * std::sqrt(2.0 / 9.81) * (1.0 + amplitude * amplitude / 16.0);
			CHECK(crossings[1] - crossings[0] == doctest::Approx(expected).epsilon(0.01));
			CHECK(crossings[2] - crossings[1] == doctest::Approx(expected).epsilon(0.01));
			CHECK(GetJoint(*session, "/Bob").Distance == doctest::Approx(2.0).epsilon(0.005));
		}

		TEST_CASE("PhysicsJoints: a motored hinge reaches its target angle and stays within its limits")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddDoor(fixture.GetScene(), 45.0f));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 240);
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(45.0).epsilon(0.02));
			// A target beyond the limits drives it to the limit and no further, either way, and a push against the limit holds.
			float largest = 0.0f;
			float smallest = 0.0f;
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.LowerAngle = -30.0f;
				hinge.UpperAngle = 60.0f;
				hinge.MotorTargetAngle = 150.0f;
			});
			for (int tick = 0; tick < 240; ++tick)
			{
				Test::RunTicks(*session, 1);
				largest = std::max(largest, GetJoint(*session, "/Door").Angle);
			}
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(60.0).epsilon(0.02));
			CHECK(largest < 61.0f);
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.MotorTargetAngle = -150.0f;
			});
			for (int tick = 0; tick < 240; ++tick)
			{
				Test::RunTicks(*session, 1);
				smallest = std::min(smallest, GetJoint(*session, "/Door").Angle);
			}
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(-30.0).epsilon(0.02));
			CHECK(smallest > -31.0f);
			static_cast<void>(session->GetPhysics().AddAngularImpulse(Find(*session, "/Door").GetUUID(), glm::vec3(0.0f, -50.0f, 0.0f)));
			Test::RunTicks(*session, 30);
			CHECK(GetJoint(*session, "/Door").Angle > -31.0f);
		}

		TEST_CASE("PhysicsJoints: a velocity-motored slider stops at its limits")
		{
			Test::SceneTestFixture fixture;
			Entity piston = Test::AddBoxBody(fixture.GetScene(), "Piston", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f, 0.25f, 0.25f), BodyType::Dynamic);
			piston.AddComponent<SliderJointComponent>(SliderJointComponent{ .UseLimits = true,
				.LowerLimit = -0.5f,
				.UpperLimit = 1.5f,
				.Motor = JointMotorMode::Velocity,
				.MotorTargetVelocity = 2.0f,
				.MotorMaxForce = 500.0f });
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 10);
			CHECK(session->GetPhysics().GetLinearVelocity(Find(*session, "/Piston").GetUUID()).value_or(glm::vec3(0.0f)).x == doctest::Approx(2.0).epsilon(0.02));
			Test::RunTicks(*session, 110);
			CHECK(GetJoint(*session, "/Piston").Position == doctest::Approx(1.5).epsilon(0.01));
			CHECK(Test::GetWorldPosition(*session, "/Piston").x == doctest::Approx(1.5).epsilon(0.01));
			// Gravity never moves it off its rail.
			CHECK(Test::GetWorldPosition(*session, "/Piston").y == doctest::Approx(2.0).epsilon(0.001));
			Find(*session, "/Piston").Patch<SliderJointComponent>([](SliderJointComponent& slider)
			{
				slider.MotorTargetVelocity = -2.0f;
			});
			Test::RunTicks(*session, 120);
			CHECK(GetJoint(*session, "/Piston").Position == doctest::Approx(-0.5).epsilon(0.02));
			CHECK(Test::GetWorldPosition(*session, "/Piston").x == doctest::Approx(-0.5).epsilon(0.02));
		}

		TEST_CASE("PhysicsJoints: a joint keeps its zero and its limits when its body is created again")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddDoor(fixture.GetScene(), 45.0f));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 240);
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(45.0).epsilon(0.02));
			CHECK(GetYawDegrees(*session, "/Door") == doctest::Approx(45.0).epsilon(0.02));

			// A RigidBody change makes the body again: the joint comes back at its kept frames, still reading 45 degrees.
			Test::PatchRigidBody(Find(*session, "/Door"), [](RigidBodyComponent& body)
			{
				body.Mass = 2.0f;
			});
			Test::RunTicks(*session, 1);
			CHECK(GetJoint(*session, "/Door").Created);
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(45.0).epsilon(0.02));
			// Its zero is where it was authored: a target of 0 turns it back to its authored pose.
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.MotorTargetAngle = 0.0f;
			});
			Test::RunTicks(*session, 240);
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(0.0).epsilon(0.5));
			CHECK(std::abs(GetYawDegrees(*session, "/Door")) < 0.5f);
			// And its limits are where they were: a target beyond the upper limit (90 degrees from the authored pose) stops there.
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.MotorTargetAngle = 120.0f;
				hinge.UpperAngle = 60.0f;
			});
			Test::PatchRigidBody(Find(*session, "/Door"), [](RigidBodyComponent& body)
			{
				body.Mass = 3.0f;
			});
			Test::RunTicks(*session, 240);
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(60.0).epsilon(0.02));
			CHECK(GetYawDegrees(*session, "/Door") == doctest::Approx(60.0).epsilon(0.02));
		}

		TEST_CASE("PhysicsSystem: a new Anchor forms a joint's frames again from that moment's poses")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddDoor(fixture.GetScene(), 30.0f));
			Scope<PlaySession> session = StartSession(fixture);
			// Turned to 30 degrees, a new Anchor forms the frames again from that pose: it is the new zero, where the motor holds
			// it.
			Test::RunTicks(*session, 240);
			CHECK(GetYawDegrees(*session, "/Door") == doctest::Approx(30.0).epsilon(0.03));
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.Anchor = glm::vec3(-0.5f, 0.5f, 0.0f);
				hinge.MotorTargetAngle = 0.0f;
			});
			Test::RunTicks(*session, 120);
			CHECK(std::abs(GetJoint(*session, "/Door").Angle) < 0.5f);
			CHECK(GetYawDegrees(*session, "/Door") == doctest::Approx(30.0).epsilon(0.03));
		}

		TEST_CASE("PhysicsSystem: a motor target written every tick drives the live joint")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddDoor(fixture.GetScene(), 0.0f));
			Scope<PlaySession> session = StartSession(fixture);
			// A target that sweeps from 0 to 60 degrees over a second, written before every tick, as a script's OnFixedUpdate.
			for (int tick = 0; tick < 60; ++tick)
			{
				Find(*session, "/Door").Patch<HingeJointComponent>([tick](HingeJointComponent& hinge)
				{
					hinge.MotorTargetAngle = static_cast<float>(tick);
				});
				Test::RunTicks(*session, 1);
			}
			Test::RunTicks(*session, 180);
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(59.0).epsilon(0.02));
			// The motor's target velocity and mode change in place as well.
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.Motor = JointMotorMode::Velocity;
				hinge.MotorTargetVelocity = -30.0f;
			});
			Test::RunTicks(*session, 60);
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(29.0).epsilon(0.05));
		}

		TEST_CASE("PhysicsJoints: a weld breaks above its BreakForce and holds below it")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddWeldedLoad(fixture.GetScene(), 150.0f));
			Scope<PlaySession> session = StartSession(fixture);
			// The weld carries the 10 kg load's 98.1 N, below its 150 N (its first steps' peak included): it holds.
			Test::RunTicks(*session, 120);
			CHECK(session->GetPhysics().GetLastJointBreaks().empty());
			CHECK(GetJoint(*session, "/Load").Created);
			CHECK(GetJoint(*session, "/Load").Force == doctest::Approx(98.1).epsilon(0.02));
			CHECK_FALSE(Find(*session, "/Load").GetComponent<FixedJointComponent>().Broken);
			CHECK(Test::GetWorldPosition(*session, "/Load").y == doctest::Approx(3.0).epsilon(0.002));
			// At 50 N, below its load, it breaks at the next step, and the load falls.
			Find(*session, "/Load").Patch<FixedJointComponent>([](FixedJointComponent& weld)
			{
				weld.BreakForce = 50.0f;
			});
			Test::RunTicks(*session, 1);
			REQUIRE(session->GetPhysics().GetLastJointBreaks().size() == 1);
			CHECK(session->GetPhysics().GetLastJointBreaks()[0].Force == doctest::Approx(98.1).epsilon(0.02));
			CHECK(Find(*session, "/Load").GetComponent<FixedJointComponent>().Broken);
			Test::RunTicks(*session, 30);
			CHECK(Test::GetWorldPosition(*session, "/Load").y < 2.5f);
		}

		TEST_CASE("PhysicsSystem: a broken joint writes Broken, its break reaches the listener after the pair events, and Broken written false mends it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddWeldedLoad(scene, 150.0f));
			// A weld that breaks at the first step, and a trigger a ball starts inside, which enters at the same PostStep.
			Entity flimsy = Test::AddBoxBody(scene, "Flimsy", glm::vec3(10.0f, 3.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			flimsy.AddComponent<FixedJointComponent>(FixedJointComponent{ .BreakForce = 0.01f });
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(20.0f, 3.0f, 0.0f), glm::vec3(1.0f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& box)
			{
				box.IsTrigger = true;
			});
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(20.0f, 3.0f, 0.0f), 0.25f, BodyType::Dynamic));

			JointListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 1);
			const std::vector<PhysicsJointBreak> first = listener.GetBreaks();
			REQUIRE(first.size() == 1);
			CHECK(first[0].Joint == Find(*session, "/Flimsy").GetUUID());
			CHECK(first[0].Tick == 0);
			CHECK(first[0].Force > 0.01f);
			// The break is delivered after every pair event of its PostStep.
			REQUIRE(listener.Log.size() >= 2);
			CHECK(std::holds_alternative<PhysicsEvent>(listener.Log.front()));
			CHECK(std::holds_alternative<PhysicsJointBreak>(listener.Log.back()));
			CHECK(session->GetPhysics().GetLastJointBreaks().size() == 1);
			CHECK(Find(*session, "/Flimsy").GetComponent<FixedJointComponent>().Broken);
			CHECK_FALSE(GetJoint(*session, "/Flimsy").Created);
			CHECK(GetJoint(*session, "/Flimsy").Broken);

			// The load's weld carries about 98 N, below its 150 N: it holds.
			Test::RunTicks(*session, 60);
			CHECK(session->GetPhysics().GetLastJointBreaks().empty());
			CHECK(GetJoint(*session, "/Load").Created);
			CHECK(GetJoint(*session, "/Load").Force == doctest::Approx(98.1).epsilon(0.02));
			CHECK(Test::GetWorldPosition(*session, "/Load").y == doctest::Approx(3.0).epsilon(0.002));
			// At 50 N it breaks at the next step, and the load falls.
			Find(*session, "/Load").Patch<FixedJointComponent>([](FixedJointComponent& weld)
			{
				weld.BreakForce = 50.0f;
			});
			Test::RunTicks(*session, 1);
			const std::span<const PhysicsJointBreak> broke = session->GetPhysics().GetLastJointBreaks();
			REQUIRE(broke.size() == 1);
			CHECK(broke[0].Joint == Find(*session, "/Load").GetUUID());
			CHECK(broke[0].Force == doctest::Approx(98.1).epsilon(0.02));
			CHECK(broke[0].Tick == 61);
			CHECK(Find(*session, "/Load").GetComponent<FixedJointComponent>().Broken);
			CHECK(listener.GetBreaks().size() == 2);
			Test::RunTicks(*session, 10);
			const float fallen = Test::GetWorldPosition(*session, "/Load").y;
			CHECK(fallen < 2.9f);

			// Broken written false mends it at its kept frames: the solver pulls the load back to where it was welded.
			Find(*session, "/Load").Patch<FixedJointComponent>([](FixedJointComponent& weld)
			{
				weld.Broken = false;
				weld.BreakForce = 0.0f;
			});
			// Jolt's position correction closes the gap gradually (Docs/Backlog.md §6): it is less than half as wide 300 ticks later.
			Test::RunTicks(*session, 300);
			CHECK(GetJoint(*session, "/Load").Created);
			const glm::vec3 mended = Test::GetWorldPosition(*session, "/Load");
			CHECK(mended.y > fallen);
			CHECK(3.0f - mended.y < 0.5f * (3.0f - fallen));
			CHECK(mended.x == doctest::Approx(1.0).epsilon(0.01));
		}

		TEST_CASE("PhysicsSystem: a joint's two bodies collide only while CollideConnected is set, with no collision event between them before")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity a = Test::AddBoxBody(scene, "A", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Entity b = Test::AddBoxBody(scene, "B", glm::vec3(0.6f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			b.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = a.GetUUID(), .MinDistance = 0.0f, .MaxDistance = 10.0f });
			JointListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 30);
			const UUID first = Find(*session, "/A").GetUUID();
			const UUID second = Find(*session, "/B").GetUUID();
			CHECK_FALSE(listener.HasCollisionEnter(first, second));
			CHECK_FALSE(listener.HasCollisionEnter(second, first));
			// The boxes still overlap: nothing pushed them apart.
			CHECK(Test::GetWorldPosition(*session, "/B").x - Test::GetWorldPosition(*session, "/A").x < 0.7f);

			Find(*session, "/B").Patch<DistanceJointComponent>([](DistanceJointComponent& joint)
			{
				joint.CollideConnected = true;
			});
			Test::RunTicks(*session, 30);
			CHECK(listener.HasCollisionEnter(first, second));
			CHECK(Test::GetWorldPosition(*session, "/B").x - Test::GetWorldPosition(*session, "/A").x > 0.95f);
		}

		TEST_CASE("PhysicsJoints: every path that removes a jointed body removes its joints first")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity b = Test::AddBoxBody(scene, "B", glm::vec3(1.5f, 3.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			b.AddComponent<FixedJointComponent>();
			Entity a = Test::AddBoxBody(scene, "A", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			a.AddComponent<HingeJointComponent>(HingeJointComponent{ .ConnectedBody = b.GetUUID(), .Anchor = glm::vec3(0.75f, 0.0f, 0.0f), .Axis = glm::vec3(0.0f, 0.0f, 1.0f) });
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 10);
			REQUIRE(GetJoint(*session, "/A").Created);
			REQUIRE(GetJoint(*session, "/B").Created);

			SUBCASE("a RigidBody change makes the body and its joints again")
			{
				Test::PatchRigidBody(Find(*session, "/A"), [](RigidBodyComponent& body)
				{
					body.Mass = 2.0f;
				});
				Test::RunTicks(*session, 30);
				CHECK(GetJoint(*session, "/A").Created);
				// A swings about the hinge, 0.75 m from its centre, at B's left edge, wherever B's weld lets B sag.
				const std::optional<PhysicsBodyReport> holder = session->GetPhysics().GetBodyInfo(Find(*session, "/B").GetUUID());
				REQUIRE(holder.has_value());
				const glm::vec3 hinge = holder->Pose.Position + holder->Pose.Rotation * glm::vec3(-0.75f, 0.0f, 0.0f);
				CHECK(glm::length(Test::GetWorldPosition(*session, "/A") - hinge) == doctest::Approx(0.75).epsilon(0.02));
			}
			SUBCASE("a body refused after an edit loses its joints until it is accepted again")
			{
				const Test::ExpectLog refused(LogLevel::Error, "PHYSICS_ALL_DOFS_LOCKED");
				Test::PatchRigidBody(Find(*session, "/A"), [](RigidBodyComponent& body)
				{
					body.LockTranslation = glm::bvec3(true);
					body.LockRotation = glm::bvec3(true);
				});
				Test::RunTicks(*session, 30);
				CHECK_FALSE(GetJoint(*session, "/A").Created);
				CHECK(GetJoint(*session, "/B").Created);
				Test::PatchRigidBody(Find(*session, "/A"), [](RigidBodyComponent& body)
				{
					body.LockTranslation = glm::bvec3(false);
					body.LockRotation = glm::bvec3(false);
				});
				Test::RunTicks(*session, 30);
				CHECK(GetJoint(*session, "/A").Created);
			}
			SUBCASE("a disabled body's joints go at the flush and come back at their kept frames")
			{
				Find(*session, "/A").SetActive(false);
				Test::RunTicks(*session, 30);
				CHECK_FALSE(GetJoint(*session, "/A").Created);
				Find(*session, "/A").SetActive(true);
				Test::RunTicks(*session, 30);
				CHECK(GetJoint(*session, "/A").Created);
				// The connected end disabled takes the joint away too, without a diagnostic.
				Find(*session, "/B").SetActive(false);
				Test::RunTicks(*session, 30);
				CHECK_FALSE(GetJoint(*session, "/A").Created);
				CHECK_FALSE(GetJoint(*session, "/B").Created);
				Find(*session, "/B").SetActive(true);
				Test::RunTicks(*session, 30);
				CHECK(GetJoint(*session, "/A").Created);
				CHECK(GetJoint(*session, "/B").Created);
			}
			SUBCASE("a destroyed body's joints go at the flush")
			{
				const UUID id = Find(*session, "/A").GetUUID();
				session->GetScene().DestroyEntity(Find(*session, "/A"));
				Test::RunTicks(*session, 30);
				CHECK_FALSE(session->GetPhysics().GetJointInfo(id).has_value());
				CHECK(GetJoint(*session, "/B").Created);
				// A destroyed connected end leaves its joint naming no entity.
				Entity c = Test::AddBoxBody(session->GetScene(), "C", glm::vec3(-2.0f, 3.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
				c.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = Find(*session, "/B").GetUUID() });
				Test::RunTicks(*session, 10);
				CHECK(GetJoint(*session, "/C").Created);
				const Test::ExpectLog dangling(LogLevel::Error, "PHYSICS_JOINT_INVALID");
				session->GetScene().DestroyEntity(Find(*session, "/B"));
				Test::RunTicks(*session, 30);
				CHECK_FALSE(GetJoint(*session, "/C").Created);
			}
			SUBCASE("the session's end destroys the joints before the bodies")
			{
				// PhysicsWorld::DestroyBody destroys a body's joints first ("PhysicsWorld: destroying a body destroys its joints
				// first, and their handles go invalid"), and the session destroys every joint before its bodies: its end leaves
				// none of its three bodies and no world behind.
				const uint32_t bodies = PhysicsEngine::GetLiveBodyCount();
				const uint32_t worlds = PhysicsEngine::GetLiveWorldCount();
				REQUIRE(bodies >= 3);
				REQUIRE(worlds >= 1);
				session.reset();
				CHECK(PhysicsEngine::GetLiveBodyCount() == bodies - 3);
				CHECK(PhysicsEngine::GetLiveWorldCount() == worlds - 1);
			}
		}

		TEST_CASE("PhysicsSystem: a joint that cannot be made raises PHYSICS_JOINT_INVALID once, and its body simulates unjoined")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			door.AddComponent<HingeJointComponent>(HingeJointComponent{ .Axis = glm::vec3(0.0f) });
			Test::RecordingPhysicsListener listener;
			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_JOINT_INVALID");
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 30);
			const std::span<const PhysicsDiagnostic> diagnostics = session->GetPhysics().GetDiagnostics();
			const auto found = std::count_if(diagnostics.begin(), diagnostics.end(), [](const PhysicsDiagnostic& diagnostic)
			{
				return diagnostic.Code == PhysicsJointInvalidCode && diagnostic.Subject == "axis";
			});
			CHECK(found == 1);
			CHECK(invalid.GetMatchCount() == 1);
			CHECK_FALSE(GetJoint(*session, "/Door").Created);
			CHECK(Test::GetWorldPosition(*session, "/Door").y < 2.0f);
			// Mended by a valid axis, the joint is made where the body is now, and holds it.
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.Axis = glm::vec3(0.0f, 0.0f, 1.0f);
			});
			Test::RunTicks(*session, 1);
			const float y = Test::GetWorldPosition(*session, "/Door").y;
			Test::RunTicks(*session, 30);
			CHECK(GetJoint(*session, "/Door").Created);
			CHECK(Test::GetWorldPosition(*session, "/Door").y == doctest::Approx(y).epsilon(0.002));
		}

		TEST_CASE("PhysicsSystem: the state hash's joint part holds each joint's kept frames, and nothing without a joint")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Post", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.25f), BodyType::Static));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 1);
			const uint64_t without = HashPhysics(*session);
			// A static post welded to the world moves nothing: what the joint adds to the block is its kept frames.
			Find(*session, "/Post").AddComponent<FixedJointComponent>(FixedJointComponent{ .Anchor = glm::vec3(0.0f, 0.5f, 0.0f) });
			Test::RunTicks(*session, 1);
			REQUIRE(GetJoint(*session, "/Post").Created);
			const uint64_t with = HashPhysics(*session);
			CHECK(with != without);
			// Broken, the joint keeps its frames (it mends at them), so the part stays.
			Find(*session, "/Post").Patch<FixedJointComponent>([](FixedJointComponent& weld)
			{
				weld.Broken = true;
			});
			Test::RunTicks(*session, 1);
			CHECK_FALSE(GetJoint(*session, "/Post").Created);
			CHECK(HashPhysics(*session) == with);
			// Removed, its frames go.
			Find(*session, "/Post").RemoveComponent<FixedJointComponent>();
			Test::RunTicks(*session, 1);
			CHECK(HashPhysics(*session) == without);
		}

		TEST_CASE("PhysicsJoints: one state reached through two histories steps identically")
		{
			// History B removes the first link's joint and adds it back before the first step, so Jolt holds it last; the
			// joints' priorities (their entities' ranks in UUID order) keep the solve order, so both reach one state.
			const auto run = [](bool readd)
			{
				Test::SceneTestFixture fixture;
				AddChain(fixture.GetScene());
				Scope<PlaySession> session = StartSession(fixture);
				if (readd)
				{
					// The joint made first (the smallest entity UUID): Jolt moves the last joint into its place when it goes, and
					// holds it last when it comes back.
					Entity first = Find(*session, "/Link0");
					for (const std::string_view path : { "/Link1", "/Link2" })
					{
						if (Find(*session, path).GetUUID() < first.GetUUID())
							first = Find(*session, path);
					}
					if (first.HasComponent<HingeJointComponent>())
					{
						const HingeJointComponent hinge = first.GetComponent<HingeJointComponent>();
						first.RemoveComponent<HingeJointComponent>();
						session->GetPhysics().SynchronizeBodies();
						first.AddComponent<HingeJointComponent>(hinge);
					}
					else if (first.HasComponent<DistanceJointComponent>())
					{
						const DistanceJointComponent rod = first.GetComponent<DistanceJointComponent>();
						first.RemoveComponent<DistanceJointComponent>();
						session->GetPhysics().SynchronizeBodies();
						first.AddComponent<DistanceJointComponent>(rod);
					}
					else
					{
						const FixedJointComponent weld = first.GetComponent<FixedJointComponent>();
						first.RemoveComponent<FixedJointComponent>();
						session->GetPhysics().SynchronizeBodies();
						first.AddComponent<FixedJointComponent>(weld);
					}
				}
				Test::RunTicks(*session, 120);
				return session->ComputeStateHash();
			};
			const uint64_t once = run(false);
			const uint64_t twice = run(true);
			CHECK(once == twice);
		}

		TEST_CASE("PhysicsSystem: random joints and edits never reach a Jolt assert")
		{
			// Random joints of every type with every field drawn wild, between random bodies, and random edits each tick: the
			// world refuses what Jolt would assert on (a PHYSICS_JOINT_INVALID diagnostic), and nothing else happens.
			Random random(0x10147);
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			std::vector<Entity> bodies;
			for (int index = 0; index < 8; ++index)
			{
				const BodyType type = index % 4 == 0 ? BodyType::Static : (index % 4 == 1 ? BodyType::Kinematic : BodyType::Dynamic);
				bodies.push_back(Test::AddBoxBody(scene, std::format("Body{}", index), glm::vec3(static_cast<float>(index), 2.0f, 0.0f), glm::vec3(0.3f), type));
			}
			const auto pick = [&random](int64_t last)
			{
				return static_cast<int>(random.RangeInt(0, last));
			};
			const auto wild = [&random, &pick]()
			{
				const int kind = pick(5);
				if (kind == 0)
					return 0.0f;
				if (kind == 1)
					return static_cast<float>(-random.RangeDouble(0.0, 1.0e6));
				if (kind == 2)
					return static_cast<float>(random.RangeDouble(0.0, 1.0e12));
				return static_cast<float>(random.RangeDouble(-2.0, 2.0));
			};
			const auto connect = [&pick, &bodies]()
			{
				const int chosen = pick(static_cast<int64_t>(bodies.size()));
				return chosen == static_cast<int>(bodies.size()) ? UUID() : bodies[static_cast<size_t>(chosen)].GetUUID();
			};
			const auto addJoint = [&random, &pick, &wild, &connect](Entity entity)
			{
				switch (pick(3))
				{
					case 0:
						entity.AddComponent<HingeJointComponent>(HingeJointComponent{ .ConnectedBody = connect(),
							.Anchor = glm::vec3(wild(), wild(), wild()),
							.Axis = glm::vec3(wild(), wild(), wild()),
							.UseLimits = random.NextBool(),
							.LowerAngle = -std::abs(wild()),
							.UpperAngle = std::abs(wild()),
							.MaxFrictionTorque = wild(),
							.Motor = static_cast<JointMotorMode>(pick(2)),
							.MotorTargetVelocity = wild(),
							.MotorTargetAngle = wild(),
							.MotorMaxTorque = wild(),
							.MotorFrequency = wild(),
							.MotorDamping = wild(),
							.CollideConnected = random.NextBool(),
							.BreakForce = std::abs(wild()),
							.BreakTorque = std::abs(wild()) });
						break;
					case 1:
						entity.AddComponent<SliderJointComponent>(SliderJointComponent{ .ConnectedBody = connect(),
							.Anchor = glm::vec3(wild(), wild(), wild()),
							.Axis = glm::vec3(wild(), wild(), wild()),
							.UseLimits = random.NextBool(),
							.LowerLimit = -std::abs(wild()),
							.UpperLimit = std::abs(wild()),
							.MaxFrictionForce = wild(),
							.Motor = static_cast<JointMotorMode>(pick(2)),
							.MotorTargetVelocity = wild(),
							.MotorTargetPosition = wild(),
							.MotorMaxForce = wild(),
							.MotorFrequency = wild(),
							.MotorDamping = wild() });
						break;
					case 2:
						entity.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = connect(),
							.Anchor = glm::vec3(wild(), wild(), wild()),
							.ConnectedAnchor = glm::vec3(wild(), wild(), wild()),
							.MinDistance = wild(),
							.MaxDistance = wild(),
							.Frequency = wild(),
							.Damping = wild(),
							.BreakForce = std::abs(wild()) });
						break;
					default:
						entity.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = connect(),
							.Anchor = glm::vec3(wild(), wild(), wild()),
							.BreakForce = std::abs(wild()),
							.BreakTorque = std::abs(wild()) });
						break;
				}
			};
			const auto removeJoints = [](Entity entity)
			{
				if (entity.HasComponent<HingeJointComponent>())
					entity.RemoveComponent<HingeJointComponent>();
				if (entity.HasComponent<SliderJointComponent>())
					entity.RemoveComponent<SliderJointComponent>();
				if (entity.HasComponent<DistanceJointComponent>())
					entity.RemoveComponent<DistanceJointComponent>();
				if (entity.HasComponent<FixedJointComponent>())
					entity.RemoveComponent<FixedJointComponent>();
			};
			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_JOINT_INVALID");
			const Test::ExpectLog locked(LogLevel::Error, "PHYSICS_");
			Scope<PlaySession> session = StartSession(fixture);
			Scene& runtime = session->GetScene();
			// Added in the runtime scene directly, past the registry's ranges, as only the world's own checks stand there.
			for (const Entity body : bodies)
				addJoint(runtime.FindEntityByID(body.GetUUID()));
			for (int tick = 0; tick < 120; ++tick)
			{
				const std::vector<UUID> order(runtime.GetCanonicalOrder().begin(), runtime.GetCanonicalOrder().end());
				Entity entity = runtime.FindEntityByID(order[static_cast<size_t>(pick(static_cast<int64_t>(order.size()) - 1))]);
				if (entity.IsValid() && entity.HasComponent<RigidBodyComponent>())
				{
					switch (pick(4))
					{
						case 0:
							removeJoints(entity);
							addJoint(entity);
							break;
						case 1:
							removeJoints(entity);
							break;
						case 2:
							entity.SetActive(!entity.IsActiveSelf());
							break;
						case 3:
							Test::PatchRigidBody(entity, [&pick](RigidBodyComponent& body)
							{
								body.Type = static_cast<BodyType>(pick(2));
								body.LockRotation = glm::bvec3(pick(3) == 0);
								body.LockTranslation = body.LockRotation;
							});
							break;
						default:
							if (tick % 40 == 39)
								runtime.DestroyEntity(entity);
							break;
					}
				}
				Test::RunTicks(*session, 1);
			}
			CHECK(session->GetTick() == 120);
		}

		TEST_CASE("PhysicsSystem: a DistanceJoint's new Anchor or ConnectedAnchor applies at the next synchronization")
		{
			// The W37.1 review's finding 1: a distance joint keeps no frames, so a new anchor must make it again.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity post = Test::AddBoxBody(scene, "Post", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.1f), BodyType::Static);
			Entity bob = Test::AddSphereBody(scene, "Bob", glm::vec3(0.0f, 3.0f, 0.0f), 0.1f, BodyType::Dynamic);
			bob.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = post.GetUUID(), .MinDistance = 0.0f, .MaxDistance = 2.0f });
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 30);
			CHECK(Test::GetWorldPosition(*session, "/Bob").y == doctest::Approx(3.0).epsilon(0.005));

			// The rope's top 1 m lower on the post: the bob falls 1 m to the rope's end.
			Find(*session, "/Bob").Patch<DistanceJointComponent>([](DistanceJointComponent& rope)
			{
				rope.ConnectedAnchor = glm::vec3(0.0f, -1.0f, 0.0f);
			});
			Test::RunTicks(*session, 90);
			const PhysicsJointReport lowered = GetJoint(*session, "/Bob");
			CHECK(lowered.Created);
			CHECK(glm::length(lowered.ConnectedPoint - glm::vec3(0.0f, 4.0f, 0.0f)) < 1.0e-3f);
			CHECK(Test::GetWorldPosition(*session, "/Bob").y == doctest::Approx(2.0).epsilon(0.01));

			// The rope tied 0.5 m above the bob's centre: it hangs 0.5 m lower.
			Find(*session, "/Bob").Patch<DistanceJointComponent>([](DistanceJointComponent& rope)
			{
				rope.Anchor = glm::vec3(0.0f, 0.5f, 0.0f);
			});
			Test::RunTicks(*session, 90);
			const PhysicsJointReport tied = GetJoint(*session, "/Bob");
			CHECK(tied.Created);
			CHECK(Test::GetWorldPosition(*session, "/Bob").y == doctest::Approx(1.5).epsilon(0.01));
			CHECK(glm::length(tied.Point - (Test::GetWorldPosition(*session, "/Bob") + glm::vec3(0.0f, 0.5f, 0.0f))) < 0.01f);
		}

		TEST_CASE("PhysicsSystem: a joint whose connected end leaves play keeps it until the flush, and raises its reason after it")
		{
			// The W37.1 review's finding 2, stepped by hand so that the joint can be read between PostStep and the flush: a
			// box welded to a static post that is disabled during a tick, and one welded to a mast that is destroyed while
			// another change recomposes the scene in the same tick.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity post = Test::AddBoxBody(scene, "Post", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.1f), BodyType::Static);
			Entity a = Test::AddBoxBody(scene, "A", glm::vec3(1.0f, 5.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			a.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = post.GetUUID() });
			Entity mast = Test::AddBoxBody(scene, "Mast", glm::vec3(10.0f, 5.0f, 0.0f), glm::vec3(0.1f), BodyType::Static);
			Entity c = Test::AddBoxBody(scene, "C", glm::vec3(11.0f, 5.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			c.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = mast.GetUUID() });
			Entity other = Test::AddBoxBody(scene, "Other", glm::vec3(20.0f, 5.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			const UUID aId = a.GetUUID();
			const UUID cId = c.GetUUID();
			TransformSystem::Update(scene);
			PhysicsSystemSpecification specification;
			specification.RuntimeScene = &scene;
			Result<Scope<PhysicsSystem>> created = PhysicsSystem::Create(specification);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PhysicsSystem& physics = **created;
			uint64_t tick = 0;
			const auto step = [&scene, &physics, &tick]()
			{
				const SimStep simStep = SimStep::FromTick(tick, 1.0 / 60.0);
				TransformSystem::Update(scene);
				physics.PreStep(simStep);
				physics.Step(simStep);
				physics.PostStep(simStep);
			};
			const auto flush = [&scene, &physics, &tick]()
			{
				physics.FlushDestroyed(tick);
				scene.FlushPendingDestroys();
				++tick;
			};
			const auto isInForce = [&physics](UUID joint)
			{
				const std::optional<PhysicsJointReport> report = physics.GetJointInfo(joint);
				return report.has_value() && report->Created;
			};
			const auto countReasons = [&physics, cId]()
			{
				const std::span<const PhysicsDiagnostic> diagnostics = physics.GetDiagnostics();
				return std::count_if(diagnostics.begin(), diagnostics.end(), [cId](const PhysicsDiagnostic& diagnostic)
				{
					return diagnostic.Code == PhysicsJointInvalidCode && diagnostic.Entity == cId;
				});
			};
			for (int index = 0; index < 5; ++index)
			{
				step();
				flush();
			}
			REQUIRE(isInForce(aId));
			REQUIRE(isInForce(cId));

			// Disabled during a tick, as a script in the fixed step would: the joint holds through that tick's step, as the
			// post's body does, and goes at the flush.
			scene.FindEntityByID(post.GetUUID()).SetActive(false);
			step();
			CHECK(isInForce(aId));
			flush();
			step();
			CHECK_FALSE(isInForce(aId));
			flush();

			// Destroyed while another change recomposes the scene: the same, and its reason ("connected": it names no entity)
			// is raised only after the flush.
			const Test::ExpectLog dangling(LogLevel::Error, "PHYSICS_JOINT_INVALID");
			scene.DestroyEntity(scene.FindEntityByID(mast.GetUUID()));
			Test::PatchRigidBody(scene.FindEntityByID(other.GetUUID()), [](RigidBodyComponent& body)
			{
				body.Mass = 3.0f;
			});
			step();
			CHECK(isInForce(cId));
			CHECK(countReasons() == 0);
			flush();
			step();
			CHECK_FALSE(isInForce(cId));
			CHECK(countReasons() == 1);
			flush();
			CHECK(dangling.GetMatchCount() == 1);
		}

		TEST_CASE("PhysicsSystem: a new ConnectedBody forms a hinge's frames again from that moment's poses")
		{
			// The W37.1 review's finding 4 (a): the door turned to 30 degrees on the world, then hinged to a static post: its
			// zero is the pose of that moment.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddDoor(scene, 30.0f));
			static_cast<void>(Test::AddBoxBody(scene, "Post", glm::vec3(-1.0f, 1.0f, 0.0f), glm::vec3(0.1f), BodyType::Static));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 240);
			CHECK(GetJoint(*session, "/Door").Angle == doctest::Approx(30.0).epsilon(0.03));
			const UUID post = Find(*session, "/Post").GetUUID();
			Find(*session, "/Door").Patch<HingeJointComponent>([post](HingeJointComponent& hinge)
			{
				hinge.ConnectedBody = post;
				hinge.MotorTargetAngle = 0.0f;
			});
			Test::RunTicks(*session, 120);
			const PhysicsJointReport door = GetJoint(*session, "/Door");
			CHECK(door.Created);
			CHECK(door.Connected == post);
			CHECK(std::abs(door.Angle) < 0.5f);
			CHECK(GetYawDegrees(*session, "/Door") == doctest::Approx(30.0).epsilon(0.03));
		}

		TEST_CASE("PhysicsSystem: a joint on a child of its body keeps its frames on the body, and a reparent forms them again")
		{
			// The W37.1 review's finding 4 (b) and (c): the hinge on "Pivot", a child of the door's body at its left edge.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(0.5f, 1.0f, 0.0f), glm::vec3(0.5f, 1.0f, 0.05f), BodyType::Dynamic);
			Entity pivot = scene.CreateEntity("Pivot");
			REQUIRE(scene.SetParent(pivot, door).has_value());
			pivot.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(-0.5f, 0.0f, 0.0f);
			});
			pivot.AddComponent<HingeJointComponent>(HingeJointComponent{ .Axis = glm::vec3(0.0f, 1.0f, 0.0f),
				.Motor = JointMotorMode::Position,
				.MotorTargetAngle = 30.0f,
				.MotorFrequency = 2.0f });
			Entity mount = scene.CreateEntity("Mount");
			REQUIRE(scene.SetParent(mount, door).has_value());
			Scope<PlaySession> session = StartSession(fixture);
			const PhysicsJointReport made = GetJoint(*session, "/Door/Pivot");
			CHECK(made.Created);
			CHECK(made.Body == Find(*session, "/Door").GetUUID());
			CHECK(glm::length(made.Point - glm::vec3(0.0f, 1.0f, 0.0f)) < 1.0e-3f);
			Test::RunTicks(*session, 240);
			CHECK(GetJoint(*session, "/Door/Pivot").Angle == doctest::Approx(30.0).epsilon(0.03));

			// The body created again (a RigidBody change) gets the joint back at its kept frames: the angle stays.
			Test::PatchRigidBody(Find(*session, "/Door"), [](RigidBodyComponent& body)
			{
				body.Mass = 2.0f;
			});
			Test::RunTicks(*session, 30);
			CHECK(GetJoint(*session, "/Door/Pivot").Angle == doctest::Approx(30.0).epsilon(0.03));

			// Moved under "Mount", keeping its world place, the joint entity's chain to its body changed: the frames are formed
			// again, and the pose of that moment is the new zero.
			Find(*session, "/Door/Pivot").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.MotorTargetAngle = 0.0f;
			});
			REQUIRE(session->GetScene().SetParent(Find(*session, "/Door/Pivot"), Find(*session, "/Door/Mount")).has_value());
			Test::RunTicks(*session, 120);
			CHECK(std::abs(GetJoint(*session, "/Door/Mount/Pivot").Angle) < 0.5f);
			CHECK(GetYawDegrees(*session, "/Door") == doctest::Approx(30.0).epsilon(0.03));
		}

		TEST_CASE("PhysicsSystem: an axis the entity's scale makes vanish raises Subject frame, and values the world refuses Subject refused")
		{
			// The W37.1 review's finding 4 (d) and (e).
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// (d) Axis (1e-5, 0, 0) on a child scaled 0.01: long enough for the composition, but 1e-7 in the world.
			Entity body = Test::AddBoxBody(scene, "Body", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			Entity tiny = scene.CreateEntity("Tiny");
			REQUIRE(scene.SetParent(tiny, body).has_value());
			tiny.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(0.01f);
			});
			tiny.AddComponent<HingeJointComponent>(HingeJointComponent{ .Axis = glm::vec3(1.0e-5f, 0.0f, 0.0f) });
			// (e) An anchor past MaxPhysicsCoordinate in the world, which the registry's range allows on the entity.
			Entity far = Test::AddBoxBody(scene, "Far", glm::vec3(1000.0f, 3.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			far.AddComponent<FixedJointComponent>(FixedJointComponent{ .Anchor = glm::vec3(MaxPhysicsCoordinate, 0.0f, 0.0f) });
			// (e) A hinge whose settings a write past the registry's range makes the world refuse in place.
			static_cast<void>(AddDoor(scene, 0.0f));
			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_JOINT_INVALID");
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 2);
			CHECK(GetJoint(*session, "/Door").Created);
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.MotorFrequency = 0.0f;
			});
			Test::RunTicks(*session, 2);

			const auto subjectsOf = [&session](std::string_view path)
			{
				const UUID id = Find(*session, path).GetUUID();
				std::vector<std::string> subjects;
				for (const PhysicsDiagnostic& diagnostic : session->GetPhysics().GetDiagnostics())
				{
					if (diagnostic.Code == PhysicsJointInvalidCode && diagnostic.Entity == id)
						subjects.push_back(diagnostic.Subject);
				}
				return subjects;
			};
			CHECK(subjectsOf("/Body/Tiny") == std::vector<std::string>{ "frame" });
			CHECK(subjectsOf("/Far") == std::vector<std::string>{ "refused" });
			CHECK(subjectsOf("/Door") == std::vector<std::string>{ "refused" });
			CHECK_FALSE(GetJoint(*session, "/Body/Tiny").Created);
			CHECK_FALSE(GetJoint(*session, "/Far").Created);
			CHECK_FALSE(GetJoint(*session, "/Door").Created);
			CHECK(invalid.GetMatchCount() == 3);
		}

		TEST_CASE("PhysicsSystem: a joint the world refused is made from the poses of the moment it is fixed")
		{
			// The W37.1 review's finding 12: a refused creation keeps no frames. The door, authored broken, falls unjoined; mended
			// with a motor frequency written past the registry's range, its creation is refused where it is then; it falls on;
			// fixed, the joint is made where the door is at that moment and holds it there, rather than pulling it back to
			// where the refused creation would have put it.
			Test::SceneTestFixture fixture;
			Entity door = AddDoor(fixture.GetScene(), 0.0f);
			door.Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.Broken = true;
			});
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 20);
			const Test::ExpectLog refused(LogLevel::Error, "PHYSICS_JOINT_INVALID");
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.Broken = false;
				hinge.MotorFrequency = 0.0f;
			});
			Test::RunTicks(*session, 20);
			CHECK_FALSE(GetJoint(*session, "/Door").Created);
			CHECK(refused.GetMatchCount() == 1);
			Find(*session, "/Door").Patch<HingeJointComponent>([](HingeJointComponent& hinge)
			{
				hinge.MotorFrequency = 2.0f;
			});
			Test::RunTicks(*session, 1);
			const float held = Test::GetWorldPosition(*session, "/Door").y;
			Test::RunTicks(*session, 30);
			CHECK(GetJoint(*session, "/Door").Created);
			CHECK(Test::GetWorldPosition(*session, "/Door").y == doctest::Approx(held).epsilon(0.002));
			CHECK(refused.GetMatchCount() == 1);
		}

		TEST_CASE("PhysicsSystem: a DistanceJoint is made whatever its entity's scale")
		{
			// The W37.1 review's finding 10: a distance joint has no axis, so two nested X scales of 1e-4 (1e-8 in the world)
			// refuse nothing.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity bob = Test::AddSphereBody(scene, "Bob", glm::vec3(0.0f, 3.0f, 0.0f), 0.1f, BodyType::Dynamic);
			Entity thin = scene.CreateEntity("Thin");
			REQUIRE(scene.SetParent(thin, bob, {}, false).has_value());
			Entity thinner = scene.CreateEntity("Thinner");
			REQUIRE(scene.SetParent(thinner, thin, {}, false).has_value());
			for (Entity scaled : { thin, thinner })
			{
				scaled.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Scale = glm::vec3(1.0e-4f, 1.0f, 1.0f);
				});
			}
			thinner.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedAnchor = glm::vec3(0.0f, 5.0f, 0.0f), .MaxDistance = 2.0f });
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 30);
			CHECK(GetJoint(*session, "/Bob/Thin/Thinner").Created);
			CHECK(session->GetPhysics().GetDiagnostics().empty());
			CHECK(Test::GetWorldPosition(*session, "/Bob").y == doctest::Approx(3.0).epsilon(0.005));
		}

		TEST_CASE("PhysicsSystem: Broken written true removes the joint without a break event")
		{
			// The W37.1 review's finding 4 (f).
			Test::SceneTestFixture fixture;
			static_cast<void>(AddWeldedLoad(fixture.GetScene(), 1000.0f));
			JointListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 10);
			REQUIRE(GetJoint(*session, "/Load").Created);
			Find(*session, "/Load").Patch<FixedJointComponent>([](FixedJointComponent& weld)
			{
				weld.Broken = true;
			});
			Test::RunTicks(*session, 30);
			CHECK_FALSE(GetJoint(*session, "/Load").Created);
			CHECK(GetJoint(*session, "/Load").Broken);
			CHECK(listener.GetBreaks().empty());
			CHECK(session->GetPhysics().GetLastJointBreaks().empty());
			// Unjoined, it falls.
			CHECK(Test::GetWorldPosition(*session, "/Load").y < 2.5f);
		}
	}

}
