#include "TestsPCH.h"

#include "Engine/Physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <vector>

// The world's buoyancy (M47, Docs/Decisions/0065-m47-water.md decision 3; Physics/PhysicsBuoyancy.h): each collider of a
// Dynamic body is submerged against its own fluid plane through Jolt's exact submerged volumes, and the body takes the
// physical buoyancy of the water's density with Jolt's drag; each plane's submerged volume is reported; refusals change
// nothing.

namespace Engine {

	namespace {

		constexpr float BuoyancyStep = 1.0f / 60.0f;

		Scope<PhysicsWorld> CreateBuoyancyWorld()
		{
			const std::vector<std::string> layers = { "Default" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Default" } };
			Result<PhysicsLayerTable> table = PhysicsLayerTable::Create(layers, collisions);
			REQUIRE(table.has_value());
			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({ .Layers = *table });
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			REQUIRE((*world)->SetGravity(glm::vec3(0.0f, -9.81f, 0.0f)));
			return std::move(*world);
		}

		// A Dynamic body of `mass` kg at `position` made of `colliders` (each a box), sleeping disallowed.
		BodyHandle CreateFloater(PhysicsWorld& world, const std::vector<ColliderShapeDescription>& colliders, float mass, const glm::vec3& position)
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = colliders });
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			Result<BodyHandle> body = world.CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Dynamic, .Pose = { .Position = position }, .Mass = mass, .AllowSleeping = false });
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			return *body;
		}

		// A level plane of still water at height `height`.
		PhysicsFluidPlane StillWater(float height)
		{
			return PhysicsFluidPlane{ .InWater = true, .Point = glm::vec3(0.0f, height, 0.0f), .Normal = glm::vec3(0.0f, 1.0f, 0.0f) };
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsWorld: a box of known mass floats at its analytic draft within 1 cm")
		{
			// A 2 m x 0.5 m x 1 m box of 400 kg on fresh water. At rest the displaced water weighs as much as the box:
			// rho V_sub = m, so V_sub = 400 / 1000 = 0.4 m^3, and over its 2 m x 1 m base the draft is V_sub / A = 0.4 / 2 = 0.2 m.
			// Its bottom lies 0.2 m below the surface at 0, at y = -0.2, and its centre 0.25 m above the bottom: y = +0.05.
			Scope<PhysicsWorld> world = CreateBuoyancyWorld();
			const BodyHandle box = CreateFloater(*world,
				{ ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(1.0f, 0.25f, 0.5f) } } }, 400.0f, glm::vec3(0.0f, 0.5f, 0.0f));
			const std::array<PhysicsFluidPlane, 1> planes = { StillWater(0.0f) };
			const PhysicsBuoyancySettings settings{ .Density = 1000.0f };
			// Dropped from 0.45 m above its draft, it bobs about its draft under Jolt's quadratic drag: the mean height over the
			// last 5 s (about five and a half periods of 0.9 s, 2 pi sqrt(m / (rho g A))) is the draft's.
			double sum = 0.0;
			double submerged = 0.0;
			float lowest = 1.0f;
			float highest = -1.0f;
			for (int step = 0; step < 900; ++step)
			{
				const Result<PhysicsBuoyancyResult> result = world->ApplyBuoyancy(box, planes, settings, BuoyancyStep);
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				CHECK(result->TotalVolume == doctest::Approx(1.0f).epsilon(1e-5));
				if (step >= 600)
					submerged += result->SubmergedVolume;
				REQUIRE(world->Step(BuoyancyStep, 1));
				if (step >= 600)
				{
					const float height = world->GetPose(box).Position.y;
					sum += height;
					lowest = std::min(lowest, height);
					highest = std::max(highest, height);
				}
			}
			const double mean = sum / 300.0;
			CAPTURE(mean);
			CAPTURE(lowest);
			CAPTURE(highest);
			CHECK(std::abs(mean - 0.05) < 0.01);
			// On average the submerged volume carries the weight: rho V_sub = m.
			CHECK(std::abs(submerged / 300.0 - 0.4) < 0.02);
			const Result<PhysicsBuoyancyResult> rest = world->ApplyBuoyancy(box, planes, settings, BuoyancyStep);
			REQUIRE(rest.has_value());
			CHECK(rest->Submerged);
			REQUIRE(rest->PlaneSubmergedVolumes.size() == 1);
			CHECK(rest->PlaneSubmergedVolumes[0] == rest->SubmergedVolume);
			// A body above the water displaces nothing; refusals change nothing.
			const BodyHandle high = CreateFloater(*world, { ColliderShapeDescription{} }, 10.0f, glm::vec3(5.0f, 10.0f, 0.0f));
			const Result<PhysicsBuoyancyResult> dry = world->ApplyBuoyancy(high, planes, settings, BuoyancyStep);
			REQUIRE(dry.has_value());
			CHECK_FALSE(dry->Submerged);
			CHECK(dry->SubmergedVolume == 0.0f);
			const std::array<PhysicsFluidPlane, 1> broken = { PhysicsFluidPlane{ .InWater = true, .Point = glm::vec3(0.0f, std::nanf(""), 0.0f) } };
			CHECK_FALSE(world->ApplyBuoyancy(box, broken, settings, BuoyancyStep).has_value());
			CHECK_FALSE(world->ApplyBuoyancy(box, planes, settings, 0.0f).has_value());
			CHECK_FALSE(world->ApplyBuoyancy(BodyHandle(), planes, settings, BuoyancyStep).has_value());
		}

		TEST_CASE("PhysicsWorld: each collider is submerged against its own plane")
		{
			// Two 1 m pontoons 4 m apart on one body, the left one's plane at its centre's height and the right one's 1 m below
			// its bottom: half the left pontoon is under water and none of the right one.
			Scope<PhysicsWorld> world = CreateBuoyancyWorld();
			const BodyHandle raft = CreateFloater(*world,
				{ ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, .Position = glm::vec3(-2.0f, 0.0f, 0.0f), .UserData = 0 },
					ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, .Position = glm::vec3(2.0f, 0.0f, 0.0f), .UserData = 1 } },
				500.0f, glm::vec3(0.0f));
			const PhysicsBuoyancySettings settings{ .Density = 1000.0f };
			const std::array<PhysicsFluidPlane, 2> split = { StillWater(0.0f), StillWater(-1.5f) };
			const Result<PhysicsBuoyancyResult> result = world->ApplyBuoyancy(raft, split, settings, BuoyancyStep);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->TotalVolume == doctest::Approx(2.0f).epsilon(1e-5));
			CHECK(result->SubmergedVolume == doctest::Approx(0.5f).epsilon(1e-4));
			// The buoyancy pushes up the left side alone: the raft turns about -Z, its left side rising.
			REQUIRE(world->Step(BuoyancyStep, 1));
			const glm::vec3 spin = world->GetAngularVelocity(raft);
			CHECK(spin.z < 0.0f);
			// One plane for both: the same volume twice over; a collider past the planes is out of water.
			const std::array<PhysicsFluidPlane, 2> level = { StillWater(0.0f), StillWater(0.0f) };
			const Result<PhysicsBuoyancyResult> both = world->ApplyBuoyancy(raft, level, settings, BuoyancyStep);
			REQUIRE(both.has_value());
			CHECK(both->SubmergedVolume == doctest::Approx(1.0f).epsilon(0.05));
			const Result<PhysicsBuoyancyResult> one = world->ApplyBuoyancy(raft, std::span<const PhysicsFluidPlane>(level.data(), 1), settings, BuoyancyStep);
			REQUIRE(one.has_value());
			CHECK(one->SubmergedVolume == doctest::Approx(0.5f).epsilon(0.05));
			// A plane out of water displaces nothing, whatever it holds.
			const std::array<PhysicsFluidPlane, 2> dry = { PhysicsFluidPlane{ .InWater = false, .Point = glm::vec3(0.0f, 100.0f, 0.0f) }, PhysicsFluidPlane{} };
			const Result<PhysicsBuoyancyResult> none = world->ApplyBuoyancy(raft, dry, settings, BuoyancyStep);
			REQUIRE(none.has_value());
			CHECK_FALSE(none->Submerged);
		}
	}

}
