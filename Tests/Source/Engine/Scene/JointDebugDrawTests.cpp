#include "TestsPCH.h"

#include "Engine/Scene/JointDebugDraw.h"

#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <tuple>
#include <variant>
#include <vector>

// The joints' debug view (M37, Docs/Decisions/0054-m37-joints.md decision 11): the colour of each state, the records of an
// edit scene (from the components) and of a play session (where the bodies hold the joints), and their lines.

namespace Engine {

	namespace {

		const JointDebugShape* FindJointShape(const std::vector<JointDebugShape>& shapes, UUID entity)
		{
			const auto found = std::find_if(shapes.begin(), shapes.end(), [entity](const JointDebugShape& shape)
			{
				return shape.Entity == entity;
			});
			return found == shapes.end() ? nullptr : &*found;
		}

		bool IsNear(const glm::vec3& a, const glm::vec3& b, float tolerance = 1.0e-3f)
		{
			return glm::length(a - b) < tolerance;
		}

		// A door hinged to the world about Y at its left edge, a weld authored broken, a hinge with a zero axis (refused), and a
		// bob on a rope 2 m below a static post.
		void AddJointedScene(Scene& scene)
		{
			Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(1.0f, 2.0f, 0.0f), glm::vec3(0.5f, 1.0f, 0.05f), BodyType::Dynamic);
			door.AddComponent<HingeJointComponent>(HingeJointComponent{ .Anchor = glm::vec3(-0.5f, 0.0f, 0.0f), .Axis = glm::vec3(0.0f, 1.0f, 0.0f) });
			Entity weld = Test::AddBoxBody(scene, "Weld", glm::vec3(5.0f, 2.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			weld.AddComponent<FixedJointComponent>(FixedJointComponent{ .Broken = true });
			Entity bent = Test::AddBoxBody(scene, "Bent", glm::vec3(10.0f, 2.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			bent.AddComponent<HingeJointComponent>(HingeJointComponent{ .Axis = glm::vec3(0.0f) });
			Entity post = Test::AddBoxBody(scene, "Post", glm::vec3(15.0f, 5.0f, 0.0f), glm::vec3(0.1f), BodyType::Static);
			Entity bob = Test::AddSphereBody(scene, "Bob", glm::vec3(15.0f, 3.0f, 0.0f), 0.1f, BodyType::Dynamic);
			bob.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = post.GetUUID(), .MinDistance = 0.0f, .MaxDistance = 2.0f });
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("JointDebugDraw: every state has its own colour")
		{
			std::set<std::tuple<float, float, float, float>> colours;
			for (const JointDebugState state : { JointDebugState::InForce, JointDebugState::Broken, JointDebugState::Refused })
			{
				const glm::vec4 colour = GetJointDebugColor(state);
				CHECK(colour.a == 1.0f);
				colours.insert({ colour.r, colour.g, colour.b, colour.a });
			}
			CHECK(colours.size() == 3);
		}

		TEST_CASE("JointDebugDraw: an edit scene draws each joint where its components place it, in its state's colour")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddJointedScene(scene);
			const std::vector<JointDebugShape> shapes = BuildJointDebugDraw(scene, PhysicsLayerTable(), nullptr);
			REQUIRE(shapes.size() == 4);
			// Canonical order of the entities.
			CHECK(shapes[0].Entity == Test::GetEntityId(scene, "/Door"));

			const JointDebugShape* door = FindJointShape(shapes, Test::GetEntityId(scene, "/Door"));
			REQUIRE(door != nullptr);
			CHECK(door->Type == PhysicsJointType::Hinge);
			CHECK(door->State == JointDebugState::InForce);
			CHECK(IsNear(door->Point, glm::vec3(0.5f, 2.0f, 0.0f)));
			CHECK(IsNear(door->ConnectedPoint, door->Point));
			CHECK(IsNear(door->Axis, glm::vec3(0.0f, 1.0f, 0.0f)));

			const JointDebugShape* weld = FindJointShape(shapes, Test::GetEntityId(scene, "/Weld"));
			REQUIRE(weld != nullptr);
			CHECK(weld->State == JointDebugState::Broken);
			CHECK(weld->Axis == glm::vec3(0.0f));

			// A zero axis cannot be made: drawn red, at its entity.
			const JointDebugShape* bent = FindJointShape(shapes, Test::GetEntityId(scene, "/Bent"));
			REQUIRE(bent != nullptr);
			CHECK(bent->State == JointDebugState::Refused);
			CHECK(IsNear(bent->Point, glm::vec3(10.0f, 2.0f, 0.0f)));

			// A rope reaches from its anchor to the connected body's.
			const JointDebugShape* bob = FindJointShape(shapes, Test::GetEntityId(scene, "/Bob"));
			REQUIRE(bob != nullptr);
			CHECK(bob->State == JointDebugState::InForce);
			CHECK(IsNear(bob->Point, glm::vec3(15.0f, 3.0f, 0.0f)));
			CHECK(IsNear(bob->ConnectedPoint, glm::vec3(15.0f, 5.0f, 0.0f)));
		}

		TEST_CASE("JointDebugDraw: a play session draws a joint in force where the bodies hold it and one it did not make as refused")
		{
			Test::SceneTestFixture fixture;
			AddJointedScene(fixture.GetScene());
			// The bent hinge's refusal.
			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_JOINT_INVALID");
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE(session.has_value());
			Test::RunTicks(**session, 60);
			const Scene& runtime = (*session)->GetScene();
			const PhysicsSystem& physics = (*session)->GetPhysics();
			const std::vector<JointDebugShape> shapes = BuildJointDebugDraw(runtime, PhysicsLayerTable(), &physics);

			// The bob has fallen to the end of its rope: the line runs from where the bob holds it to the post.
			const UUID bobId = Test::GetEntityId(runtime, "/Bob");
			const JointDebugShape* bob = FindJointShape(shapes, bobId);
			const std::optional<PhysicsJointReport> report = physics.GetJointInfo(bobId);
			REQUIRE(bob != nullptr);
			REQUIRE(report.has_value());
			REQUIRE(report->Created);
			CHECK(bob->State == JointDebugState::InForce);
			CHECK(IsNear(bob->Point, report->Point));
			CHECK(IsNear(bob->ConnectedPoint, glm::vec3(15.0f, 5.0f, 0.0f)));
			CHECK(glm::length(bob->Point - bob->ConnectedPoint) == doctest::Approx(2.0f).epsilon(0.02));

			const JointDebugShape* weld = FindJointShape(shapes, Test::GetEntityId(runtime, "/Weld"));
			const JointDebugShape* bent = FindJointShape(shapes, Test::GetEntityId(runtime, "/Bent"));
			REQUIRE(weld != nullptr);
			REQUIRE(bent != nullptr);
			CHECK(weld->State == JointDebugState::Broken);
			CHECK(bent->State == JointDebugState::Refused);
		}

		TEST_CASE("JointDebugDraw: AppendJointDebugDraw draws a cross, the axis and the line to the connected point on top")
		{
			JointDebugShape hinge;
			hinge.Type = PhysicsJointType::Hinge;
			hinge.Point = glm::vec3(1.0f, 2.0f, 3.0f);
			hinge.ConnectedPoint = hinge.Point;
			hinge.Axis = glm::vec3(0.0f, 1.0f, 0.0f);
			JointDebugShape rope;
			rope.Type = PhysicsJointType::Distance;
			rope.State = JointDebugState::Broken;
			rope.Point = glm::vec3(0.0f);
			rope.ConnectedPoint = glm::vec3(0.0f, 2.0f, 0.0f);
			const std::array<JointDebugShape, 2> shapes = { hinge, rope };

			DebugDrawList list;
			AppendJointDebugDraw(shapes, list);
			const auto commands = list.GetCommands();
			// The hinge: three cross lines and its axis; the rope: three cross lines and its line.
			REQUIRE(commands.size() == 8);
			for (size_t index = 0; index < commands.size(); ++index)
			{
				CAPTURE(index);
				const DebugDrawCommand& command = commands[index];
				REQUIRE(std::holds_alternative<DebugLine>(command.Shape));
				CHECK(command.Depth == DebugDepthMode::OnTop);
				CHECK(command.Duration == 0.0f);
				CHECK(command.Color == GetJointDebugColor(index < 4 ? JointDebugState::InForce : JointDebugState::Broken));
			}
			const DebugLine& axis = std::get<DebugLine>(commands[3].Shape);
			CHECK(IsNear(axis.From, glm::vec3(1.0f, 2.0f - JointDebugAxisLength * 0.5f, 3.0f)));
			CHECK(IsNear(axis.To, glm::vec3(1.0f, 2.0f + JointDebugAxisLength * 0.5f, 3.0f)));
			const DebugLine& line = std::get<DebugLine>(commands[7].Shape);
			CHECK(line.From == rope.Point);
			CHECK(line.To == rope.ConnectedPoint);
		}
	}

}
