#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/BuoyancyComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

// Buoyancy in the play session (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §9.12): PhysicsSystem's PreStep
// item 4c floats every Buoyancy's Dynamic body on the planes its colliders' waters give at the tick's world time, a body on
// calm, unchanged water sleeps and a change wakes it, and the result is deterministic across Jolt thread counts. C47 commits
// the physics stream's acceptance cases skipped; W47.2's physics stream implements item 4c and lifts the skips, recording the
// committed hash from the implementation.

namespace Engine {

	namespace {

		// The 16 swell bodies' state hash after BuoyancySwellTicks ticks, recorded by W47.2's physics stream from its
		// implementation and verified equal in Debug, Release and Dist (§9.1; decision 6).
		constexpr uint64_t CommittedBuoyancySwellHash = 0x0000000000000000;
		constexpr uint32_t BuoyancySwellTicks = 300;

		// An Ocean at y = 0 with `waves` at `waveScale`, named "Sea".
		Entity AddSea(Scene& scene, std::vector<WaterWave> waves, float waveScale)
		{
			Entity sea = scene.CreateEntity("Sea");
			WaterComponent water;
			water.Waves = std::move(waves);
			water.WaveScale = waveScale;
			sea.AddComponent<WaterComponent>(water);
			return sea;
		}

		// A 6 m swell travelling along +X.
		std::vector<WaterWave> MakeShortSwell()
		{
			return { WaterWave{ .Direction = 0.0f, .Wavelength = 6.0f, .Amplitude = 0.3f, .Steepness = 0.4f } };
		}

