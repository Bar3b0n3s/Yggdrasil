#include "TestsPCH.h"

#include "Engine/Scene/JointDebugDraw.h"

#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Core/Time.h"
#include "Engine/Scene/ColliderDebugDraw.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <tuple>
#include <variant>
#include <vector>

// The joints' debug view (M37, Docs/Decisions/0054-m37-joints.md decision 11): the colour of each state, the records of an
// edit scene (from the components, refusals included) and of a play session (where the bodies hold the joints, at the
// view's alpha), their lines, and the Colliders flag's hook in AppendRenderAnnotations.

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
			// None is a collider category's (the W37.2 review's finding 7), so a joint on its body's outline stands out.
			for (const ColliderDebugCategory category : { ColliderDebugCategory::Static, ColliderDebugCategory::Kinematic, ColliderDebugCategory::Dynamic,
					 ColliderDebugCategory::Sleeping, ColliderDebugCategory::Trigger, ColliderDebugCategory::Character, ColliderDebugCategory::Invalid })
			{
				const glm::vec4 colour = GetColliderDebugColor(category);
				CHECK_FALSE(colours.contains({ colour.r, colour.g, colour.b, colour.a }));
			}
		}

		TEST_CASE("JointDebugDraw: an edit scene draws each joint where its components place it, in its state's colour")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddJointedScene(scene);
			// The W37.2 review's finding 3: what the session would refuse though the composition accepts it. An axis of 1e-5
			// on a child scaled 0.01 vanishes in the world (the session's "frame"), and an anchor 1e9 m out lies beyond
			// MaxPhysicsCoordinate (the world's refusal).
			Entity body = Test::AddBoxBody(scene, "Body", glm::vec3(20.0f, 2.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			Entity faint = scene.CreateEntity("Faint");
			REQUIRE(scene.SetParent(faint, body, {}, false).has_value());
			faint.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(0.01f);
			});
			faint.AddComponent<HingeJointComponent>(HingeJointComponent{ .Axis = glm::vec3(1.0e-5f, 0.0f, 0.0f) });
			Entity far = Test::AddBoxBody(scene, "Far", glm::vec3(1000.0f, 2.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
			far.AddComponent<FixedJointComponent>(FixedJointComponent{ .Anchor = glm::vec3(MaxPhysicsCoordinate, 0.0f, 0.0f) });
			const std::vector<JointDebugShape> shapes = BuildJointDebugDraw(scene, PhysicsLayerTable(), nullptr);
			REQUIRE(shapes.size() == 6);
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

			const JointDebugShape* faintShape = FindJointShape(shapes, Test::GetEntityId(scene, "/Body/Faint"));
			REQUIRE(faintShape != nullptr);
			CHECK(faintShape->State == JointDebugState::Refused);
			CHECK(IsNear(faintShape->Point, glm::vec3(20.0f, 2.0f, 0.0f)));
			const JointDebugShape* farShape = FindJointShape(shapes, Test::GetEntityId(scene, "/Far"));
			REQUIRE(farShape != nullptr);
			CHECK(farShape->State == JointDebugState::Refused);
		}

		TEST_CASE("JointDebugDraw: a scene without a joint component draws no joint")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));
			CHECK(BuildJointDebugDraw(scene, PhysicsLayerTable(), nullptr).empty());
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

		TEST_CASE("JointDebugDraw: a play view draws a joint in force where its body's mesh is drawn, at the view's alpha")
		{
			// The W37.2 review's finding 2: between two fixed steps a joint in force sits at its body's rendered pose, as the
			// body's collider record does (BuildColliderDebugDraw at the same alpha), not up to a step ahead of it.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 50.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
			{
				body.GravityFactor = 0.0f;
				body.LinearDamping = 0.0f;
				body.InitialLinearVelocity = glm::vec3(12.0f, 0.0f, 0.0f);
			});
			// A slack rope to a point 10 m above: in force, and it never pulls.
			ball.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedAnchor = glm::vec3(0.0f, 60.0f, 0.0f), .MaxDistance = 1000.0f });
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE(session.has_value());
			Test::RunTicks(**session, 2);
			constexpr float ViewAlpha = 0.5f;
			(*session)->FixedStep();
			(*session)->FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 120.0, .UnscaledDeltaTime = 1.0 / 120.0, .Alpha = ViewAlpha, .FrameIndex = 3 });
			const Scene& runtime = (*session)->GetScene();
			const PhysicsSystem& physics = (*session)->GetPhysics();
			const UUID id = Test::GetEntityId(runtime, "/Ball");

			const std::vector<ColliderDebugShape> colliders = BuildColliderDebugDraw(runtime, PhysicsLayerTable(), &physics, nullptr, { .Alpha = ViewAlpha });
			REQUIRE(colliders.size() == 1);
			const std::vector<JointDebugShape> atView = BuildJointDebugDraw(runtime, PhysicsLayerTable(), &physics, ViewAlpha);
			const JointDebugShape* rope = FindJointShape(atView, id);
			REQUIRE(rope != nullptr);
			CHECK(rope->State == JointDebugState::InForce);
			CHECK(IsNear(rope->Point, colliders[0].Position));
			// The rope's other end is the world's: it does not move.
			CHECK(IsNear(rope->ConnectedPoint, glm::vec3(0.0f, 60.0f, 0.0f)));
			// At alpha 1 the joint sits where the bodies hold it, half a step (0.1 m at 12 m/s and 60 Hz) ahead of the drawn ball.
			const std::optional<PhysicsJointReport> report = physics.GetJointInfo(id);
			REQUIRE(report.has_value());
			const std::vector<JointDebugShape> stepped = BuildJointDebugDraw(runtime, PhysicsLayerTable(), &physics);
			const JointDebugShape* atStep = FindJointShape(stepped, id);
			REQUIRE(atStep != nullptr);
			CHECK(IsNear(atStep->Point, report->Point));
			CHECK(report->Point.x - rope->Point.x == doctest::Approx(0.1f).epsilon(0.01));
		}

		TEST_CASE("RenderAnnotations: the Colliders flag appends each joint's lines after the colliders")
		{
			// The W37.2 review's finding 2: the view's way into a frame (AppendRenderAnnotations, as the editor's collider view
			// and viewport.screenshot's colliders annotation call it).
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(1.0f, 2.0f, 0.0f), glm::vec3(0.5f, 1.0f, 0.05f), BodyType::Dynamic);
			door.AddComponent<HingeJointComponent>(HingeJointComponent{ .Anchor = glm::vec3(-0.5f, 0.0f, 0.0f), .Axis = glm::vec3(0.0f, 1.0f, 0.0f) });
			Entity camera = scene.CreateEntity("Camera");
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 2.0f, 10.0f);
			});
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			TransformSystem::Update(scene);
			Test::InMemoryAssetManager assets;

			DebugDrawList collidersOnly;
			AppendColliderDebugDraw(BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, &assets), collidersOnly);
			REQUIRE(collidersOnly.GetSize() > 0);
			for (const bool shown : { false, true })
			{
				CAPTURE(shown);
				RenderExtractionRequest request{ .Width = 64, .Height = 48 };
				request.Flags = shown ? RenderViewFlags::Colliders : RenderViewFlags::None;
				Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, request);
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				REQUIRE(AppendRenderAnnotations(scene, assets, PhysicsLayerTable(), nullptr, *snapshot).has_value());
				// The hinge: three cross lines and its axis, after the colliders' commands, in the joint's colour.
				CHECK(snapshot->AnnotationDraw.GetSize() == (shown ? collidersOnly.GetSize() + 4 : 0));
				if (shown)
				{
					const auto commands = snapshot->AnnotationDraw.GetCommands();
					for (size_t index = collidersOnly.GetSize(); index < commands.size(); ++index)
					{
						CHECK(std::holds_alternative<DebugLine>(commands[index].Shape));
						CHECK(commands[index].Color == GetJointDebugColor(JointDebugState::InForce));
						CHECK(commands[index].Depth == DebugDepthMode::OnTop);
					}
				}
			}
		}
	}

}
