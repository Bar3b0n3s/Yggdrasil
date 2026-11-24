#include "TestsPCH.h"

#include "Engine/Scene/PhysicsVehicles.h"

#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/VehicleWheelComponent.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

// The vehicle composition rules (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Architecture §5.3, §9.11): which
// body and wheels a WheeledVehicle has, PHYSICS_VEHICLE_INVALID with one reason per vehicle, and the components read in the
// world's units.

namespace Engine {

	namespace {

		PhysicsVehicleComposition Compose(const Scene& scene)
		{
			return ComposePhysicsVehicles(scene, ComposePhysicsBodies(scene, PhysicsLayerTable()));
		}

		const PhysicsVehiclePlan* FindVehicle(const PhysicsVehicleComposition& composition, UUID entity)
		{
			for (const PhysicsVehiclePlan& plan : composition.Vehicles)
			{
				if (plan.Entity == entity)
					return &plan;
			}
			return nullptr;
		}

		// The subjects of the PHYSICS_VEHICLE_INVALID diagnostics on `entity`.
		std::vector<std::string> InvalidSubjects(const PhysicsVehicleComposition& composition, UUID entity)
		{
			std::vector<std::string> subjects;
			for (const PhysicsDiagnostic& diagnostic : composition.Diagnostics)
			{
				if (diagnostic.Entity == entity && diagnostic.Code == PhysicsVehicleInvalidCode)
					subjects.push_back(diagnostic.Subject);
			}
			return subjects;
		}

		Entity AddWheel(Scene& scene, Entity parent, std::string_view name, const glm::vec3& position)
		{
			Entity wheel = scene.CreateEntity(name, parent);
			wheel.Patch<TransformComponent>([&position](TransformComponent& transform)
			{
				transform.Translation = position;
			});
			wheel.AddComponent<VehicleWheelComponent>();
			return wheel;
		}

		// A Dynamic box with a WheeledVehicle (patched by `patch`) and two wheels, Left and Right, below it, driven through one
		// differential.
		Entity AddCar(Scene& scene, std::string_view name, float x, const std::function<void(Scene&, Entity, WheeledVehicleComponent&)>& patch = {})
		{
			Entity body = Test::AddBoxBody(scene, name, glm::vec3(x, 1.0f, 0.0f), glm::vec3(0.5f, 0.2f, 1.0f), BodyType::Dynamic);
			const Entity left = AddWheel(scene, body, "Left", glm::vec3(-0.5f, -0.5f, 0.0f));
			const Entity right = AddWheel(scene, body, "Right", glm::vec3(0.5f, -0.5f, 0.0f));
			WheeledVehicleComponent vehicle;
			vehicle.Differentials = { VehicleDifferential{ .LeftWheel = left.GetUUID(), .RightWheel = right.GetUUID() } };
			if (patch)
				patch(scene, body, vehicle);
			body.AddComponent<WheeledVehicleComponent>(vehicle);
			return body;
		}

		UUID Child(Entity parent, size_t index)
		{
			return parent.GetChildren()[index];
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsVehicles: a vehicle drives its own Dynamic body on the VehicleWheel entities below it, in canonical order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity car = AddCar(scene, "Car", 0.0f);
			// A wheel two levels down (under a plain axle entity) is the car's too, after its own two in canonical order; a wheel
			// under a second vehicle below the car belongs to that one.
			Entity axle = scene.CreateEntity("Axle", car);
			Entity deep = AddWheel(scene, axle, "Deep", glm::vec3(0.0f, -0.5f, 1.0f));
			Entity trailer = Test::AddBoxBody(scene, "Trailer", glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.5f), BodyType::Dynamic, car);
			Entity trailerWheel = AddWheel(scene, trailer, "TrailerWheel", glm::vec3(0.0f, -0.6f, 0.0f));
			WheeledVehicleComponent trailerVehicle;
			trailerVehicle.Transmission = VehicleTransmissionMode::Manual;
			trailerVehicle.Differentials = { VehicleDifferential{ .LeftWheel = trailerWheel.GetUUID() } };
			trailer.AddComponent<WheeledVehicleComponent>(trailerVehicle);

