#include "TestsPCH.h"

#include "Engine/Scene/VehicleDebugDraw.h"

#include "Engine/Core/Time.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scene/ColliderDebugDraw.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/VehicleWheelComponent.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"
#include "Engine/Scene/JointDebugDraw.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/PhysicsTestScene.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

// The vehicles' debug view (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3 "The editor"): the colour of each
// state, the records of an edit scene (from the components, refusals included) and of a play session (where the session placed
// the wheels, with their suspension and contact), their lines, and the Colliders flag's hook in AppendRenderAnnotations.

namespace Engine {

	namespace {

		constexpr std::array<std::string_view, 4> DebugWheelNames = { "FL", "FR", "RL", "RR" };

		// A 1200 kg buggy (a 1.8 x 0.5 x 4 m box) at `position` on four wheels of radius 0.4 m, each hanging 0.5 m below the
		// chassis's centre height; the rear ones are driven, unless `differential` is false (PHYSICS_VEHICLE_INVALID).
		Entity AddDebugBuggy(Scene& scene, std::string_view name, const glm::vec3& position, bool differential = true)
		{
			Entity body = Test::AddBoxBody(scene, name, position, glm::vec3(0.9f, 0.25f, 2.0f), BodyType::Dynamic);
			Test::PatchRigidBody(body, [](RigidBodyComponent& rigidBody)
			{
				rigidBody.Mass = 1200.0f;
			});
			std::array<UUID, 4> wheels{};
			for (size_t index = 0; index < DebugWheelNames.size(); ++index)
			{
				Entity wheel = scene.CreateEntity(DebugWheelNames[index], body);
				wheel.Patch<TransformComponent>([index](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(index % 2 == 0 ? -0.9f : 0.9f, -0.5f, index < 2 ? -1.4f : 1.4f);
				});
				wheel.AddComponent<VehicleWheelComponent>(VehicleWheelComponent{ .Radius = 0.4f, .Width = 0.3f });
				wheels[index] = wheel.GetUUID();
			}
			WheeledVehicleComponent vehicle;
			if (differential)
				vehicle.Differentials = { VehicleDifferential{ .LeftWheel = wheels[2], .RightWheel = wheels[3] } };
			body.AddComponent<WheeledVehicleComponent>(vehicle);
			return body;
		}

		const VehicleWheelDebugShape* FindWheelShape(const std::vector<VehicleWheelDebugShape>& shapes, UUID wheel)
		{
			const auto found = std::find_if(shapes.begin(), shapes.end(), [wheel](const VehicleWheelDebugShape& shape)
			{
				return shape.Wheel == wheel;
			});
			return found == shapes.end() ? nullptr : &*found;
		}

