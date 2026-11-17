#include "TestsPCH.h"

#include "Engine/Scene/PhysicsJoints.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/SliderJointComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <numbers>
#include <string>
#include <tuple>
#include <vector>

// The joint composition rules (M37, Docs/Decisions/0054-m37-joints.md decisions 3 and 9; Architecture §5.3, §9.10): which
// bodies a joint joins, PHYSICS_JOINT_INVALID with one reason per joint, the component read in the world's units and the
// frames' conversions.

namespace Engine {

	namespace {

		PhysicsJointComposition Compose(const Scene& scene)
		{
			return ComposePhysicsJoints(scene, ComposePhysicsBodies(scene, PhysicsLayerTable()));
		}

		const PhysicsJointPlan* FindJoint(const PhysicsJointComposition& composition, UUID entity)
		{
			for (const PhysicsJointPlan& plan : composition.Joints)
			{
				if (plan.Entity == entity)
					return &plan;
			}
			return nullptr;
		}

		// The subjects of the PHYSICS_JOINT_INVALID diagnostics on `entity`.
		std::vector<std::string> InvalidSubjects(const PhysicsJointComposition& composition, UUID entity)
		{
			std::vector<std::string> subjects;
			for (const PhysicsDiagnostic& diagnostic : composition.Diagnostics)
			{
				if (diagnostic.Entity == entity && diagnostic.Code == PhysicsJointInvalidCode)
					subjects.push_back(diagnostic.Subject);
			}
			return subjects;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsJoints: a joint joins the RigidBody at or above its entity to its ConnectedBody's body or to the world")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A level of static colliders without a RigidBody (implicit static bodies), a cage with a Dynamic RigidBody and two
			// rope entities below it, and a door joined to the world.
			Entity level = scene.CreateEntity("Level");
			Entity beam = Test::AddBoxBody(scene, "Beam", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(2.0f, 0.1f, 0.1f), std::nullopt, level);
			Entity cage = Test::AddBoxBody(scene, "Cage", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Entity left = scene.CreateEntity("Left", cage);
			Entity right = scene.CreateEntity("Right", cage);
			left.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = beam.GetUUID() });
			right.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = beam.GetUUID() });
			Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(3.0f, 1.0f, 0.0f), glm::vec3(0.5f, 1.0f, 0.05f), BodyType::Dynamic);
			door.AddComponent<HingeJointComponent>(HingeJointComponent{ .Axis = glm::vec3(0.0f, 1.0f, 0.0f) });

			const PhysicsJointComposition composition = Compose(scene);
			CHECK(composition.Diagnostics.empty());
			REQUIRE(composition.Joints.size() == 3);
			// Canonical order of the joints' entities.
			CHECK(composition.Joints[0].Entity == left.GetUUID());
			CHECK(composition.Joints[1].Entity == right.GetUUID());
			CHECK(composition.Joints[2].Entity == door.GetUUID());
			for (const PhysicsJointPlan* rope : { FindJoint(composition, left.GetUUID()), FindJoint(composition, right.GetUUID()) })
			{
				REQUIRE(rope != nullptr);
				CHECK(rope->Type == PhysicsJointType::Distance);
				CHECK(rope->IsCreatable);
				CHECK(rope->Body == cage.GetUUID());
				CHECK(rope->Connected == beam.GetUUID());
				CHECK(rope->ConnectedOrigin == PhysicsBodyOrigin::ImplicitStatic);
			}
			const PhysicsJointPlan* hinge = FindJoint(composition, door.GetUUID());
			REQUIRE(hinge != nullptr);
			CHECK(hinge->Type == PhysicsJointType::Hinge);
			CHECK(hinge->Body == door.GetUUID());
			CHECK_FALSE(hinge->Connected.IsValid());

			// A collider gathered into an ancestor's implicit static body names that body; a disabled joint is not listed, and
			// one whose connected entity is disabled is listed not creatable, without a diagnostic.
			Entity bolt = Test::AddBoxBody(scene, "Bolt", glm::vec3(0.0f, 0.2f, 0.0f), glm::vec3(0.05f), std::nullopt, beam);
			left.Patch<DistanceJointComponent>([&bolt](DistanceJointComponent& joint)
			{
				joint.ConnectedBody = bolt.GetUUID();
			});
			door.SetActive(false);
			Entity hook = Test::AddBoxBody(scene, "Hook", glm::vec3(5.0f, 5.0f, 0.0f), glm::vec3(0.1f), BodyType::Static);
			right.Patch<DistanceJointComponent>([&hook](DistanceJointComponent& joint)
			{
				joint.ConnectedBody = hook.GetUUID();
			});
			hook.SetActive(false);
			const PhysicsJointComposition changed = Compose(scene);
			CHECK(changed.Diagnostics.empty());
			REQUIRE(changed.Joints.size() == 2);
			CHECK(changed.Joints[0].Connected == beam.GetUUID());
			CHECK(changed.Joints[0].ConnectedOrigin == PhysicsBodyOrigin::ImplicitStatic);
			CHECK(changed.Joints[0].IsCreatable);
			CHECK_FALSE(changed.Joints[1].IsCreatable);
			CHECK(FindJoint(changed, door.GetUUID()) == nullptr);
		}

		TEST_CASE("PhysicsJoints: each joint that cannot be made gets one PHYSICS_JOINT_INVALID naming its reason")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity body = Test::AddBoxBody(scene, "Body", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Entity other = Test::AddBoxBody(scene, "Other", glm::vec3(2.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);

			// No RigidBody at or above; a RigidBody without collider; under a CharacterController; under a RigidBody2D.
			Entity loose = scene.CreateEntity("Loose");
			loose.AddComponent<FixedJointComponent>();
			Entity bare = scene.CreateEntity("Bare");
			bare.AddComponent<RigidBodyComponent>();
			bare.AddComponent<FixedJointComponent>();
			Entity character = scene.CreateEntity("Character");
			character.AddComponent<CharacterControllerComponent>();
			Entity riding = scene.CreateEntity("Riding", character);
			riding.AddComponent<FixedJointComponent>();
			Entity flat = scene.CreateEntity("Flat");
			flat.AddComponent<RigidBody2DComponent>();
			Entity flatJoint = scene.CreateEntity("FlatJoint", flat);
			flatJoint.AddComponent<FixedJointComponent>();
			// ConnectedBody naming no entity, an entity without a body, a character, or the joint's own body.
			Entity dangling = scene.CreateEntity("Dangling", body);
			dangling.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = UUID(0xdead) });
			Entity empty = scene.CreateEntity("Empty");
			Entity toEmpty = scene.CreateEntity("ToEmpty", body);
			toEmpty.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = empty.GetUUID() });
			Entity toCharacter = scene.CreateEntity("ToCharacter", body);
			toCharacter.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = character.GetUUID() });
			Entity same = scene.CreateEntity("Same", body);
			same.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = body.GetUUID() });
			// A zero axis, equal limits, a minimum above the maximum.
			Entity zeroAxis = scene.CreateEntity("ZeroAxis", other);
			zeroAxis.AddComponent<HingeJointComponent>(HingeJointComponent{ .Axis = glm::vec3(0.0f) });
			Entity locked = scene.CreateEntity("Locked", other);
			locked.AddComponent<SliderJointComponent>(SliderJointComponent{ .UseLimits = true, .LowerLimit = 0.0f, .UpperLimit = 0.0f });
			Entity lockedHinge = scene.CreateEntity("LockedHinge", other);
			lockedHinge.AddComponent<HingeJointComponent>(HingeJointComponent{ .UseLimits = true, .LowerAngle = 0.0f, .UpperAngle = 0.0f });
			Entity inverted = scene.CreateEntity("Inverted", other);
			inverted.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .MinDistance = 2.0f, .MaxDistance = 1.0f });
			// Valid: equal limits without UseLimits, a minimum equal to the maximum, neither end Dynamic.
			Entity unlimited = scene.CreateEntity("Unlimited", other);
			unlimited.AddComponent<HingeJointComponent>(HingeJointComponent{ .UseLimits = false, .LowerAngle = 0.0f, .UpperAngle = 0.0f });
			Entity rod = scene.CreateEntity("Rod", other);
			rod.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .MinDistance = 1.0f, .MaxDistance = 1.0f });
			Entity anchored = Test::AddBoxBody(scene, "Anchored", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Static);
			anchored.AddComponent<FixedJointComponent>();

			const PhysicsJointComposition composition = Compose(scene);
			const auto expect = [&composition](Entity entity, std::string_view subject)
			{
				CAPTURE(entity.GetName());
				const std::vector<std::string> subjects = InvalidSubjects(composition, entity.GetUUID());
				REQUIRE(subjects.size() == 1);
				CHECK(subjects[0] == subject);
				const PhysicsJointPlan* plan = FindJoint(composition, entity.GetUUID());
				REQUIRE(plan != nullptr);
				CHECK_FALSE(plan->IsCreatable);
			};
			expect(loose, "body");
			expect(bare, "body");
			expect(riding, "character");
			expect(flatJoint, "2d");
			expect(dangling, "connected");
			expect(toEmpty, "connected");
			expect(toCharacter, "character");
			expect(same, "same");
			expect(zeroAxis, "axis");
			expect(locked, "limits");
			expect(lockedHinge, "limits");
			expect(inverted, "distance");
			for (const Entity valid : { unlimited, rod, anchored })
			{
				CAPTURE(valid.GetName());
				CHECK(InvalidSubjects(composition, valid.GetUUID()).empty());
				const PhysicsJointPlan* plan = FindJoint(composition, valid.GetUUID());
				REQUIRE(plan != nullptr);
				CHECK(plan->IsCreatable);
			}
			// The diagnostic names the component, its field and the entity, and is an Error.
			const auto found = std::find_if(composition.Diagnostics.begin(), composition.Diagnostics.end(), [&zeroAxis](const PhysicsDiagnostic& diagnostic)
			{
				return diagnostic.Entity == zeroAxis.GetUUID();
			});
			REQUIRE(found != composition.Diagnostics.end());
			CHECK(found->Component == "HingeJoint");
			CHECK(found->Field == "Axis");
			CHECK(found->Severity == DiagnosticSeverity::Error);
			CHECK_FALSE(found->AutoFixable);
			CHECK(std::is_sorted(composition.Diagnostics.begin(), composition.Diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
			{
				return std::tie(a.Entity, a.Code, a.Subject) < std::tie(b.Entity, b.Code, b.Subject);
			}));
		}

		TEST_CASE("PhysicsJoints: a joint whose body a body diagnostic refused gets no second diagnostic")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A Dynamic body with every degree of freedom locked is refused with PHYSICS_ALL_DOFS_LOCKED.
			Entity locked = Test::AddBoxBody(scene, "Locked", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(locked, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});
			locked.AddComponent<HingeJointComponent>();
			Entity free = Test::AddBoxBody(scene, "Free", glm::vec3(2.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			free.AddComponent<FixedJointComponent>(FixedJointComponent{ .ConnectedBody = locked.GetUUID() });

			const PhysicsJointComposition composition = Compose(scene);
			CHECK(composition.Diagnostics.empty());
			REQUIRE(composition.Joints.size() == 2);
			CHECK_FALSE(composition.Joints[0].IsCreatable);
			CHECK_FALSE(composition.Joints[1].IsCreatable);
		}

		TEST_CASE("PhysicsJoints: a joint component is read in the world's units")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity hinge = scene.CreateEntity("Hinge");
			hinge.AddComponent<HingeJointComponent>(HingeJointComponent{ .UseLimits = true,
				.LowerAngle = -90.0f,
				.UpperAngle = 45.0f,
				.MaxFrictionTorque = 3.0f,
				.Motor = JointMotorMode::Position,
				.MotorTargetVelocity = 180.0f,
				.MotorTargetAngle = 30.0f,
				.MotorMaxTorque = 50.0f,
				.CollideConnected = true,
				.BreakForce = 10.0f,
				.BreakTorque = 20.0f,
				.Broken = true });
			const std::optional<PhysicsJointFields> fields = ReadPhysicsJoint(hinge);
			REQUIRE(fields.has_value());
			const float pi = std::numbers::pi_v<float>;
			CHECK(fields->Type == PhysicsJointType::Hinge);
			CHECK(fields->Settings.LowerLimit == doctest::Approx(-pi / 2.0f));
			CHECK(fields->Settings.UpperLimit == doctest::Approx(pi / 4.0f));
			CHECK(fields->Settings.MotorTargetVelocity == doctest::Approx(pi));
			CHECK(fields->Settings.MotorTargetPosition == doctest::Approx(pi / 6.0f));
			CHECK(fields->Settings.MaxFriction == 3.0f);
			CHECK(fields->Settings.MotorMaxForce == 50.0f);
			CHECK(fields->Settings.Motor == PhysicsJointMotor::Position);
			CHECK(fields->Settings.CollideConnected);
			CHECK(fields->BreakForce == 10.0f);
			CHECK(fields->BreakTorque == 20.0f);
			CHECK(fields->Broken);

			Entity rope = scene.CreateEntity("Rope");
			rope.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedAnchor = glm::vec3(1.0f, 2.0f, 3.0f),
				.MinDistance = 0.5f,
				.MaxDistance = 2.0f,
				.Frequency = 3.0f,
				.Damping = 0.5f });
			const std::optional<PhysicsJointFields> distance = ReadPhysicsJoint(rope);
			REQUIRE(distance.has_value());
			CHECK(distance->Type == PhysicsJointType::Distance);
			CHECK(distance->ConnectedAnchor == glm::vec3(1.0f, 2.0f, 3.0f));
			CHECK(distance->Settings.LowerLimit == 0.5f);
			CHECK(distance->Settings.UpperLimit == 2.0f);
			CHECK(distance->Settings.LimitFrequency == 3.0f);
			CHECK(distance->Settings.LimitDamping == 0.5f);
			CHECK(distance->BreakTorque == 0.0f);

			CHECK_FALSE(ReadPhysicsJoint(scene.CreateEntity("None")).has_value());
			WritePhysicsJointBroken(rope, true);
			CHECK(rope.GetComponent<DistanceJointComponent>().Broken);
		}

		TEST_CASE("PhysicsJoints: a joint's world frame bakes in its entity's transform, and body frames convert both ways")
		{
			// An entity at (1, 2, 3) turned 90 degrees about Y and scaled 2: its Anchor (1, 0, 0) lies at (1, 2, 1) and its X
			// axis along -Z.
			const glm::quat turn = glm::angleAxis(std::numbers::pi_v<float> / 2.0f, glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)) * glm::mat4_cast(turn) * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
			PhysicsJointFields fields;
			fields.Type = PhysicsJointType::Hinge;
			fields.Anchor = glm::vec3(1.0f, 0.0f, 0.0f);
			fields.Axis = glm::vec3(3.0f, 0.0f, 0.0f);
			const std::optional<PhysicsJointFrame> frame = MakePhysicsJointWorldFrame(world, fields);
			REQUIRE(frame.has_value());
			CHECK(frame->Point.x == doctest::Approx(1.0));
			CHECK(frame->Point.y == doctest::Approx(2.0));
			CHECK(frame->Point.z == doctest::Approx(1.0));
			CHECK(frame->Axis.z == doctest::Approx(-1.0));
			CHECK(glm::length(frame->Normal) == doctest::Approx(1.0));
			CHECK(std::abs(glm::dot(frame->Axis, frame->Normal)) < 1.0e-6f);

			const PhysicsPose pose{ .Position = glm::vec3(1.0f, 2.0f, 3.0f), .Rotation = turn };
			const PhysicsJointFrame local = ToPhysicsJointBodyFrame(pose, *frame);
			CHECK(local.Point.x == doctest::Approx(2.0));
			CHECK(local.Axis.x == doctest::Approx(1.0));
			const PhysicsJointFrame back = ToPhysicsJointWorldFrame(pose, local);
			CHECK(back.Point.z == doctest::Approx(1.0));
			CHECK(back.Axis.z == doctest::Approx(-1.0));

			// A zero axis has no frame.
			fields.Axis = glm::vec3(0.0f);
			CHECK_FALSE(MakePhysicsJointWorldFrame(world, fields).has_value());
		}
	}

}