			const PhysicsVehicleComposition composition = Compose(scene);
			CHECK(composition.Diagnostics.empty());
			const PhysicsVehiclePlan* plan = FindVehicle(composition, car.GetUUID());
			REQUIRE(plan != nullptr);
			CHECK(plan->IsCreatable);
			CHECK(plan->Body == car.GetUUID());
			CHECK(plan->Wheels == std::vector<UUID>{ Child(car, 0), Child(car, 1), deep.GetUUID() });
			const PhysicsVehiclePlan* other = FindVehicle(composition, trailer.GetUUID());
			REQUIRE(other != nullptr);
			CHECK(other->Wheels == std::vector<UUID>{ trailerWheel.GetUUID() });
			CHECK(FindPhysicsVehicleEntity(scene.FindEntityByID(deep.GetUUID())) == scene.FindEntityByID(car.GetUUID()));
			CHECK(FindPhysicsVehicleEntity(scene.FindEntityByID(car.GetUUID())) == scene.FindEntityByID(car.GetUUID()));
			CHECK_FALSE(FindPhysicsVehicleEntity(scene.FindEntityByID(axle.GetUUID())).IsValid());

			// A disabled wheel is no wheel, and a disabled vehicle is not listed.
			scene.FindEntityByID(deep.GetUUID()).AddComponent<DisabledTag>();
			trailer.AddComponent<DisabledTag>();
			const PhysicsVehicleComposition disabled = Compose(scene);
			REQUIRE(FindVehicle(disabled, car.GetUUID()) != nullptr);
			CHECK(FindVehicle(disabled, car.GetUUID())->Wheels.size() == 2);
			CHECK(FindVehicle(disabled, trailer.GetUUID()) == nullptr);
		}

		TEST_CASE("PhysicsVehicles: each vehicle that cannot be made gets one PHYSICS_VEHICLE_INVALID naming its reason")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const auto withVehicle = [&scene](std::string_view name, float x, const std::function<void(WheeledVehicleComponent&)>& patch)
			{
				return AddCar(scene, name, x, [&patch](Scene&, Entity, WheeledVehicleComponent& vehicle)
				{
					patch(vehicle);
				});
			};
			const auto patchWheel = [](Entity car, const std::function<void(VehicleWheelComponent&)>& patch)
			{
				car.GetScene()->FindEntityByID(Child(car, 0)).Patch<VehicleWheelComponent>([&patch](VehicleWheelComponent& wheel)
				{
					patch(wheel);
				});
			};
			std::vector<std::pair<Entity, std::string>> expected;

