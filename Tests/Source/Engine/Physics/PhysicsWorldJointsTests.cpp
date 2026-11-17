#include "TestsPCH.h"

#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Core/Hash.h"
#include "Engine/Physics/PhysicsJoint.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

// The physics world's joints (M37, Docs/Decisions/0054-m37-joints.md decision 2; Architecture §9.10): each type against an
// analytic case (a pendulum's period, a motor reaching its target, limits holding, a weld reading its load), what CreateJoint
// refuses before Jolt sees it, a body's joints destroyed with it, SetShape moving a joint's centre-of-mass offset,
// CollideConnected in both of Jolt's collision passes, and one state reached through two creation orders. ECS-agnostic:
// bodies and joints are created directly.

namespace Engine {

	namespace {

		constexpr float FixedDelta = 1.0f / 60.0f;
		constexpr float Gravity = 9.81f;

		Scope<PhysicsWorld> CreateWorld()
		{
			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			return std::move(*world);
		}

		Ref<const PhysicsShape> CreateShape(const PhysicsShapeGeometry& geometry, const glm::vec3& offset = glm::vec3(0.0f))
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = geometry, .Position = offset } } });
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			return std::move(*shape);
		}

		// A body of `geometry` at `position` without damping (so analytic cases hold), of `type`.
		BodyHandle CreateBody(PhysicsWorld& world, const PhysicsShapeGeometry& geometry, const glm::vec3& position, PhysicsMotionType type = PhysicsMotionType::Dynamic,
			float mass = 1.0f, PhysicsMotionQuality quality = PhysicsMotionQuality::Discrete)
		{
			BodyDescription description;
			description.Shape = CreateShape(geometry);
			description.MotionType = type;
			description.Pose.Position = position;
			description.Mass = mass;
			description.LinearDamping = 0.0f;
			description.AngularDamping = 0.0f;
			description.AllowSleeping = false;
			description.MotionQuality = quality;
			Result<BodyHandle> body = world.CreateBody(description);
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			return *body;
		}

		// A frame at `point` with `axis` and a normal perpendicular to it.
		PhysicsJointFrame MakeFrame(const glm::vec3& point, const glm::vec3& axis = glm::vec3(1.0f, 0.0f, 0.0f), const glm::vec3& normal = glm::vec3(0.0f, 1.0f, 0.0f))
		{
			return PhysicsJointFrame{ .Point = point, .Axis = axis, .Normal = normal };
		}

		JointHandle CreateJoint(PhysicsWorld& world, const PhysicsJointDescription& description)
		{
			Result<JointHandle> joint = world.CreateJoint(description);
			REQUIRE_MESSAGE(joint.has_value(), joint.error().ToString());
			return *joint;
		}

		void StepWorld(PhysicsWorld& world, uint32_t steps)
		{
			for (uint32_t step = 0; step < steps; ++step)
				REQUIRE(world.Step(FixedDelta, 1).has_value());
		}

		// Whether `contacts` holds a record between `a` and `b`, in either order.
		bool HasContact(const std::vector<ContactEvent>& contacts, BodyHandle a, BodyHandle b)
		{
			return std::any_of(contacts.begin(), contacts.end(), [a, b](const ContactEvent& event)
			{
				return (event.BodyA == a && event.BodyB == b) || (event.BodyA == b && event.BodyB == a);
			});
		}

		// XXH64 of each body's pose and velocities (float bit patterns), in the order given.
		uint64_t HashBodies(const PhysicsWorld& world, const std::vector<BodyHandle>& bodies)
		{
			XXH64Hasher hasher(0);
			for (const BodyHandle body : bodies)
			{
				const PhysicsPose pose = world.GetPose(body);
				for (const float value : { pose.Position.x, pose.Position.y, pose.Position.z, pose.Rotation.x, pose.Rotation.y, pose.Rotation.z, pose.Rotation.w })
					hasher.UpdateU64(std::bit_cast<uint32_t>(value));
				const glm::vec3 linear = world.GetLinearVelocity(body);
				const glm::vec3 angular = world.GetAngularVelocity(body);
				for (const float value : { linear.x, linear.y, linear.z, angular.x, angular.y, angular.z })
					hasher.UpdateU64(std::bit_cast<uint32_t>(value));
			}
			return hasher.Digest();
		}

		// A chain of four boxes hanging from a static beam on hinges, distance joints and a weld, its joints created in
		// `forward` order or in reverse, each with the priority of its place in the forward order; the bodies' hash after
		// 120 steps.
		uint64_t RunChain(bool forward)
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle beam = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(2.0f, 0.1f, 0.1f) }, glm::vec3(0.0f, 5.0f, 0.0f),
				PhysicsMotionType::Static);
			std::vector<BodyHandle> links;
			for (int link = 0; link < 4; ++link)
			{
				links.push_back(CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.1f, 0.25f, 0.1f) },
					glm::vec3(0.0f, 4.5f - 0.6f * static_cast<float>(link), 0.0f)));
			}
			std::vector<PhysicsJointDescription> joints;
			joints.push_back(PhysicsJointDescription{ .Type = PhysicsJointType::Hinge,
				.Body = links[0],
				.ConnectedBody = beam,
				.Frame = MakeFrame(glm::vec3(0.0f, 0.25f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
				.ConnectedFrame = MakeFrame(glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f)) });
			// Links 1 and 2 on distance joints held at their 0.1 m (rods, always in force, so each joint shares a body with the next
			// and the solve order shows in the result), link 3 welded where it hangs.
			for (size_t link = 1; link < links.size(); ++link)
			{
				const bool weld = link == 3;
				joints.push_back(PhysicsJointDescription{ .Type = weld ? PhysicsJointType::Fixed : PhysicsJointType::Distance,
					.Body = links[link],
					.ConnectedBody = links[link - 1],
					.Frame = MakeFrame(glm::vec3(0.0f, weld ? 0.3f : 0.25f, 0.0f)),
					.ConnectedFrame = MakeFrame(glm::vec3(0.0f, weld ? -0.3f : -0.25f, 0.0f)),
					.Settings = PhysicsJointSettings{ .LowerLimit = 0.1f, .UpperLimit = 0.1f } });
			}
			for (size_t index = 0; index < joints.size(); ++index)
				joints[index].Priority = static_cast<uint32_t>(index);
			if (!forward)
				std::reverse(joints.begin(), joints.end());
			for (const PhysicsJointDescription& joint : joints)
				static_cast<void>(CreateJoint(*world, joint));
			// A kick, so the chain swings and every joint works.
			world->SetLinearVelocity(links[3], glm::vec3(2.0f, 0.0f, 1.0f));
			StepWorld(*world, 120);
			return HashBodies(*world, links);
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsWorld: a hinge pendulum swings with the period of the analytic pendulum")
		{
			// A 0.05 m sphere 1 m below a hinge to the world about Z, pushed sideways at the bottom: a physical pendulum of
			// period 2 pi sqrt(I / (m g L)), I = 2/5 m r^2 + m L^2, times 1 + theta^2 / 16 for its amplitude of about 0.1 rad.
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle bob = CreateBody(*world, SphereShapeGeometry{ .Radius = 0.05f }, glm::vec3(0.0f, -1.0f, 0.0f));
			static_cast<void>(CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Hinge, .Body = bob, .Frame = MakeFrame(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f)) }));
			world->SetLinearVelocity(bob, glm::vec3(0.3f, 0.0f, 0.0f));

			// The times x crosses 0 going from positive to negative, interpolated within the step.
			std::vector<double> crossings;
			float previous = world->GetPose(bob).Position.x;
			for (uint32_t step = 1; step <= 600 && crossings.size() < 3; ++step)
			{
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				const float x = world->GetPose(bob).Position.x;
				if (previous > 0.0f && x <= 0.0f)
				{
					const double fraction = static_cast<double>(previous) / static_cast<double>(previous - x);
					crossings.push_back((static_cast<double>(step) - 1.0 + fraction) * static_cast<double>(FixedDelta));
				}
				previous = x;
			}
			REQUIRE(crossings.size() == 3);
			const double inertiaOverMassLength = (0.4 * 0.05 * 0.05 + 1.0) / 1.0;
			const double expected = 2.0 * std::numbers::pi * std::sqrt(inertiaOverMassLength / static_cast<double>(Gravity)) * (1.0 + 0.0096 / 16.0);
			CHECK(crossings[1] - crossings[0] == doctest::Approx(expected).epsilon(0.01));
			CHECK(crossings[2] - crossings[1] == doctest::Approx(expected).epsilon(0.01));
			// The bob stays 1 m from the pivot.
			CHECK(glm::length(world->GetPose(bob).Position) == doctest::Approx(1.0).epsilon(0.005));
		}

		TEST_CASE("PhysicsWorld: a hinge's velocity motor reaches its target velocity and its position motor its target angle")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			// A box turning about the vertical axis through its centre, so gravity puts no torque on it.
			const BodyHandle wheel = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f, 0.1f, 0.1f) }, glm::vec3(0.0f, 2.0f, 0.0f));
			const JointHandle hinge = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Hinge, .Body = wheel, .Frame = MakeFrame(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)), .Settings = PhysicsJointSettings{ .Motor = PhysicsJointMotor::Velocity, .MotorTargetVelocity = 1.5f, .MotorMaxForce = 1000.0f } });
			StepWorld(*world, 30);
			CHECK(world->GetAngularVelocity(wheel).y == doctest::Approx(1.5).epsilon(0.01));
			CHECK(world->GetPose(wheel).Position.y == doctest::Approx(2.0).epsilon(0.001));

			// In place: a position drive to 0.5 rad, from wherever it turned to.
			REQUIRE(world->SetJointSettings(hinge, PhysicsJointSettings{ .Motor = PhysicsJointMotor::Position, .MotorTargetPosition = 0.5f }).has_value());
			StepWorld(*world, 240);
			const PhysicsJointState reading = world->GetJointState(hinge);
			CHECK(reading.Value == doctest::Approx(0.5).epsilon(0.01));
			CHECK(std::abs(world->GetAngularVelocity(wheel).y) < 0.05f);
			// Where the joint is: both points at the pivot, the axis vertical as the world holds it.
			CHECK(glm::length(reading.Point - glm::vec3(0.0f, 2.0f, 0.0f)) < 0.002f);
			CHECK(glm::length(reading.ConnectedPoint - glm::vec3(0.0f, 2.0f, 0.0f)) < 1.0e-5f);
			CHECK(reading.Axis.y == doctest::Approx(1.0).epsilon(1.0e-5));
		}

		TEST_CASE("PhysicsWorld: a hinge's limits hold its angle and a motor's target is clamped to them")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			// A bar hinged about Z at its left end: gravity turns its free end down (a negative angle) until the lower limit.
			const BodyHandle bar = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(1.0f, 0.05f, 0.05f) }, glm::vec3(1.0f, 3.0f, 0.0f));
			const JointHandle hinge = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Hinge, .Body = bar, .Frame = MakeFrame(glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f)), .Settings = PhysicsJointSettings{ .UseLimits = true, .LowerLimit = -0.3f, .UpperLimit = 0.4f } });
			StepWorld(*world, 180);
			CHECK(world->GetJointState(hinge).Value == doctest::Approx(-0.3).epsilon(0.03));

			// A position target beyond the upper limit drives it to the limit.
			REQUIRE(world->SetJointSettings(hinge,
							 PhysicsJointSettings{ .UseLimits = true,
								 .LowerLimit = -0.3f,
								 .UpperLimit = 0.4f,
								 .Motor = PhysicsJointMotor::Position,
								 .MotorTargetPosition = 1.2f,
								 .MotorMaxForce = 10000.0f,
								 .MotorFrequency = 8.0f })
					.has_value());
			StepWorld(*world, 240);
			CHECK(world->GetJointState(hinge).Value == doctest::Approx(0.4).epsilon(0.03));
		}

		TEST_CASE("PhysicsWorld: a slider moves its body only along its axis, driven by its motor until its limit")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle carriage = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, glm::vec3(0.0f, 2.0f, 0.0f));
			const JointHandle slider = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Slider, .Body = carriage, .Frame = MakeFrame(glm::vec3(0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f, 2.0f, 0.0f)), .Settings = PhysicsJointSettings{ .UseLimits = true, .LowerLimit = -1.0f, .UpperLimit = 0.5f, .Motor = PhysicsJointMotor::Velocity, .MotorTargetVelocity = 2.0f, .MotorMaxForce = 1000.0f } });
			StepWorld(*world, 6);
			CHECK(world->GetLinearVelocity(carriage).x == doctest::Approx(2.0).epsilon(0.02));
			StepWorld(*world, 114);
			const PhysicsPose pose = world->GetPose(carriage);
			CHECK(world->GetJointState(slider).Value == doctest::Approx(0.5).epsilon(0.02));
			CHECK(pose.Position.x == doctest::Approx(0.5).epsilon(0.02));
			// Gravity never moves it off the rail, and it never turns.
			CHECK(pose.Position.y == doctest::Approx(2.0).epsilon(0.001));
			CHECK(std::abs(pose.Rotation.w) == doctest::Approx(1.0).epsilon(0.0001));
		}

		TEST_CASE("PhysicsWorld: a distance joint keeps its body between its minimum and maximum distance, and a spring lets it stretch")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle rigid = CreateBody(*world, SphereShapeGeometry{ .Radius = 0.1f }, glm::vec3(0.0f, 4.0f, 0.0f));
			const BodyHandle springy = CreateBody(*world, SphereShapeGeometry{ .Radius = 0.1f }, glm::vec3(3.0f, 4.0f, 0.0f));
			const JointHandle rope = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Distance, .Body = rigid, .Frame = MakeFrame(glm::vec3(0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f, 5.0f, 0.0f)), .Settings = PhysicsJointSettings{ .LowerLimit = 0.0f, .UpperLimit = 2.0f } });
			const JointHandle elastic = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Distance, .Body = springy, .Frame = MakeFrame(glm::vec3(0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(3.0f, 5.0f, 0.0f)), .Settings = PhysicsJointSettings{ .LowerLimit = 0.0f, .UpperLimit = 2.0f, .LimitFrequency = 1.0f, .LimitDamping = 0.2f } });
			float longestRigid = 0.0f;
			float longestSpringy = 0.0f;
			for (int step = 0; step < 240; ++step)
			{
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				longestRigid = std::max(longestRigid, world->GetJointState(rope).Value);
				longestSpringy = std::max(longestSpringy, world->GetJointState(elastic).Value);
			}
			// The rope stops the falling bob within a few centimetres of its length, as Jolt's position correction allows.
			CHECK(longestRigid < 2.05f);
			CHECK(world->GetJointState(rope).Value == doctest::Approx(2.0).epsilon(0.01));
			// The spring lets the rope stretch past its maximum on the fall.
			CHECK(longestSpringy > 2.1f);
			// The taut rope carries the bob's weight.
			CHECK(world->GetJointState(rope).Force == doctest::Approx(Gravity).epsilon(0.03));
		}

		TEST_CASE("PhysicsWorld: a fixed joint welds two bodies, and reads the force and torque of the load it carries")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			// A 10 kg box welded to the world 1 m to the side of its centre: it carries m g and a torque of m g times 1 m.
			const BodyHandle load = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, glm::vec3(1.0f, 3.0f, 0.0f), PhysicsMotionType::Dynamic,
				10.0f);
			const JointHandle weld = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Fixed, .Body = load, .Frame = MakeFrame(glm::vec3(-1.0f, 0.0f, 0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f, 3.0f, 0.0f)) });
			CHECK(world->GetJointState(weld).Force == 0.0f);
			StepWorld(*world, 60);
			const PhysicsJointState reading = world->GetJointState(weld);
			CHECK(reading.Force == doctest::Approx(10.0f * Gravity).epsilon(0.01));
			CHECK(reading.Torque == doctest::Approx(10.0f * Gravity).epsilon(0.02));
			CHECK(reading.Value == 0.0f);
			CHECK(world->GetPose(load).Position.y == doctest::Approx(3.0).epsilon(0.002));

			// Two Dynamic bodies welded together fall onto the ground as one.
			const BodyHandle ground = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(20.0f, 0.5f, 20.0f) }, glm::vec3(0.0f, -0.5f, 0.0f),
				PhysicsMotionType::Static);
			static_cast<void>(ground);
			const BodyHandle left = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, glm::vec3(5.0f, 2.0f, 0.0f));
			const BodyHandle right = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, glm::vec3(6.0f, 2.5f, 0.0f));
			static_cast<void>(CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Fixed, .Body = right, .ConnectedBody = left, .Frame = MakeFrame(glm::vec3(-0.5f, -0.25f, 0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.5f, 0.25f, 0.0f)) }));
			StepWorld(*world, 120);
			const glm::vec3 offset = glm::conjugate(world->GetPose(left).Rotation) * (world->GetPose(right).Position - world->GetPose(left).Position);
			CHECK(offset.x == doctest::Approx(1.0).epsilon(0.01));
			CHECK(offset.y == doctest::Approx(0.5).epsilon(0.02));
		}

		TEST_CASE("PhysicsWorld: destroying a body destroys its joints first, and their handles go invalid")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle a = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, glm::vec3(0.0f, 2.0f, 0.0f));
			const BodyHandle b = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, glm::vec3(1.0f, 2.0f, 0.0f));
			const JointHandle between = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Distance, .Body = b, .ConnectedBody = a, .Frame = MakeFrame(glm::vec3(0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f)), .Settings = PhysicsJointSettings{ .LowerLimit = 1.0f, .UpperLimit = 1.0f } });
			const JointHandle toWorld = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Fixed, .Body = a, .Frame = MakeFrame(glm::vec3(0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f, 2.0f, 0.0f)) });
			StepWorld(*world, 10);
			CHECK(world->GetJointCount() == 2);

			world->DestroyBody(a);
			CHECK_FALSE(world->IsJointValid(between));
			CHECK_FALSE(world->IsJointValid(toWorld));
			CHECK(world->GetJointCount() == 0);
			CHECK(world->GetJointState(between).Force == 0.0f);
			// A handle that names no live joint is ignored, and the world keeps stepping its free body.
			world->DestroyJoint(between);
			REQUIRE_FALSE(world->SetJointSettings(between, {}).has_value());
			const float before = world->GetPose(b).Position.y;
			StepWorld(*world, 30);
			CHECK(world->GetPose(b).Position.y < before - 0.5f);
		}

		TEST_CASE("PhysicsWorld: a joint's bodies never collide while CollideConnected is false, continuous casts included")
		{
			for (const bool collide : { false, true })
			{
				CAPTURE(collide);
				Scope<PhysicsWorld> world = CreateWorld();
				static_cast<void>(CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(20.0f, 0.5f, 20.0f) }, glm::vec3(0.0f, -0.5f, 0.0f),
					PhysicsMotionType::Static));
				// Two overlapping boxes resting on the ground, joined loosely.
				const BodyHandle a = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 0.5f, 0.0f));
				const BodyHandle b = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.6f, 0.5f, 0.0f));
				const JointHandle loose = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Distance, .Body = b, .ConnectedBody = a, .Frame = MakeFrame(glm::vec3(0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f)), .Settings = PhysicsJointSettings{ .CollideConnected = collide, .LowerLimit = 0.0f, .UpperLimit = 10.0f } });
				// A LinearCast bullet at 200 m/s towards a thin static wall it is joined to (its continuous cast would hit it).
				const BodyHandle wall = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.05f, 2.0f, 2.0f) }, glm::vec3(10.0f, 3.0f, 10.0f),
					PhysicsMotionType::Static);
				const BodyHandle bullet = CreateBody(*world, SphereShapeGeometry{ .Radius = 0.1f }, glm::vec3(5.0f, 3.0f, 10.0f), PhysicsMotionType::Dynamic, 1.0f,
					PhysicsMotionQuality::LinearCast);
				static_cast<void>(CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Distance, .Body = bullet, .ConnectedBody = wall, .Frame = MakeFrame(glm::vec3(0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f)), .Settings = PhysicsJointSettings{ .CollideConnected = collide, .LowerLimit = 0.0f, .UpperLimit = 100.0f } }));
				world->SetLinearVelocity(bullet, glm::vec3(200.0f, 0.0f, 0.0f));

				std::vector<ContactEvent> contacts;
				for (int step = 0; step < 5; ++step)
				{
					REQUIRE(world->Step(FixedDelta, 1).has_value());
					std::vector<ContactEvent> stepContacts = world->DrainContactEvents();
					contacts.insert(contacts.end(), stepContacts.begin(), stepContacts.end());
				}
				CHECK(HasContact(contacts, a, b) == collide);
				CHECK(HasContact(contacts, bullet, wall) == collide);
				// Without the collision the bullet flew through the wall; with it, the wall stopped it.
				CHECK((world->GetPose(bullet).Position.x > 10.5f) == !collide);

				// Switched in place, the resting boxes collide afresh at the next step (or stop colliding).
				REQUIRE(world->SetJointSettings(loose, PhysicsJointSettings{ .CollideConnected = !collide, .LowerLimit = 0.0f, .UpperLimit = 10.0f }).has_value());
				std::vector<ContactEvent> switched;
				for (int step = 0; step < 3; ++step)
				{
					REQUIRE(world->Step(FixedDelta, 1).has_value());
					std::vector<ContactEvent> stepContacts = world->DrainContactEvents();
					switched.insert(switched.end(), stepContacts.begin(), stepContacts.end());
				}
				const bool added = std::any_of(switched.begin(), switched.end(), [a, b](const ContactEvent& event)
				{
					return event.Kind == ContactEventKind::Added && ((event.BodyA == a && event.BodyB == b) || (event.BodyA == b && event.BodyB == a));
				});
				CHECK(added == !collide);
			}
		}

		TEST_CASE("PhysicsWorld: joint descriptions and settings Jolt would assert on are refused, and nothing reaches Jolt")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle a = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, glm::vec3(0.0f, 2.0f, 0.0f));
			const BodyHandle b = CreateBody(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, glm::vec3(1.0f, 2.0f, 0.0f));
			const PhysicsJointDescription valid{ .Type = PhysicsJointType::Hinge,
				.Body = a,
				.ConnectedBody = b,
				.Frame = MakeFrame(glm::vec3(0.5f, 0.0f, 0.0f)),
				.ConnectedFrame = MakeFrame(glm::vec3(-0.5f, 0.0f, 0.0f)) };
			const auto refused = [&world](PhysicsJointDescription description)
			{
				const Result<JointHandle> joint = world->CreateJoint(description);
				return !joint.has_value() && joint.error().GetCode() == ErrorCode::InvalidArgument;
			};

			PhysicsJointDescription description = valid;
			description.Type = static_cast<PhysicsJointType>(9);
			CHECK(refused(description));
			description = valid;
			description.Body = BodyHandle(12345);
			CHECK(refused(description));
			description = valid;
			description.ConnectedBody = BodyHandle(12345);
			CHECK(refused(description));
			description = valid;
			description.ConnectedBody = a;
			CHECK(refused(description));
			description = valid;
			description.Frame.Axis = glm::vec3(0.0f);
			CHECK(refused(description));
			description = valid;
			description.ConnectedFrame.Axis = glm::vec3(2.0f, 0.0f, 0.0f);
			CHECK(refused(description));
			description = valid;
			description.Frame.Normal = glm::vec3(1.0f, 0.0f, 0.0f);
			CHECK(refused(description));
			description = valid;
			description.Frame.Point = glm::vec3(2.0f * MaxPhysicsCoordinate, 0.0f, 0.0f);
			CHECK(refused(description));
			// Equal limits without a spring ("better use a fixed constraint"), on a hinge and on a slider.
			description = valid;
			description.Settings = PhysicsJointSettings{ .UseLimits = true, .LowerLimit = 0.0f, .UpperLimit = 0.0f };
			CHECK(refused(description));
			description.Type = PhysicsJointType::Slider;
			CHECK(refused(description));
			// Limits of the wrong sign.
			description = valid;
			description.Settings = PhysicsJointSettings{ .UseLimits = true, .LowerLimit = 0.2f, .UpperLimit = 0.5f };
			CHECK(refused(description));
			// A position drive without a spring, a negative damping or force, a target or a spring beyond the bounds.
			description = valid;
			description.Settings.MotorFrequency = 0.0f;
			CHECK(refused(description));
			description = valid;
			description.Settings.MotorDamping = -1.0f;
			CHECK(refused(description));
			description = valid;
			description.Settings.MotorMaxForce = -1.0f;
			CHECK(refused(description));
			description = valid;
			description.Settings.MotorTargetVelocity = 2.0f * MaxPhysicsJointMotorTarget;
			CHECK(refused(description));
			description = valid;
			description.Settings.MaxFriction = -1.0f;
			CHECK(refused(description));
			description = valid;
			description.Settings.MotorTargetPosition = std::numeric_limits<float>::quiet_NaN();
			CHECK(refused(description));
			description = valid;
			description.Settings.Motor = static_cast<PhysicsJointMotor>(7);
			CHECK(refused(description));
			// A distance joint's minimum above its maximum, a negative one, and a spring beyond the bound.
			description = valid;
			description.Type = PhysicsJointType::Distance;
			description.Settings = PhysicsJointSettings{ .LowerLimit = 2.0f, .UpperLimit = 1.0f };
			CHECK(refused(description));
			description.Settings = PhysicsJointSettings{ .LowerLimit = -1.0f, .UpperLimit = 1.0f };
			CHECK(refused(description));
			description.Settings = PhysicsJointSettings{ .LowerLimit = 0.0f, .UpperLimit = 1.0f, .LimitFrequency = 2.0f * MaxPhysicsJointSpring };
			CHECK(refused(description));
			CHECK(world->GetJointCount() == 0);

			// A hinge's limits beyond a turn are clamped to Jolt's range, and the valid description is created.
			description = valid;
			description.Settings = PhysicsJointSettings{ .UseLimits = true, .LowerLimit = -4.0f, .UpperLimit = 4.0f };
			const JointHandle joint = CreateJoint(*world, description);
			CHECK(world->IsJointValid(joint));
			REQUIRE_FALSE(world->SetJointSettings(joint, PhysicsJointSettings{ .UseLimits = true }).has_value());
			StepWorld(*world, 10);
		}

		TEST_CASE("PhysicsWorld: SetShape moves a joint's centre-of-mass offset, so the joint still holds the body at its origin")
		{
			// A sphere hanging on a 1 m rope from its origin. Its new shape moves the centre of mass 0.5 m from the origin: the
			// rope still holds the origin, which stays within 1 m of the pivot while the centre of mass swings under it.
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle bob = CreateBody(*world, SphereShapeGeometry{ .Radius = 0.25f }, glm::vec3(0.0f, 2.0f, 0.0f));
			const JointHandle rope = CreateJoint(*world, PhysicsJointDescription{ .Type = PhysicsJointType::Distance, .Body = bob, .Frame = MakeFrame(glm::vec3(0.0f)), .ConnectedFrame = MakeFrame(glm::vec3(0.0f, 3.0f, 0.0f)), .Settings = PhysicsJointSettings{ .LowerLimit = 0.0f, .UpperLimit = 1.0f } });
			StepWorld(*world, 30);
			REQUIRE(world->SetShape(bob, CreateShape(SphereShapeGeometry{ .Radius = 0.25f }, glm::vec3(0.5f, 0.0f, 0.0f))).has_value());
			REQUIRE(world->IsJointValid(rope));
			float farthest = 0.0f;
			for (int step = 0; step < 180; ++step)
			{
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				farthest = std::max(farthest, glm::length(world->GetPose(bob).Position - glm::vec3(0.0f, 3.0f, 0.0f)));
			}
			CHECK(farthest < 1.03f);
			CHECK(world->GetJointState(rope).Value == doctest::Approx(1.0).epsilon(0.02));
		}

		TEST_CASE("PhysicsWorld: joints given the same priorities step identically whatever order they were created in")
		{
			// Jolt solves an island's constraints by (priority, index), and its index follows the add-and-remove history; unique
			// priorities make the order a function of the joints alone (Docs/Decisions/0054-m37-joints.md decision 5).
			const uint64_t forward = RunChain(true);
			const uint64_t reverse = RunChain(false);
			CHECK(forward == reverse);
		}
	}

}