		bool IsNearVector(const glm::vec3& a, const glm::vec3& b, float tolerance = 1.0e-3f)
		{
			return glm::length(a - b) < tolerance;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("VehicleDebugDraw: every state has its own colour, none a collider's or a joint's")
		{
			std::set<std::tuple<float, float, float, float>> colours;
			for (const VehicleDebugState state : { VehicleDebugState::InForce, VehicleDebugState::Refused })
			{
				const glm::vec4 colour = GetVehicleDebugColor(state);
				CHECK(colour.a == 1.0f);
				colours.insert({ colour.r, colour.g, colour.b, colour.a });
			}
			CHECK(colours.size() == 2);
			for (const ColliderDebugCategory category : { ColliderDebugCategory::Static, ColliderDebugCategory::Kinematic, ColliderDebugCategory::Dynamic,
					 ColliderDebugCategory::Sleeping, ColliderDebugCategory::Trigger, ColliderDebugCategory::Character, ColliderDebugCategory::Invalid })
			{
				const glm::vec4 colour = GetColliderDebugColor(category);
				CHECK_FALSE(colours.contains({ colour.r, colour.g, colour.b, colour.a }));
			}
			for (const JointDebugState state : { JointDebugState::InForce, JointDebugState::Broken, JointDebugState::Refused })
			{
				const glm::vec4 colour = GetJointDebugColor(state);
				CHECK_FALSE(colours.contains({ colour.r, colour.g, colour.b, colour.a }));
			}
		}

		TEST_CASE("VehicleDebugDraw: an edit scene draws each wheel hanging at its authored centre, in its vehicle's state's colour")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddDebugBuggy(scene, "Buggy", glm::vec3(0.0f, 1.0f, 0.0f)));
			Entity refused = AddDebugBuggy(scene, "Refused", glm::vec3(10.0f, 1.0f, 0.0f), false);
			// A quarter turn about Y: the body's axes turn with it, its up stays +Y.
			refused.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			});
			TransformSystem::Update(scene);
			const std::vector<VehicleWheelDebugShape> shapes = BuildVehicleDebugDraw(scene, PhysicsLayerTable(), nullptr);
			REQUIRE(shapes.size() == 8);
			// Vehicle by vehicle in canonical order, each in its wheel order.
			CHECK(shapes[0].Wheel == Test::GetEntityId(scene, "/Buggy/FL"));
			CHECK(shapes[3].Wheel == Test::GetEntityId(scene, "/Buggy/RR"));
			CHECK(shapes[4].Vehicle == refused.GetUUID());

			const VehicleWheelDebugShape* front = FindWheelShape(shapes, Test::GetEntityId(scene, "/Buggy/FL"));
			REQUIRE(front != nullptr);
			CHECK(front->State == VehicleDebugState::InForce);
			CHECK(IsNearVector(front->Center, glm::vec3(-0.9f, 0.5f, -1.4f)));
			CHECK(IsNearVector(front->Axle, glm::vec3(1.0f, 0.0f, 0.0f)));
			CHECK(IsNearVector(front->Up, glm::vec3(0.0f, 1.0f, 0.0f)));
			CHECK(front->Radius == 0.4f);
			// The suspension runs from 0.5 m above the centre (SuspensionMaxLength) down to it: the wheel hangs at full droop.
			CHECK(IsNearVector(front->Attachment, glm::vec3(-0.9f, 1.0f, -1.4f)));
			CHECK(IsNearVector(front->SuspensionEnd, front->Center));
			CHECK_FALSE(front->Touching);

			// The refused buggy is drawn where it would hang, turned with its body.
			const VehicleWheelDebugShape* turned = FindWheelShape(shapes, Test::GetEntityId(scene, "/Refused/FL"));
			REQUIRE(turned != nullptr);
			CHECK(turned->State == VehicleDebugState::Refused);
			CHECK(IsNearVector(turned->Center, glm::vec3(10.0f - 1.4f, 0.5f, 0.9f)));
			CHECK(IsNearVector(turned->Axle, glm::vec3(0.0f, 0.0f, -1.0f)));
			CHECK(IsNearVector(turned->Attachment - turned->Center, glm::vec3(0.0f, 0.5f, 0.0f)));
		}

		TEST_CASE("VehicleDebugDraw: a scene without a WheeledVehicle draws no vehicle")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));
			Entity loose = scene.CreateEntity("Loose");
			loose.AddComponent<VehicleWheelComponent>();
			CHECK(BuildVehicleDebugDraw(scene, PhysicsLayerTable(), nullptr).empty());
		}

		TEST_CASE("VehicleDebugDraw: a play session draws each wheel at its suspension with its contact, and a vehicle it did not make as refused")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(AddDebugBuggy(scene, "Buggy", glm::vec3(0.0f, 1.0f, 0.0f)));
			static_cast<void>(AddDebugBuggy(scene, "Refused", glm::vec3(10.0f, 1.0f, 0.0f), false));
			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_VEHICLE_INVALID");
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE(session.has_value());
			Test::RunTicks(**session, 60);
			const Scene& runtime = (*session)->GetScene();
			const PhysicsSystem& physics = (*session)->GetPhysics();
			const std::vector<VehicleWheelDebugShape> shapes = BuildVehicleDebugDraw(runtime, PhysicsLayerTable(), &physics);
			REQUIRE(shapes.size() == 8);

			const std::optional<PhysicsVehicleReport> report = physics.GetVehicleInfo(Test::GetEntityId(runtime, "/Buggy"));
			REQUIRE(report.has_value());
			REQUIRE(report->Created);
			REQUIRE(report->Wheels.size() == 4);
			for (const PhysicsVehicleWheelReport& reading : report->Wheels)
			{
				const VehicleWheelDebugShape* wheel = FindWheelShape(shapes, reading.Entity);
				REQUIRE(wheel != nullptr);
				CHECK(wheel->State == VehicleDebugState::InForce);
				// The wheel where the session placed its entity, its spring compressed under the buggy's weight.
				const ConstEntity entity = runtime.FindEntityByID(reading.Entity);
				CHECK(IsNearVector(wheel->Center, glm::vec3(TransformSystem::ComputeWorldMatrix(entity)[3])));
				CHECK(reading.SuspensionLength < 0.5f);
				CHECK(glm::length(wheel->Attachment - wheel->Center) == doctest::Approx(reading.SuspensionLength).epsilon(1.0e-3));
				CHECK(glm::length(wheel->Attachment - wheel->SuspensionEnd) == doctest::Approx(0.5f).epsilon(1.0e-3));
				// It stands on the ground, at the wheel's bottom.
				REQUIRE(wheel->Touching);
				CHECK(IsNearVector(wheel->ContactPoint, reading.Point));
				CHECK(std::abs(wheel->ContactPoint.y) < 0.05f);
				CHECK(IsNearVector(wheel->ContactNormal, glm::vec3(0.0f, 1.0f, 0.0f), 0.01f));
				CHECK(wheel->Center.y - wheel->ContactPoint.y == doctest::Approx(0.4f).epsilon(0.05));
			}
			for (const std::string_view name : DebugWheelNames)
			{
				const VehicleWheelDebugShape* wheel = FindWheelShape(shapes, Test::GetEntityId(runtime, std::string("/Refused/") + std::string(name)));
				REQUIRE(wheel != nullptr);
				CHECK(wheel->State == VehicleDebugState::Refused);
				CHECK_FALSE(wheel->Touching);
			}
		}

		TEST_CASE("VehicleDebugDraw: a play view draws each wheel where its mesh is drawn, at the view's alpha")
		{
			// The W44.3 review's finding 1 (as M37's joint view): between two fixed steps a wheel is drawn at its entity's rendered
			// pose, and its contact moves with the vehicle's, not up to a step ahead of the meshes.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity buggy = AddDebugBuggy(scene, "Buggy", glm::vec3(0.0f, 1.0f, 0.0f));
			buggy.Patch<WheeledVehicleComponent>([](WheeledVehicleComponent& vehicle)
			{
				vehicle.Throttle = 1.0f;
			});
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE(session.has_value());
			Test::RunTicks(**session, 90);
			constexpr float ViewAlpha = 0.5f;
			(*session)->FixedStep();
			(*session)->FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 120.0, .UnscaledDeltaTime = 1.0 / 120.0, .Alpha = ViewAlpha, .FrameIndex = 92 });
			const Scene& runtime = (*session)->GetScene();
			const PhysicsSystem& physics = (*session)->GetPhysics();
			const std::optional<PhysicsVehicleReport> report = physics.GetVehicleInfo(Test::GetEntityId(runtime, "/Buggy"));
			REQUIRE(report.has_value());
			REQUIRE(report->Created);
			REQUIRE(report->Speed > 2.0f);
			// Half a step's travel at the buggy's speed.
			const float halfStep = report->Speed / 60.0f * 0.5f;
			const std::vector<VehicleWheelDebugShape> atView = BuildVehicleDebugDraw(runtime, PhysicsLayerTable(), &physics, ViewAlpha);
			const std::vector<VehicleWheelDebugShape> atStep = BuildVehicleDebugDraw(runtime, PhysicsLayerTable(), &physics);
			const ConstEntity vehicle = runtime.FindEntityByID(report->Entity);
			const glm::mat4 vehicleStep = TransformSystem::ComputeWorldMatrix(vehicle);
			const glm::mat4 vehicleView = ComputeRenderedWorldMatrix(vehicle, ViewAlpha);
			for (const PhysicsVehicleWheelReport& reading : report->Wheels)
			{
				const ConstEntity wheel = runtime.FindEntityByID(reading.Entity);
				INFO(wheel.GetName());
				const VehicleWheelDebugShape* view = FindWheelShape(atView, reading.Entity);
				const VehicleWheelDebugShape* step = FindWheelShape(atStep, reading.Entity);
				REQUIRE(view != nullptr);
				REQUIRE(step != nullptr);
				CHECK(IsNearVector(view->Center, glm::vec3(ComputeRenderedWorldMatrix(wheel, ViewAlpha)[3])));
				REQUIRE(view->Touching);
				const glm::vec3 local = glm::vec3(glm::affineInverse(vehicleStep) * glm::vec4(reading.Point, 1.0f));
				CHECK(IsNearVector(view->ContactPoint, glm::vec3(vehicleView * glm::vec4(local, 1.0f))));
				CHECK(IsNearVector(step->ContactPoint, reading.Point));
				// The view draws them about half a step behind the step's pose.
				CHECK(glm::length(step->Center - view->Center) == doctest::Approx(halfStep).epsilon(0.25));
				CHECK(glm::length(step->ContactPoint - view->ContactPoint) == doctest::Approx(halfStep).epsilon(0.25));
			}
		}

		TEST_CASE("VehicleDebugDraw: a wheel whose transform does not decompose is drawn at its entity, refused")
		{
			// The W44.3 review's finding 1: a wheel below an axle of Scale 0 has no frame; it is drawn at its entity's position in
			// the refused colour, while the vehicle's other wheels are drawn in force.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity buggy = AddDebugBuggy(scene, "Buggy", glm::vec3(0.0f, 1.0f, 0.0f));
			Entity axle = scene.CreateEntity("Axle", buggy);
			axle.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, -0.5f, 3.0f);
				transform.Scale = glm::vec3(0.0f);
			});
			Entity flat = scene.CreateEntity("Flat", axle);
			flat.AddComponent<VehicleWheelComponent>();
			TransformSystem::Update(scene);
			const std::vector<VehicleWheelDebugShape> shapes = BuildVehicleDebugDraw(scene, PhysicsLayerTable(), nullptr);
			REQUIRE(shapes.size() == 5);
			const VehicleWheelDebugShape* shape = FindWheelShape(shapes, flat.GetUUID());
			REQUIRE(shape != nullptr);
			CHECK(shape->State == VehicleDebugState::Refused);
			CHECK(IsNearVector(shape->Center, glm::vec3(TransformSystem::ComputeWorldMatrix(flat)[3])));
			CHECK(IsNearVector(shape->Center, glm::vec3(0.0f, 0.5f, 3.0f)));
			const VehicleWheelDebugShape* front = FindWheelShape(shapes, Test::GetEntityId(scene, "/Buggy/FL"));
			REQUIRE(front != nullptr);
			CHECK(front->State == VehicleDebugState::InForce);
		}

		TEST_CASE("VehicleDebugDraw: AppendVehicleDebugDraw draws the circle, the spoke, the suspension and the contact's normal on top")
		{
			VehicleWheelDebugShape hanging;
			hanging.Center = glm::vec3(1.0f, 2.0f, 3.0f);
			hanging.Radius = 0.5f;
			hanging.Attachment = glm::vec3(1.0f, 2.5f, 3.0f);
			hanging.SuspensionEnd = hanging.Center;
			VehicleWheelDebugShape standing = hanging;
			standing.State = VehicleDebugState::Refused;
			standing.Touching = true;
			standing.ContactPoint = glm::vec3(1.0f, 1.5f, 3.0f);
			standing.ContactNormal = glm::vec3(0.0f, 1.0f, 0.0f);
			const std::array<VehicleWheelDebugShape, 2> shapes = { hanging, standing };

			DebugDrawList list;
			AppendVehicleDebugDraw(shapes, list);
			const auto commands = list.GetCommands();
			constexpr size_t Hanging = VehicleDebugCircleSegments + 2;
			REQUIRE(commands.size() == Hanging * 2 + 1);
			for (size_t index = 0; index < commands.size(); ++index)
			{
				CAPTURE(index);
				const DebugDrawCommand& command = commands[index];
				REQUIRE(std::holds_alternative<DebugLine>(command.Shape));
				CHECK(command.Depth == DebugDepthMode::OnTop);
				CHECK(command.Duration == 0.0f);
				CHECK(command.Color == GetVehicleDebugColor(index < Hanging ? VehicleDebugState::InForce : VehicleDebugState::Refused));
			}
			// The circle's points lie on the rim, in the plane normal to the axle (+X).
			for (size_t index = 0; index < VehicleDebugCircleSegments; ++index)
			{
				const DebugLine& segment = std::get<DebugLine>(commands[index].Shape);
				CHECK(glm::length(segment.From - hanging.Center) == doctest::Approx(0.5f));
				CHECK(segment.From.x == doctest::Approx(1.0f));
			}
			const DebugLine& spoke = std::get<DebugLine>(commands[VehicleDebugCircleSegments].Shape);
			CHECK(spoke.From == hanging.Center);
			CHECK(IsNearVector(spoke.To, glm::vec3(1.0f, 2.5f, 3.0f)));
			const DebugLine& suspension = std::get<DebugLine>(commands[VehicleDebugCircleSegments + 1].Shape);
			CHECK(suspension.From == hanging.Attachment);
			CHECK(suspension.To == hanging.SuspensionEnd);
			const DebugLine& normal = std::get<DebugLine>(commands.back().Shape);
			CHECK(normal.From == standing.ContactPoint);
			CHECK(IsNearVector(normal.To, standing.ContactPoint + glm::vec3(0.0f, VehicleDebugNormalLength, 0.0f)));
		}

		TEST_CASE("RenderAnnotations: the Colliders flag appends each wheel's lines after the colliders and the joints")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddDebugBuggy(scene, "Buggy", glm::vec3(0.0f, 1.0f, 0.0f)));
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
				// Four wheels, each its circle, its spoke and its suspension, after the colliders' commands, in the vehicle's colour.
				constexpr size_t WheelLines = 4 * (VehicleDebugCircleSegments + 2);
				CHECK(snapshot->AnnotationDraw.GetSize() == (shown ? collidersOnly.GetSize() + WheelLines : 0));
				if (shown)
				{
					const auto commands = snapshot->AnnotationDraw.GetCommands();
					for (size_t index = collidersOnly.GetSize(); index < commands.size(); ++index)
					{
						CHECK(std::holds_alternative<DebugLine>(commands[index].Shape));
						CHECK(commands[index].Color == GetVehicleDebugColor(VehicleDebugState::InForce));
						CHECK(commands[index].Depth == DebugDepthMode::OnTop);
					}
				}
			}
		}
	}

}