			// body: no collider; Static; a trigger.
			Entity bare = scene.CreateEntity("Bare");
			bare.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Dynamic });
			static_cast<void>(AddWheel(scene, bare, "Wheel", glm::vec3(0.0f, -0.5f, 0.0f)));
			bare.AddComponent<WheeledVehicleComponent>();
			expected.emplace_back(bare, "body");
			Entity wall = Test::AddBoxBody(scene, "Wall", glm::vec3(10.0f, 1.0f, 0.0f), glm::vec3(0.5f), BodyType::Static);
			static_cast<void>(AddWheel(scene, wall, "Wheel", glm::vec3(0.0f, -0.5f, 0.0f)));
			wall.AddComponent<WheeledVehicleComponent>();
			expected.emplace_back(wall, "body");
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(20.0f, 1.0f, 0.0f), glm::vec3(0.5f), BodyType::Kinematic);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& box)
			{
				box.IsTrigger = true;
			});
			static_cast<void>(AddWheel(scene, zone, "Wheel", glm::vec3(0.0f, -0.5f, 0.0f)));
			zone.AddComponent<WheeledVehicleComponent>();
			expected.emplace_back(zone, "body");
			// wheels: none; seventeen.
			Entity sled = Test::AddBoxBody(scene, "Sled", glm::vec3(30.0f, 1.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			sled.AddComponent<WheeledVehicleComponent>();
			expected.emplace_back(sled, "wheels");
			Entity millipede = Test::AddBoxBody(scene, "Millipede", glm::vec3(40.0f, 1.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			for (int index = 0; index < 17; ++index)
				static_cast<void>(AddWheel(scene, millipede, std::format("Wheel{}", index), glm::vec3(0.0f, -0.5f, static_cast<float>(index) * 0.1f)));
			millipede.AddComponent<WheeledVehicleComponent>();
			expected.emplace_back(millipede, "wheels");
			// wheel: one under a RigidBody of its own, one under a CharacterController, one on the vehicle's own entity.
			Entity carried = AddCar(scene, "Carried", 50.0f);
			Entity cargo = Test::AddBoxBody(scene, "Cargo", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.2f), BodyType::Dynamic, carried);
			static_cast<void>(AddWheel(scene, cargo, "Spare", glm::vec3(0.0f)));
			expected.emplace_back(carried, "wheel");
			Entity ridden = AddCar(scene, "Ridden", 60.0f);
			Entity rider = scene.CreateEntity("Rider", ridden);
			rider.AddComponent<CharacterControllerComponent>();
			static_cast<void>(AddWheel(scene, rider, "Wheel", glm::vec3(0.0f)));
			expected.emplace_back(ridden, "wheel");
			Entity self = AddCar(scene, "Self", 70.0f);
			self.AddComponent<VehicleWheelComponent>();
			expected.emplace_back(self, "wheel");
			// collider: a solid collider on a wheel, gathered into the body; a trigger there is not one.
			Entity bumpy = AddCar(scene, "Bumpy", 80.0f);
			scene.FindEntityByID(Child(bumpy, 0)).AddComponent<BoxColliderComponent>();
			expected.emplace_back(bumpy, "collider");
			Entity sensing = AddCar(scene, "Sensing", 90.0f);
			scene.FindEntityByID(Child(sensing, 0)).AddComponent<BoxColliderComponent>(BoxColliderComponent{ .IsTrigger = true });
			// suspension: min above max; no spring.
			Entity sagging = AddCar(scene, "Sagging", 100.0f);
			patchWheel(sagging, [](VehicleWheelComponent& wheel)
			{
				wheel.SuspensionMinLength = 0.6f;
			});
			expected.emplace_back(sagging, "suspension");
			Entity springless = AddCar(scene, "Springless", 110.0f);
			patchWheel(springless, [](VehicleWheelComponent& wheel)
			{
				wheel.SpringFrequency = 0.0f;
			});
			expected.emplace_back(springless, "suspension");
			// engine.
			expected.emplace_back(withVehicle("Revving", 120.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.MinRpm = 7000.0f;
			}),
				"engine");
			// transmission: no forward gear; a reverse ratio above 0; ShiftUpRpm at ShiftDownRpm; an Auto ShiftUpRpm at MaxRpm; no
			// clutch; nine gears.
			expected.emplace_back(withVehicle("Gearless", 130.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.GearRatios.clear();
			}),
				"transmission");
			expected.emplace_back(withVehicle("Backward", 140.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.ReverseGearRatios = { 2.0f };
			}),
				"transmission");
			expected.emplace_back(withVehicle("Hunting", 150.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.ShiftUpRpm = vehicle.ShiftDownRpm;
			}),
				"transmission");
			expected.emplace_back(withVehicle("Limited", 160.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.ShiftUpRpm = vehicle.MaxRpm;
			}),
				"transmission");
			expected.emplace_back(withVehicle("Slipping", 170.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.ClutchStrength = 0.0f;
			}),
				"transmission");
			expected.emplace_back(withVehicle("Geared", 180.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.GearRatios.assign(9, 1.0f);
			}),
				"transmission");
			// A Manual transmission may shift up anywhere: its ShiftUpRpm is not used.
			Entity manual = withVehicle("Manual", 190.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.Transmission = VehicleTransmissionMode::Manual;
				vehicle.ShiftUpRpm = vehicle.MaxRpm;
			});
			// differential: a wheel of another vehicle; one wheel twice; torque shares that do not sum to 1; a limited slip in
			// (0, 1].
			Entity foreign = AddCar(scene, "Foreign", 200.0f, [&manual](Scene&, Entity, WheeledVehicleComponent& vehicle)
			{
				vehicle.Differentials = { VehicleDifferential{ .LeftWheel = Child(manual, 0) } };
			});
			expected.emplace_back(foreign, "differential");
			Entity twice = AddCar(scene, "Twice", 210.0f);
			twice.Patch<WheeledVehicleComponent>([&twice](WheeledVehicleComponent& vehicle)
			{
				vehicle.Differentials = { VehicleDifferential{ .LeftWheel = Child(twice, 0), .RightWheel = Child(twice, 0) } };
			});
			expected.emplace_back(twice, "differential");
			Entity shared = AddCar(scene, "Shared", 220.0f);
			shared.Patch<WheeledVehicleComponent>([&shared](WheeledVehicleComponent& vehicle)
			{
				vehicle.Differentials = { VehicleDifferential{ .LeftWheel = Child(shared, 0), .EngineTorqueRatio = 0.5f },
					VehicleDifferential{ .RightWheel = Child(shared, 1), .EngineTorqueRatio = 0.4f } };
			});
			expected.emplace_back(shared, "differential");
			expected.emplace_back(withVehicle("Locked", 230.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.DifferentialLimitedSlipRatio = 0.5f;
			}),
				"differential");
			// differential: none (the component's default), and one that names no wheel (Jolt drives the wheels through one); three
			// thirds that sum to 1 only within 1e-4, as Jolt sums them in float.
			expected.emplace_back(withVehicle("Undriven", 232.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.Differentials.clear();
			}),
				"differential");
			expected.emplace_back(withVehicle("Idle", 234.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.Differentials = { VehicleDifferential{} };
			}),
				"differential");
			Entity thirds = AddCar(scene, "Thirds", 236.0f);
			thirds.Patch<WheeledVehicleComponent>([&thirds](WheeledVehicleComponent& vehicle)
			{
				vehicle.Differentials = { VehicleDifferential{ .LeftWheel = Child(thirds, 0), .EngineTorqueRatio = 0.3333f },
					VehicleDifferential{ .RightWheel = Child(thirds, 1), .EngineTorqueRatio = 0.3333f },
					VehicleDifferential{ .LeftWheel = Child(thirds, 0), .RightWheel = Child(thirds, 1), .EngineTorqueRatio = 0.3333f } };
			});
			expected.emplace_back(thirds, "differential");
			// bar: a null side; one wheel twice.
			Entity halfBar = AddCar(scene, "HalfBar", 240.0f);
			halfBar.Patch<WheeledVehicleComponent>([&halfBar](WheeledVehicleComponent& vehicle)
			{
				vehicle.AntiRollBars = { VehicleAntiRollBar{ .LeftWheel = Child(halfBar, 0) } };
			});
			expected.emplace_back(halfBar, "bar");
			Entity loop = AddCar(scene, "Loop", 250.0f);
			loop.Patch<WheeledVehicleComponent>([&loop](WheeledVehicleComponent& vehicle)
			{
				vehicle.AntiRollBars = { VehicleAntiRollBar{ .LeftWheel = Child(loop, 1), .RightWheel = Child(loop, 1) } };
			});
			expected.emplace_back(loop, "bar");
			// curve: a torque curve past X = 1; a tyre curve whose X falls; nine keys.
			expected.emplace_back(withVehicle("Overrev", 260.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.TorqueCurve.push_back(VehicleCurveKey{ .X = 1.5f, .Y = 0.5f });
			}),
				"curve");
			Entity slick = AddCar(scene, "Slick", 270.0f);
			patchWheel(slick, [](VehicleWheelComponent& wheel)
			{
				wheel.LateralFriction = { VehicleCurveKey{ .X = 5.0f, .Y = 1.0f }, VehicleCurveKey{ .X = 2.0f, .Y = 1.0f } };
			});
			expected.emplace_back(slick, "curve");
			Entity dense = AddCar(scene, "Dense", 280.0f);
			patchWheel(dense, [](VehicleWheelComponent& wheel)
			{
				wheel.LongitudinalFriction.clear();
				for (int key = 0; key < 9; ++key)
					wheel.LongitudinalFriction.push_back(VehicleCurveKey{ .X = static_cast<float>(key), .Y = 1.0f });
			});
			expected.emplace_back(dense, "curve");
			// Several reasons at once: only the first, in the rules' order.
			Entity broken = withVehicle("Broken", 290.0f, [](WheeledVehicleComponent& vehicle)
			{
				vehicle.MinRpm = 7000.0f;
				vehicle.GearRatios.clear();
			});
			patchWheel(broken, [](VehicleWheelComponent& wheel)
			{
				wheel.SuspensionMinLength = 0.6f;
			});
			expected.emplace_back(broken, "suspension");

			const PhysicsVehicleComposition composition = Compose(scene);
			for (const auto& [entity, subject] : expected)
			{
				INFO(entity.GetName());
				CHECK(InvalidSubjects(composition, entity.GetUUID()) == std::vector<std::string>{ subject });
				const PhysicsVehiclePlan* plan = FindVehicle(composition, entity.GetUUID());
				REQUIRE(plan != nullptr);
				CHECK_FALSE(plan->IsCreatable);
			}
			CHECK(composition.Diagnostics.size() == expected.size());
			for (const Entity valid : { sensing, manual })
			{
				INFO(valid.GetName());
				CHECK(InvalidSubjects(composition, valid.GetUUID()).empty());
				REQUIRE(FindVehicle(composition, valid.GetUUID()) != nullptr);
				CHECK(FindVehicle(composition, valid.GetUUID())->IsCreatable);
			}
			for (const PhysicsDiagnostic& diagnostic : composition.Diagnostics)
			{
				CHECK(diagnostic.Severity == DiagnosticSeverity::Error);
				CHECK(diagnostic.Component == "WheeledVehicle");
				CHECK_FALSE(diagnostic.Hint.empty());
			}
			CHECK(std::is_sorted(composition.Diagnostics.begin(), composition.Diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
			{
				return a.Entity < b.Entity;
			}));
		}

		TEST_CASE("PhysicsVehicles: a reference to a disabled wheel is no wheel, and makes no vehicle invalid")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity car = AddCar(scene, "Car", 0.0f);
			car.Patch<WheeledVehicleComponent>([&car](WheeledVehicleComponent& vehicle)
			{
				vehicle.Differentials = { VehicleDifferential{ .LeftWheel = Child(car, 0), .RightWheel = Child(car, 1) } };
				vehicle.AntiRollBars = { VehicleAntiRollBar{ .LeftWheel = Child(car, 0), .RightWheel = Child(car, 1) } };
			});
			scene.FindEntityByID(Child(car, 0)).AddComponent<DisabledTag>();
			const PhysicsVehicleComposition composition = Compose(scene);
			CHECK(composition.Diagnostics.empty());
			const PhysicsVehiclePlan* plan = FindVehicle(composition, car.GetUUID());
			REQUIRE(plan != nullptr);
			REQUIRE(plan->Wheels == std::vector<UUID>{ Child(car, 1) });
			const PhysicsVehicleSettings settings = MakePhysicsVehicleSettings(car.GetComponent<WheeledVehicleComponent>(), plan->Wheels);
			REQUIRE(settings.Differentials.size() == 1);
			CHECK(settings.Differentials[0].LeftWheel == -1);
			CHECK(settings.Differentials[0].RightWheel == 0);
			CHECK(settings.AntiRollBars.empty());
		}

		TEST_CASE("PhysicsVehicles: the components read in the world's units")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity car = AddCar(scene, "Car", 0.0f);
			const std::vector<UUID> wheels = { Child(car, 0), Child(car, 1) };
			WheeledVehicleComponent component;
			component.Throttle = 0.5f;
			component.Steering = -0.25f;
			component.Brake = 0.75f;
			component.HandBrake = 1.0f;
			component.Gear = -1;
			component.Clutch = 0.5f;
			component.Transmission = VehicleTransmissionMode::Manual;
			component.TorqueCurve = { VehicleCurveKey{ .X = 0.0f, .Y = 1.0f }, VehicleCurveKey{ .X = 1.0f, .Y = 0.0f } };
			component.Differentials = { VehicleDifferential{ .LeftWheel = wheels[1], .RightWheel = UUID(), .Ratio = 4.0f, .LimitedSlipRatio = 0.0f } };
			component.AntiRollBars = { VehicleAntiRollBar{ .LeftWheel = wheels[0], .RightWheel = wheels[1], .Stiffness = 5000.0f } };
			component.MaxPitchRollAngle = 180.0f;
			component.WheelCast = VehicleWheelCast::Sphere;

			const PhysicsVehicleInput input = MakePhysicsVehicleInput(component);
			CHECK(input == PhysicsVehicleInput{ .Forward = 0.5f, .Right = -0.25f, .Brake = 0.75f, .HandBrake = 1.0f, .Gear = -1, .Clutch = 0.5f });
			CHECK(MakePhysicsVehicleCast(component) == PhysicsVehicleCast::Sphere);
			const PhysicsVehicleSettings settings = MakePhysicsVehicleSettings(component, wheels);
			CHECK(settings.Transmission.Mode == PhysicsVehicleTransmissionMode::Manual);
			CHECK(settings.Engine.TorqueCurve == std::vector<PhysicsVehicleCurveKey>{ { 0.0f, 1.0f }, { 1.0f, 0.0f } });
			REQUIRE(settings.Differentials.size() == 1);
			CHECK(settings.Differentials[0] == PhysicsVehicleDifferential{ .LeftWheel = 1, .RightWheel = -1, .Ratio = 4.0f, .LimitedSlipRatio = 0.0f });
			CHECK(settings.AntiRollBars == std::vector<PhysicsVehicleAntiRollBar>{ { .LeftWheel = 0, .RightWheel = 1, .Stiffness = 5000.0f } });
			// 180 degrees is the world's pi, never a float above it.
			CHECK(settings.MaxPitchRollAngle <= std::numbers::pi_v<float>);
			CHECK(settings.MaxPitchRollAngle == doctest::Approx(std::numbers::pi_v<float>));
			const WheeledVehicleComponent stripped = StripPhysicsVehicleInput(component);
			CHECK(MakePhysicsVehicleInput(stripped) == PhysicsVehicleInput{});
			CHECK(stripped.Transmission == VehicleTransmissionMode::Manual);

			// A wheel at its frame: its attachment point SuspensionMaxLength above its centre, 90 degrees of steer the world's
			// pi / 2.
			VehicleWheelComponent wheelComponent;
			wheelComponent.SuspensionMaxLength = 0.4f;
			wheelComponent.MaxSteerAngle = 90.0f;
			const PhysicsVehicleWheelFrame frame{ .Center = glm::vec3(1.0f, -0.5f, 2.0f) };
			const PhysicsVehicleWheelDescription wheel = MakePhysicsVehicleWheel(wheelComponent, frame);
			CHECK(wheel.Position.x == 1.0f);
			CHECK(wheel.Position.y == doctest::Approx(-0.1f));
			CHECK(wheel.Position.z == 2.0f);
			CHECK(wheel.MaxSteerAngle <= 0.5f * std::numbers::pi_v<float>);
			CHECK(wheel.MaxSteerAngle == doctest::Approx(0.5f * std::numbers::pi_v<float>));
		}

		TEST_CASE("PhysicsVehicles: a wheel's frame is its pose in the body's space, the vehicle's scale included")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity car = AddCar(scene, "Car", 0.0f);
			car.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(5.0f, 3.0f, -2.0f);
				transform.Rotation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
				transform.Scale = glm::vec3(2.0f);
			});
			// A wheel under an axle 1 m back, toed in 10 degrees about Y.
			Entity axle = scene.CreateEntity("Axle", car);
			axle.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 0.0f, 1.0f);
			});
			Entity wheel = AddWheel(scene, axle, "Wheel", glm::vec3(0.5f, -0.25f, 0.0f));
			wheel.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(10.0f), glm::vec3(0.0f, 1.0f, 0.0f));
				transform.Scale = glm::vec3(3.0f);
			});
			const std::optional<PhysicsVehicleWheelFrame> frame = MakePhysicsVehicleWheelFrame(car, wheel);
			REQUIRE(frame.has_value());
			// The car's rotation and position are the body's own; its scale doubles the offsets; the wheel's own scale is not
			// part of it.
			CHECK(frame->Center.x == doctest::Approx(1.0f));
			CHECK(frame->Center.y == doctest::Approx(-0.5f));
			CHECK(frame->Center.z == doctest::Approx(2.0f));
			CHECK(frame->Up.y == doctest::Approx(1.0f));
			const glm::vec3 forward = glm::angleAxis(glm::radians(10.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * glm::vec3(0.0f, 0.0f, -1.0f);
			CHECK(frame->Forward.x == doctest::Approx(forward.x));
			CHECK(frame->Forward.z == doctest::Approx(forward.z));
			CHECK(GetPhysicsVehicleWheelChain(car, wheel) == std::vector<UUID>{ wheel.GetUUID(), axle.GetUUID(), car.GetUUID() });
		}
	}

}
