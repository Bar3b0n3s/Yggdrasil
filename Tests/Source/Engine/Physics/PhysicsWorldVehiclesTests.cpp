#include "TestsPCH.h"

#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Core/Hash.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Support/PhysicsTestScene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

// The physics world's wheeled vehicles (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Architecture §9.11):
// the Roadmap's M44 acceptance cases of the world (rest, top speed, the roll, the drop, twenty-four vehicles at 0, 1 and 8
// Jolt threads), the lifetime and refusal cases, and the grip by surface. ECS-agnostic: bodies and vehicles are created
// directly.

namespace Engine {

	namespace {

		constexpr float FixedDelta = 1.0f / 60.0f;
		constexpr float Gravity = 9.81f;
		constexpr float DegreesToRadians = std::numbers::pi_v<float> / 180.0f;

		Scope<PhysicsWorld> CreateWorld()
		{
			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			return std::move(*world);
		}

		Ref<const PhysicsShape> CreateBoxShape(const glm::vec3& halfExtents)
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = halfExtents } } } });
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			return std::move(*shape);
		}

		// A static box `halfExtents` whose top face is at y = 0 and whose centre is at `center` along X and Z.
		BodyHandle CreateGround(PhysicsWorld& world, const glm::vec3& halfExtents = glm::vec3(500.0f, 0.5f, 500.0f), const glm::vec2& center = glm::vec2(0.0f))
		{
			Result<BodyHandle> ground = world.CreateBody({ .Shape = CreateBoxShape(halfExtents),
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = glm::vec3(center.x, -halfExtents.y, center.y) },
				.Friction = 1.0f });
			REQUIRE(ground.has_value());
			return *ground;
		}

		// A buggy: a box chassis on four wheels, front left, front right, rear left and rear right (the front at -Z, the left
		// at -X), whose attachment points are at the chassis's centre height, so each wheel hangs SuspensionMaxLength below
		// it. The rear wheels are driven through one differential, and both axles have an anti-roll bar.
		struct Buggy
		{
			glm::vec3 Position = glm::vec3(0.0f, 1.0f, 0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 HalfExtents = glm::vec3(0.9f, 0.25f, 2.0f);
			float Mass = 1200.0f;
			float Track = 0.9f;       // each wheel's distance from the centre along X
			float Wheelbase = 1.4f;   // each axle's distance from the centre along Z
			float WheelHeight = 0.0f; // the attachment points' height above the body origin
			float Radius = 0.4f;
			float MaxSteer = 35.0f * DegreesToRadians;
			float LinearDamping = 0.05f;
			PhysicsVehicleCast Cast = PhysicsVehicleCast::Cylinder;
			PhysicsMassOverride MassOverride{};
			glm::vec3 LinearVelocity = glm::vec3(0.0f);
			bool AntiRollBars = true;
		};

		BodyHandle CreateChassis(PhysicsWorld& world, const Buggy& buggy)
		{
			Result<BodyHandle> body = world.CreateBody({ .Shape = CreateBoxShape(buggy.HalfExtents),
				.Pose = { .Position = buggy.Position, .Rotation = buggy.Rotation },
				.Mass = buggy.Mass,
				.LinearDamping = buggy.LinearDamping,
				.LinearVelocity = buggy.LinearVelocity,
				.MassOverride = buggy.MassOverride });
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			return *body;
		}

		PhysicsVehicleDescription MakeBuggy(BodyHandle body, const Buggy& buggy)
		{
			PhysicsVehicleDescription description;
			description.Body = body;
			description.Cast = buggy.Cast;
			for (const float z : { -buggy.Wheelbase, buggy.Wheelbase })
			{
				for (const float x : { -buggy.Track, buggy.Track })
				{
					PhysicsVehicleWheelDescription wheel;
					wheel.Position = glm::vec3(x, buggy.WheelHeight, z);
					wheel.Radius = buggy.Radius;
					wheel.Width = 0.3f;
					wheel.MaxSteerAngle = z < 0.0f ? buggy.MaxSteer : 0.0f;
					wheel.MaxHandBrakeTorque = z < 0.0f ? 0.0f : 4000.0f;
					description.Wheels.push_back(wheel);
				}
			}
			description.Settings.Differentials = { PhysicsVehicleDifferential{ .LeftWheel = 2, .RightWheel = 3 } };
			if (buggy.AntiRollBars)
			{
				description.Settings.AntiRollBars = { PhysicsVehicleAntiRollBar{ .LeftWheel = 0, .RightWheel = 1 },
					PhysicsVehicleAntiRollBar{ .LeftWheel = 2, .RightWheel = 3 } };
			}
			return description;
		}

		struct LiveBuggy
		{
			BodyHandle Body{};
			VehicleHandle Vehicle{};
		};

		LiveBuggy CreateBuggy(PhysicsWorld& world, const Buggy& buggy, const std::optional<PhysicsVehicleSettings>& settings = std::nullopt)
		{
			const BodyHandle body = CreateChassis(world, buggy);
			PhysicsVehicleDescription description = MakeBuggy(body, buggy);
			if (settings.has_value())
				description.Settings = *settings;
			Result<VehicleHandle> vehicle = world.CreateVehicle(description);
			REQUIRE_MESSAGE(vehicle.has_value(), vehicle.error().ToString());
			return { body, *vehicle };
		}

		void StepWorld(PhysicsWorld& world, uint32_t steps)
		{
			for (uint32_t step = 0; step < steps; ++step)
				REQUIRE(world.Step(FixedDelta, 1).has_value());
		}

		// The body's speed along its forward (local -Z), signed.
		float GetForwardSpeed(const PhysicsWorld& world, BodyHandle body)
		{
			const glm::vec3 forward = world.GetPose(body).Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
			return glm::dot(world.GetLinearVelocity(body), forward);
		}

		// How far the body's up has tilted from the world's: the cosine of the angle between them.
		float GetUpright(const PhysicsWorld& world, BodyHandle body)
		{
			return (world.GetPose(body).Rotation * glm::vec3(0.0f, 1.0f, 0.0f)).y;
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsWorld: a vehicle at rest touches with every wheel within 1 cm of its rest length")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			const Buggy buggy{ .Cast = PhysicsVehicleCast::Ray, .AntiRollBars = false };
			const LiveBuggy live = CreateBuggy(*world, buggy);
			StepWorld(*world, 300);

			// Each spring holds a quarter of the weight. Jolt's spring of frequency f has the stiffness k = m_eff (2 pi f)^2,
			// m_eff the effective mass of the vehicle along the suspension at the wheel's contact (1 / (1/m + (r x n) I^-1
			// (r x n)), the box's inertia about its centre), so the rest length is SuspensionMaxLength - (m g / 4) / k.
			const PhysicsVehicleWheelDescription wheel = MakeBuggy(live.Body, buggy).Wheels.front();
			const glm::vec3 size = 2.0f * buggy.HalfExtents;
			const float inertiaX = buggy.Mass / 12.0f * (size.y * size.y + size.z * size.z);
			const float inertiaZ = buggy.Mass / 12.0f * (size.x * size.x + size.y * size.y);
			const PhysicsVehicleState state = world->GetVehicleState(live.Vehicle);
			REQUIRE(state.Wheels.size() == 4);
			const float omega = 2.0f * std::numbers::pi_v<float> * wheel.SpringFrequency;
			for (size_t index = 0; index < state.Wheels.size(); ++index)
			{
				INFO("wheel " << index);
				const PhysicsVehicleWheelState& reading = state.Wheels[index];
				CHECK(reading.Touching);
				const glm::vec3 lever = reading.Point - world->GetCenterOfMassPosition(live.Body);
				const float effectiveMass = 1.0f / (1.0f / buggy.Mass + lever.z * lever.z / inertiaX + lever.x * lever.x / inertiaZ);
				const float restLength = wheel.SuspensionMaxLength - buggy.Mass * Gravity / 4.0f / (effectiveMass * omega * omega);
				CHECK(std::abs(reading.SuspensionLength - restLength) < 0.01f);
				CHECK(reading.SuspensionForce == doctest::Approx(buggy.Mass * Gravity / 4.0f).epsilon(0.02));
				CHECK(reading.Normal.y == doctest::Approx(1.0f).epsilon(1.0e-3));
			}
			CHECK(GetUpright(*world, live.Body) > 0.999f);
		}

		TEST_CASE("PhysicsWorld: full throttle reaches the analytic top speed and holds it within 2 %")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world, glm::vec3(20.0f, 0.5f, 1000.0f), glm::vec2(0.0f, -900.0f)));
			// A strong engine whose torque falls to 0 at MaxRpm (a rev limiter) and no damping on the body: the engine's MaxRpm in
			// the top gear is all that limits the speed. Jolt clamps the engine's RPM but keeps its torque at MaxRpm, so with a
			// curve that ends above 0 the clutch keeps driving the wheels past it.
			PhysicsVehicleSettings settings;
			settings.Engine = { .MaxTorque = 2000.0f,
				.MinRpm = 500.0f,
				.MaxRpm = 3000.0f,
				.TorqueCurve = { { 0.0f, 0.8f }, { 0.66f, 1.0f }, { 0.9f, 1.0f }, { 1.0f, 0.0f } } };
			settings.Transmission.GearRatios = { 2.0f, 1.0f };
			settings.Transmission.ShiftUpRpm = 2500.0f;
			settings.Transmission.ShiftDownRpm = 1000.0f;
			settings.Differentials = { PhysicsVehicleDifferential{ .LeftWheel = 2, .RightWheel = 3, .Ratio = 4.0f } };
			const Buggy buggy{ .Radius = 0.3f, .LinearDamping = 0.0f };
			const LiveBuggy live = CreateBuggy(*world, buggy, settings);
			world->SetVehicleInput(live.Vehicle, { .Forward = 1.0f });

			// MaxRpm 2 pi / 60 / (top ratio * differential ratio) * radius.
			const float topSpeed = settings.Engine.MaxRpm * 2.0f * std::numbers::pi_v<float> / 60.0f / (1.0f * 4.0f) * buggy.Radius;
			StepWorld(*world, 600);
			CHECK(world->GetVehicleState(live.Vehicle).Gear == 2);
			for (uint32_t step = 0; step < 120; ++step)
			{
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				CHECK(GetForwardSpeed(*world, live.Body) == doctest::Approx(topSpeed).epsilon(0.02));
			}
			CHECK(world->GetVehicleState(live.Vehicle).EngineRpm == doctest::Approx(settings.Engine.MaxRpm).epsilon(0.02));
		}

		TEST_CASE("PhysicsWorld: a full-lock turn at 15 m/s rolls a buggy over without its lowered centre of mass and not with it")
		{
			// A tall, narrow buggy, its wheels under the chassis: its centre of mass about 1.6 m above the ground over a track of
			// 1.1 m, so the tyres' grip tips it in a hard turn; lowered by 1.4 m, the same turn keeps it on its wheels.
			const auto turn = [](const PhysicsMassOverride& massOverride)
			{
				Scope<PhysicsWorld> world = CreateWorld();
				static_cast<void>(CreateGround(*world));
				const Buggy buggy{ .Position = glm::vec3(0.0f, 1.8f, 0.0f),
					.HalfExtents = glm::vec3(0.55f, 0.8f, 1.6f),
					.Track = 0.55f,
					.Wheelbase = 1.2f,
					.WheelHeight = -0.8f,
					.MaxSteer = 40.0f * DegreesToRadians,
					.MassOverride = massOverride,
					.LinearVelocity = glm::vec3(0.0f, 0.0f, -15.0f),
					.AntiRollBars = false };
				const LiveBuggy live = CreateBuggy(*world, buggy);
				// The suspension settles at speed first, then the wheel is turned to full lock with the throttle held.
				StepWorld(*world, 20);
				world->SetVehicleInput(live.Vehicle, { .Forward = 1.0f, .Right = 1.0f });
				float upright = 1.0f;
				for (uint32_t step = 0; step < 180; ++step)
				{
					REQUIRE(world->Step(FixedDelta, 1).has_value());
					upright = std::min(upright, GetUpright(*world, live.Body));
				}
				return upright;
			};
			CHECK(turn({}) < 0.0f);
			CHECK(turn({ .CenterOfMassOffset = glm::vec3(0.0f, -1.4f, 0.0f) }) > 0.7f);
		}

		TEST_CASE("PhysicsWorld: a vehicle dropped 2 m lands on its wheels")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			// The wheels' lowest point 2 m above the ground: the attachment at the chassis's centre, the wheel's centre
			// SuspensionMaxLength (0.5 m) below it, its bottom a radius (0.4 m) lower.
			const LiveBuggy live = CreateBuggy(*world, { .Position = glm::vec3(0.0f, 2.9f, 0.0f), .MassOverride = { .CenterOfMassOffset = glm::vec3(0.0f, -0.2f, 0.0f) } });
			bool compressed = false;
			for (uint32_t step = 0; step < 240; ++step)
			{
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				for (const PhysicsVehicleWheelState& wheel : world->GetVehicleState(live.Vehicle).Wheels)
					compressed = compressed || (wheel.Touching && wheel.SuspensionLength < 0.35f);
			}
			// The suspension took the landing, and the buggy stands on all four wheels.
			CHECK(compressed);
			CHECK(GetUpright(*world, live.Body) > 0.99f);
			for (const PhysicsVehicleWheelState& wheel : world->GetVehicleState(live.Vehicle).Wheels)
				CHECK(wheel.Touching);
			CHECK(world->GetPose(live.Body).Position.y > 0.6f);
		}

		TEST_CASE("PhysicsWorld: twenty-four vehicles step identically with 0, 1 and 8 Jolt threads")
		{
			// Twenty-four vehicles: Jolt runs step listeners in batches of 8, so with 8 worker threads they run in three jobs at
			// once (a test of 8 vehicles or fewer would run every OnStep in one job). Each drives with its own input, across
			// boxes they push and over each other's wheels' ground.
			const auto run = [](uint32_t workerThreads)
			{
				const Test::ScopedWorkerThreadCount threads(workerThreads);
				Scope<PhysicsWorld> world = CreateWorld();
				static_cast<void>(CreateGround(*world));
				std::vector<BodyHandle> bodies;
				std::vector<VehicleHandle> vehicles;
				for (int index = 0; index < 24; ++index)
				{
					const float x = static_cast<float>(index % 6) * 6.0f - 15.0f;
					const float z = static_cast<float>(index / 6) * 8.0f - 12.0f;
					const LiveBuggy live = CreateBuggy(*world, { .Position = glm::vec3(x, 1.0f + 0.25f * static_cast<float>(index % 3), z), .Rotation = glm::normalize(glm::quat(1.0f, 0.0f, 0.125f * static_cast<float>(index % 5), 0.0f)), .Cast = static_cast<PhysicsVehicleCast>(index % 3) });
					world->SetVehicleInput(live.Vehicle, { .Forward = index % 4 == 3 ? -0.5f : 1.0f, .Right = static_cast<float>(index % 5 - 2) * 0.5f, .Brake = index % 7 == 0 ? 0.5f : 0.0f });
					bodies.push_back(live.Body);
					vehicles.push_back(live.Vehicle);
				}
				for (int index = 0; index < 12; ++index)
				{
					Result<BodyHandle> box = world->CreateBody({ .Shape = CreateBoxShape(glm::vec3(0.3f)),
						.Pose = { .Position = glm::vec3(static_cast<float>(index) * 2.5f - 14.0f, 0.3f, -20.0f) } });
					REQUIRE(box.has_value());
					bodies.push_back(*box);
				}
				StepWorld(*world, 240);

				XXH64Hasher hasher(0);
				for (const BodyHandle body : bodies)
				{
					const PhysicsPose pose = world->GetPose(body);
					for (const float value : { pose.Position.x, pose.Position.y, pose.Position.z, pose.Rotation.x, pose.Rotation.y, pose.Rotation.z,
							 pose.Rotation.w })
						hasher.UpdateU64(std::bit_cast<uint32_t>(value));
					const glm::vec3 velocity = world->GetLinearVelocity(body);
					for (const float value : { velocity.x, velocity.y, velocity.z })
						hasher.UpdateU64(std::bit_cast<uint32_t>(value));
				}
				std::vector<std::byte> vehicleBytes;
				for (const VehicleHandle vehicle : vehicles)
					world->AppendVehicleState(vehicle, vehicleBytes);
				for (const std::byte byte : vehicleBytes)
					hasher.UpdateU64(std::to_integer<uint64_t>(byte));
				return hasher.Digest();
			};
			const uint64_t none = run(0);
			CHECK(run(1) == none);
			CHECK(run(8) == none);
		}

		TEST_CASE("PhysicsWorld: destroying a body destroys its vehicle first, and the vehicle's handle goes invalid")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ground = CreateGround(*world);
			const LiveBuggy first = CreateBuggy(*world, {});
			const LiveBuggy second = CreateBuggy(*world, { .Position = glm::vec3(10.0f, 1.0f, 0.0f) });
			CHECK(world->GetVehicleCount() == 2);
			CHECK(world->FindVehicle(first.Body) == first.Vehicle);
			CHECK(second.Vehicle.GetValue() == first.Vehicle.GetValue() + 1);
			// A body holds one vehicle.
			const Result<VehicleHandle> again = world->CreateVehicle(MakeBuggy(first.Body, {}));
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::InvalidArgument);
			world->SetVehicleInput(first.Vehicle, { .Forward = 1.0f });
			StepWorld(*world, 30);

			world->DestroyBody(first.Body);
			CHECK_FALSE(world->IsVehicleValid(first.Vehicle));
			CHECK(world->GetVehicleCount() == 1);
			CHECK_FALSE(world->FindVehicle(first.Body).IsValid());
			CHECK(world->GetVehicleState(first.Vehicle).Wheels.empty());
			// A dead handle is ignored, and the world steps on without the vehicle.
			world->DestroyVehicle(first.Vehicle);
			CHECK(world->SetVehicleSettings(first.Vehicle, {}).error().GetCode() == ErrorCode::InvalidArgument);
			StepWorld(*world, 30);
			CHECK(world->IsVehicleValid(second.Vehicle));

			// The ground destroyed under the second vehicle: its wheels' last contacts read as no body, never through Jolt's
			// pointer, and the next step finds no ground.
			REQUIRE(world->GetVehicleState(second.Vehicle).Wheels.front().Touching);
			world->DestroyBody(ground);
			const PhysicsVehicleWheelState stale = world->GetVehicleState(second.Vehicle).Wheels.front();
			CHECK_FALSE(stale.ContactBody.IsValid());
			CHECK(stale.ContactCollider == 0);
			StepWorld(*world, 1);
			CHECK_FALSE(world->GetVehicleState(second.Vehicle).Wheels.front().Touching);

			// DestroyVehicle keeps the body; the world's destructor removes the vehicles still alive before their bodies.
			world->DestroyVehicle(second.Vehicle);
			CHECK(world->IsBodyValid(second.Body));
			CHECK(world->GetVehicleCount() == 0);
			const LiveBuggy third = CreateBuggy(*world, { .Position = glm::vec3(-10.0f, 1.0f, 0.0f) });
			CHECK(third.Vehicle.GetValue() == second.Vehicle.GetValue() + 1);
			world.reset();
		}

		TEST_CASE("PhysicsWorld: vehicle descriptions and settings Jolt would assert on are refused, and nothing reaches Jolt")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			const Buggy buggy{};
			const BodyHandle body = CreateChassis(*world, buggy);
			using Edit = void (*)(PhysicsVehicleDescription&);
			const std::array<Edit, 33> refused = {
				[](PhysicsVehicleDescription& d)
			{
				d.Body = BodyHandle();
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels.clear();
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels.resize(MaxPhysicsVehicleWheels + 1);
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Cast = static_cast<PhysicsVehicleCast>(7);
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[1].SuspensionMinLength = 0.6f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[1].SpringFrequency = 0.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[1].Radius = 0.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[0].MaxSteerAngle = 91.0f * DegreesToRadians;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[0].Up = glm::vec3(0.0f, 0.0f, -1.0f);
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[0].Forward = glm::vec3(0.0f, 0.0f, -2.0f);
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[2].Position.x = std::numeric_limits<float>::quiet_NaN();
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[2].Inertia = 0.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[2].MaxBrakeTorque = -1.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[3].LateralFriction = { { 0.0f, 0.0f }, { 0.0f, 1.0f } };
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Wheels[3].LongitudinalFriction = { { 0.0f, -1.0f } };
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Engine.MinRpm = 7000.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Engine.MaxRpm = 0.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Engine.TorqueCurve = { { 0.5f, 1.0f }, { 0.25f, 1.0f } };
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Engine.Inertia = 0.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Transmission.GearRatios = { 2.0f, -1.0f };
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Transmission.ReverseGearRatios = { 2.9f };
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Transmission.GearRatios.clear();
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Transmission.ShiftUpRpm = 1500.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Transmission.ShiftUpRpm = 6500.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Transmission.ClutchStrength = 0.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Differentials[0].LeftRightSplit = 1.5f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Differentials[0].LimitedSlipRatio = 0.5f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Differentials[0].RightWheel = 4;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Differentials[0].RightWheel = 2;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.Differentials[0].EngineTorqueRatio = 0.5f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.AntiRollBars[0].RightWheel = 0;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.DifferentialLimitedSlipRatio = 1.0f;
			},
				[](PhysicsVehicleDescription& d)
			{
				d.Settings.MaxPitchRollAngle = std::numeric_limits<float>::infinity();
			},
			};
			for (size_t index = 0; index < refused.size(); ++index)
			{
				INFO("case " << index);
				PhysicsVehicleDescription description = MakeBuggy(body, buggy);
				refused[index](description);
				const Result<VehicleHandle> created = world->CreateVehicle(description);
				REQUIRE_FALSE(created.has_value());
				CHECK(created.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(world->GetVehicleCount() == 0);
			}
			// A Static or Kinematic body cannot hold a vehicle.
			for (const PhysicsMotionType type : { PhysicsMotionType::Static, PhysicsMotionType::Kinematic })
			{
				Result<BodyHandle> other = world->CreateBody({ .Shape = CreateBoxShape(glm::vec3(1.0f)), .MotionType = type, .Pose = { .Position = glm::vec3(20.0f, 5.0f, 0.0f) } });
				REQUIRE(other.has_value());
				CHECK_FALSE(world->CreateVehicle(MakeBuggy(*other, buggy)).has_value());
			}

			// The valid description; then settings that are refused leave the vehicle as it was.
			Result<VehicleHandle> vehicle = world->CreateVehicle(MakeBuggy(body, buggy));
			REQUIRE_MESSAGE(vehicle.has_value(), vehicle.error().ToString());
			for (size_t index = 15; index < refused.size(); ++index)
			{
				INFO("settings case " << index);
				PhysicsVehicleDescription description = MakeBuggy(body, buggy);
				refused[index](description);
				CHECK(world->SetVehicleSettings(*vehicle, description.Settings).error().GetCode() == ErrorCode::InvalidArgument);
			}
			// Valid settings apply in place: fewer gears move the gear into range, and the RPM into the new engine's range.
			world->SetVehicleInput(*vehicle, { .Forward = 1.0f });
			StepWorld(*world, 120);
			PhysicsVehicleSettings changed = MakeBuggy(body, buggy).Settings;
			changed.Transmission.GearRatios = { 3.0f };
			changed.Engine.MaxRpm = 2000.0f;
			changed.Engine.MinRpm = 1500.0f;
			changed.Transmission.ShiftUpRpm = 1900.0f;
			changed.Transmission.ShiftDownRpm = 1600.0f;
			REQUIRE(world->SetVehicleSettings(*vehicle, changed).has_value());
			const PhysicsVehicleState state = world->GetVehicleState(*vehicle);
			CHECK(state.Gear <= 1);
			CHECK(state.EngineRpm >= 1500.0f);
			CHECK(state.EngineRpm <= 2000.0f);
			StepWorld(*world, 60);
			CHECK(std::isfinite(GetForwardSpeed(*world, body)));
		}

		TEST_CASE("PhysicsWorld: a wheel's grip follows the friction of the collider it stands on")
		{
			// One static compound of two strips side by side, collider 0 rough (its body's friction 1.0) and collider 1 slick
			// (a surface of friction 0.05): the same buggy braking from 12 m/s stops much shorter on the rough one, and its
			// wheels name the collider they stand on.
			const auto stop = [](bool slick)
			{
				Scope<PhysicsWorld> world = CreateWorld();
				const BoxShapeGeometry strip{ .HalfExtents = glm::vec3(5.0f, 0.5f, 200.0f) };
				Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = {
																				   ColliderShapeDescription{ .Geometry = strip, .Position = glm::vec3(-5.0f, -0.5f, -150.0f), .UserData = 0 },
																				   ColliderShapeDescription{ .Geometry = strip, .Position = glm::vec3(5.0f, -0.5f, -150.0f), .UserData = 1 } } });
				REQUIRE(shape.has_value());
				Result<BodyHandle> ground = world->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static, .Friction = 1.0f });
				REQUIRE(ground.has_value());
				REQUIRE(world->SetColliderSurfaces(*ground, std::array{ PhysicsColliderSurface{ .Collider = 1, .Friction = 0.05f } }).has_value());
				const LiveBuggy live = CreateBuggy(*world, { .Position = glm::vec3(slick ? 5.0f : -5.0f, 1.0f, 0.0f), .LinearVelocity = glm::vec3(0.0f, 0.0f, -12.0f) });
				StepWorld(*world, 10);
				for (const PhysicsVehicleWheelState& wheel : world->GetVehicleState(live.Vehicle).Wheels)
				{
					CHECK(wheel.Touching);
					CHECK(wheel.ContactBody == *ground);
					CHECK(wheel.ContactCollider == (slick ? 1u : 0u));
				}
				const float start = world->GetPose(live.Body).Position.z;
				world->SetVehicleInput(live.Vehicle, { .Brake = 1.0f });
				uint32_t steps = 0;
				while (GetForwardSpeed(*world, live.Body) > 0.1f && steps < 1200)
				{
					REQUIRE(world->Step(FixedDelta, 1).has_value());
					++steps;
				}
				REQUIRE(steps < 1200);
				return start - world->GetPose(live.Body).Position.z;
			};
			const float rough = stop(false);
			const float slick = stop(true);
			CHECK(rough > 1.0f);
			CHECK(slick > 3.0f * rough);
		}
	}

}