		// A 200 kg raft at `position`: a Dynamic RigidBody with two 1 m x 0.5 m x 1 m pontoons 3 m apart along X (child
		// colliders gathered into its compound), and a Buoyancy.
		Entity AddRaft(Scene& scene, std::string_view name, const glm::vec3& position)
		{
			Entity raft = scene.CreateEntity(name);
			raft.Patch<TransformComponent>([&position](TransformComponent& transform)
			{
				transform.Translation = position;
			});
			RigidBodyComponent body;
			body.Type = BodyType::Dynamic;
			body.Mass = 200.0f;
			raft.AddComponent<RigidBodyComponent>(body);
			for (const float x : { -1.5f, 1.5f })
			{
				Entity pontoon = scene.CreateEntity(x < 0.0f ? "Left" : "Right", raft);
				pontoon.Patch<TransformComponent>([x](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(x, 0.0f, 0.0f);
				});
				pontoon.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(0.5f, 0.25f, 0.5f) });
			}
			raft.AddComponent<BuoyancyComponent>();
			return raft;
		}

		// The raft's pitch about Z, in degrees.
		float PitchDegrees(PlaySession& session, std::string_view path)
		{
			const Entity raft = session.GetScene().FindEntityByPath(path);
			REQUIRE(raft.IsValid());
			const glm::quat rotation = raft.GetComponent<TransformComponent>().Rotation;
			const glm::vec3 axisX = rotation * glm::vec3(1.0f, 0.0f, 0.0f);
			return glm::degrees(std::atan2(axisX.y, glm::length(glm::vec2(axisX.x, axisX.z))));
		}

		// The 16 rafts on a crossed swell after BuoyancySwellTicks ticks with `workerThreads` Jolt threads.
		uint64_t RunSwell(uint32_t workerThreads)
		{
			const Test::ScopedWorkerThreadCount threads(workerThreads);
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddSea(scene,
				{ WaterWave{ .Direction = 0.0f, .Wavelength = 9.0f, .Amplitude = 0.35f, .Steepness = 0.6f },
					WaterWave{ .Direction = 70.0f, .Wavelength = 5.0f, .Amplitude = 0.15f, .Steepness = 0.8f, .Speed = 2.5f, .Phase = 45.0f } },
				1.0f));
			for (int row = 0; row < 4; ++row)
			{
				for (int column = 0; column < 4; ++column)
				{
					const Entity raft = AddRaft(scene, "Raft", glm::vec3(static_cast<float>(column) * 6.5f, 0.5f + 0.125f * static_cast<float>(row), static_cast<float>(row) * 3.0f));
					raft.Patch<TransformComponent>([column](TransformComponent& transform)
					{
						transform.Rotation = glm::normalize(glm::quat(1.0f, 0.0f, 0.0625f * static_cast<float>(column), 0.0f));
					});
				}
			}
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 4401));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Test::RunTicks(**session, BuoyancySwellTicks);
			return (*session)->ComputeStateHash();
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("Buoyancy: two pontoons pitch with a swell's slope" * doctest::skip(true))
		{
			// On calm water the raft settles level; on a 6 m swell, shorter than the raft's 4 m span plus its pontoons, each
			// pontoon rides the surface under it, so the raft pitches with the slope between them.
			const auto pitchRange = [](float waveScale)
			{
				Test::SceneTestFixture fixture;
				static_cast<void>(AddSea(fixture.GetScene(), MakeShortSwell(), waveScale));
				static_cast<void>(AddRaft(fixture.GetScene(), "Raft", glm::vec3(0.0f, 0.2f, 0.0f)));
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				Test::RunTicks(**session, 120); // settle
				float lowest = 0.0f;
				float highest = 0.0f;
				for (int tick = 0; tick < 240; ++tick)
				{
					Test::RunTicks(**session, 1);
					const float pitch = PitchDegrees(**session, "/Raft");
					lowest = std::min(lowest, pitch);
					highest = std::max(highest, pitch);
				}
				// The raft floats: its height stays near the surface.
				CHECK(std::abs(Test::GetWorldPosition(**session, "/Raft").y) < 1.0f);
				return highest - lowest;
			};
			const float calm = pitchRange(0.0f);
			const float swell = pitchRange(1.0f);
			CAPTURE(calm);
			CAPTURE(swell);
			CHECK(calm < 0.2f);
			CHECK(swell > 4.0f);
		}

		TEST_CASE("Buoyancy: a buoyant body on calm water sleeps and a tide wakes it" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddSea(fixture.GetScene(), {}, 1.0f));
			static_cast<void>(AddRaft(fixture.GetScene(), "Raft", glm::vec3(0.0f, 0.1f, 0.0f)));
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			const UUID raft = Test::GetEntityId(play.GetScene(), "/Raft");
			// Settled on calm, unchanged water, it sleeps.
			Test::RunTicks(play, 900);
			std::optional<PhysicsBodyReport> report = play.GetPhysics().GetBodyInfo(raft);
			REQUIRE(report.has_value());
			CHECK(report->IsSleeping);
			const float restHeight = Test::GetWorldPosition(play, "/Raft").y;
			// The tide: the sea's base rises by 0.5 m; the changed planes wake it, and it rises with the water.
			play.GetScene().FindEntityByPath("/Sea").Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 0.5f;
			});
			Test::RunTicks(play, 1);
			report = play.GetPhysics().GetBodyInfo(raft);
			REQUIRE(report.has_value());
			CHECK_FALSE(report->IsSleeping);
			Test::RunTicks(play, 600);
			CHECK(Test::GetWorldPosition(play, "/Raft").y == doctest::Approx(restHeight + 0.5f).epsilon(0.02));
			// The buoyancy block of physics.bodyInfo: the sea, the raft's 0.5 m^3, the 200 kg's 0.2 m^3 submerged.
			report = play.GetPhysics().GetBodyInfo(raft);
			REQUIRE(report.has_value());
			REQUIRE(report->Buoyancy.has_value());
			CHECK(report->Buoyancy->Water == Test::GetEntityId(play.GetScene(), "/Sea"));
			CHECK(report->Buoyancy->TotalVolume == doctest::Approx(0.5f).epsilon(1e-4));
			CHECK(report->Buoyancy->SubmergedVolume == doctest::Approx(0.2f).epsilon(0.05));
		}

		TEST_CASE("Buoyancy: 16 bodies on a swell end at their committed hash at Jolt thread counts 0, 1 and 8" * doctest::skip(true))
		{
			const uint64_t none = RunSwell(0);
			const uint64_t one = RunSwell(1);
			const uint64_t eight = RunSwell(8);
			INFO("swell hash: ", std::format("{:016x}", none));
			CHECK(one == none);
			CHECK(eight == none);
			// One committed value for every configuration (§9.1: a configuration-dependent result is a bug).
			CHECK(none == CommittedBuoyancySwellHash);
		}

		// Nothing new is hashed and nothing steps differently without a Buoyancy: a box falling past a sea's surface lands as
		// it does with no water at all.
		TEST_CASE("Buoyancy: a scene without Buoyancy steps as before")
		{
			const auto land = [](bool water)
			{
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				static_cast<void>(Test::AddGround(scene));
				if (water)
					static_cast<void>(AddSea(scene, MakeShortSwell(), 1.0f));
				static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.25f, 3.0f, -0.5f), glm::vec3(0.5f), BodyType::Dynamic));
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				Test::RunTicks(**session, 120);
				const Entity box = (*session)->GetScene().FindEntityByPath("/Box");
				REQUIRE(box.IsValid());
				return box.GetComponent<TransformComponent>();
			};
			const TransformComponent dry = land(false);
			const TransformComponent wet = land(true);
			CHECK(dry.Translation == wet.Translation);
			CHECK(dry.Rotation == wet.Rotation);
		}
	}

}
