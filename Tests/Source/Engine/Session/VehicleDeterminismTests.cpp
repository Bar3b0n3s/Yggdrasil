#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/VehicleWheelComponent.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"
#include "Support/TestTimeout.h"

#include <array>
#include <format>
#include <string>
#include <string_view>

// Vehicle determinism through the play session's state hash (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decisions 3,
// 4.7 and 6; Architecture §9.7, §9.11): a convoy of twenty-four vehicles of every cast and transmission, driven by planned
// input over bumps, ends in one state at Jolt thread counts 0, 1 and 8 (Jolt steps vehicles in jobs of eight, so fewer would
// prove nothing), and in the committed state in every configuration (the Debug and Release unit runs).

namespace Engine {

	namespace {

		// The convoy's state hash after ConvoyTicks ticks, recorded from M44's W44.2 and verified equal in Debug and Release. A
		// change of physics or of the scene that legitimately changes the simulation updates it with the reason in the change's
		// review (§13.6 "Re-recording"). Re-recorded by the W44.2 review's fixes: the hash's vehicle part appends each placed
		// wheel's authored pose (finding 1); the simulation itself is unchanged.
		constexpr uint64_t CommittedConvoyHash = 0xf3cdac7de44dc587;

		constexpr uint32_t ConvoyTicks = 300;
		constexpr int ConvoySize = 24;

		// Vehicle `index` of the convoy: an Auto buggy on cylinders, a Manual cart on rays or an Auto rig on spheres, by index
		// modulo 3, in a grid of six columns 8 m apart and four rows 12 m apart, each on four wheels with its rear driven.
		void AddConvoyVehicle(Scene& scene, int index)
		{
			const int kind = index % 3;
			Entity body = Test::AddBoxBody(scene, std::format("Vehicle{}", index),
				glm::vec3(static_cast<float>(index % 6) * 8.0f - 20.0f, 0.8f, static_cast<float>(index / 6) * 12.0f - 18.0f), glm::vec3(0.8f, 0.25f, 1.8f),
				BodyType::Dynamic);
			Test::PatchRigidBody(body, [kind](RigidBodyComponent& rigidBody)
			{
				rigidBody.Mass = kind == 1 ? 600.0f : 1200.0f;
			});
			constexpr std::array<std::string_view, 4> WheelNames = { "FL", "FR", "RL", "RR" };
			std::array<UUID, 4> wheels{};
			for (size_t wheel = 0; wheel < WheelNames.size(); ++wheel)
			{
				const bool front = wheel < 2;
				Entity entity = scene.CreateEntity(WheelNames[wheel], body);
				entity.Patch<TransformComponent>([wheel, front](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(wheel % 2 == 0 ? -0.8f : 0.8f, -0.5f, front ? -1.3f : 1.3f);
				});
				entity.AddComponent<VehicleWheelComponent>(VehicleWheelComponent{ .Radius = 0.4f,
					.Width = 0.25f,
					.MaxSteerAngle = front ? 30.0f : 0.0f,
					.MaxHandBrakeTorque = front ? 0.0f : 3000.0f });
				wheels[wheel] = entity.GetUUID();
			}
			WheeledVehicleComponent vehicle;
			vehicle.Transmission = kind == 1 ? VehicleTransmissionMode::Manual : VehicleTransmissionMode::Auto;
			vehicle.WheelCast = kind == 0 ? VehicleWheelCast::Cylinder : (kind == 1 ? VehicleWheelCast::Ray : VehicleWheelCast::Sphere);
			vehicle.Differentials = { VehicleDifferential{ .LeftWheel = wheels[2], .RightWheel = wheels[3] } };
			vehicle.AntiRollBars = { VehicleAntiRollBar{ .LeftWheel = wheels[0], .RightWheel = wheels[1] } };
			body.AddComponent<WheeledVehicleComponent>(vehicle);
		}

		// The ground, a row of low static bumps across the convoy's path, and the convoy.
		void BuildConvoy(Scene& scene)
		{
			static_cast<void>(Test::AddBoxBody(scene, "Ground", glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(150.0f, 0.5f, 150.0f), BodyType::Static));
			for (int bump = 0; bump < 6; ++bump)
			{
				static_cast<void>(Test::AddBoxBody(scene, std::format("Bump{}", bump), glm::vec3(static_cast<float>(bump) * 8.0f - 20.0f, 0.0f, -30.0f),
					glm::vec3(3.0f, 0.15f, 0.5f), BodyType::Static));
			}
			for (int index = 0; index < ConvoySize; ++index)
				AddConvoyVehicle(scene, index);
		}

		// The planned input of vehicle `index` at `tick`: still, then a throttle, a triangle-wave steering, Manual gears up, and a
		// stop with the brake and hand brake; integer arithmetic, so it is the same in every configuration.
		void PlanInput(WheeledVehicleComponent& vehicle, int index, uint32_t tick)
		{
			const auto phase = static_cast<int>((tick + 7u * static_cast<uint32_t>(index)) % 120u);
			vehicle.Steering = static_cast<float>(phase < 60 ? phase : 120 - phase) / 30.0f - 1.0f;
			vehicle.Throttle = tick < 20u || tick >= 220u ? 0.0f : (index % 2 == 0 ? 1.0f : 0.6f);
			vehicle.Brake = tick >= 240u ? 1.0f : 0.0f;
			vehicle.HandBrake = tick >= 270u ? 1.0f : 0.0f;
			vehicle.Gear = tick < 80u ? 1 : (tick < 160u ? 2 : 3);
			vehicle.Clutch = 1.0f;
		}

		uint64_t RunConvoy(uint32_t workerThreads)
		{
			const Test::ScopedWorkerThreadCount threads(workerThreads);
			Test::SceneTestFixture fixture;
			BuildConvoy(fixture.GetScene());
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 4401));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Scene& scene = (*session)->GetScene();
			for (uint32_t tick = 0; tick < ConvoyTicks; ++tick)
			{
				for (int index = 0; index < ConvoySize; ++index)
				{
					scene.FindEntityByPath(std::format("/Vehicle{}", index)).Patch<WheeledVehicleComponent>([index, tick](WheeledVehicleComponent& vehicle)
					{
						PlanInput(vehicle, index, tick);
					});
				}
				Test::RunTicks(**session, 1);
			}
			return (*session)->ComputeStateHash();
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PhysicsVehicles: a convoy driven by planned input ends in one state with 0, 1 and 8 Jolt threads")
		{
			Test::SetTestCaseTimeLimit(360.0);
			const uint64_t none = RunConvoy(0);
			CHECK(RunConvoy(1) == none);
			CHECK(RunConvoy(8) == none);
		}

		TEST_CASE("PhysicsVehicles: the convoy's state hash equals the committed constant in every configuration")
		{
			Test::SetTestCaseTimeLimit(180.0);
			// One committed value for every configuration (§9.1: a configuration-dependent result is a bug).
			const uint64_t hash = RunConvoy(PhysicsEngine::GetWorkerThreadCount());
			INFO("convoy hash: ", std::format("{:016x}", hash));
			CHECK(hash == CommittedConvoyHash);
		}
	}

}
