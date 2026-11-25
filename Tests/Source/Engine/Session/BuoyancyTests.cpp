#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/BuoyancyComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scene/WaterValidation.h"
#include "Support/PhysicsTestScene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

// Buoyancy in the play session (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §9.12): PhysicsSystem's PreStep
// item 4c floats every Buoyancy's Dynamic body on the planes its colliders' waters give at the tick's world time, a body on
// calm, unchanged water sleeps and a change wakes it, and the result is deterministic across Jolt thread counts. Phase 8's
// landing: 4c visits its owners alone (M43's EntitiesVisited), and a body it skipped asleep that item 5 wakes (a character's
// weight) floats in that step too.

namespace Engine {

	namespace {

		// The 16 swell bodies' state hash after BuoyancySwellTicks ticks, recorded by W47.2's physics stream from its
		// implementation and verified equal in the Debug and Release unit runs (§9.1; decision 6). A change of the buoyancy or of
		// the wave function that legitimately changes the simulation updates it with the reason in the change's review: W47.2's
		// review raised the Buoyancy's default LinearDrag from 0.5 to 2 (0x5839cad91ba7b98a before). Phase 8's landing re-recorded
		// it: W47.3's review fixes changed Water's default UnderwaterColor, which the session hash's scene block serializes, so
		// the hash moved while the simulation did not (UnderwaterColor is render-only; 0x4c1f812d6448c124 before, and M47's own
		// tip gives the new value too).
		constexpr uint64_t CommittedBuoyancySwellHash = 0x08c2e9fe6252aab9;
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
		TEST_CASE("Buoyancy: two pontoons pitch with a swell's slope")
		{
			// On calm water the raft settles level. On a 6 m swell the pontoons, 3 m apart, are half a wavelength apart: one rides
			// a crest while the other sits in a trough, each on the surface under it, so the raft pitches with the slope between them.
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

		TEST_CASE("Buoyancy: a buoyant body on calm water sleeps and a tide wakes it")
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
			// The buoyancy block of physics.bodyInfo: the sea; the raft's two 1 m x 0.5 m x 1 m pontoons (half extents 0.5, 0.25,
			// 0.5), 0.5 m^3 each, so 1 m^3 in all; and rho V_sub = m, so the 200 kg raft keeps 200 / 1000 = 0.2 m^3 submerged.
			report = play.GetPhysics().GetBodyInfo(raft);
			REQUIRE(report.has_value());
			REQUIRE(report->Buoyancy.has_value());
			CHECK(report->Buoyancy->Water == Test::GetEntityId(play.GetScene(), "/Sea"));
			CHECK(report->Buoyancy->TotalVolume == doctest::Approx(1.0f).epsilon(1e-4));
			CHECK(report->Buoyancy->SubmergedVolume == doctest::Approx(0.2f).epsilon(0.05));
		}

		TEST_CASE("Buoyancy: 16 bodies on a swell end at their committed hash at Jolt thread counts 0, 1 and 8")
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

		// A Buoyancy on a Kinematic body, and one naming no Water, float nothing: each is skipped, reported once (the run-time
		// PHYSICS_BUOYANCY_INVALID, on the component and field project.validate names, the Water checked first) and reports zero
		// volumes, while a Buoyancy naming a pool floats on the pool alone, at the pool's density, until the pool is disabled.
		TEST_CASE("Buoyancy: a Buoyancy that cannot float is skipped and reported once, and a named water floats its body alone")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddSea(scene, {}, 1.0f));
			Entity pool = scene.CreateEntity("Pool");
			pool.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(20.0f, 3.0f, 0.0f);
			});
			pool.AddComponent<WaterComponent>(
				WaterComponent{ .Shape = WaterShape::Rectangle, .Size = glm::vec2(10.0f), .Depth = 4.0f, .Headroom = 2.0f, .Density = 2000.0f });
			Entity lift = Test::AddBoxBody(scene, "Lift", glm::vec3(-6.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Kinematic);
			lift.AddComponent<BuoyancyComponent>();
			Entity lost = Test::AddBoxBody(scene, "Lost", glm::vec3(-12.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			lost.AddComponent<BuoyancyComponent>(BuoyancyComponent{ .Water = lift.GetUUID() });
			// Kinematic and naming no Water: the Water is reported, as project.validate reports it.
			Entity stray = Test::AddBoxBody(scene, "Stray", glm::vec3(-18.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Kinematic);
			stray.AddComponent<BuoyancyComponent>(BuoyancyComponent{ .Water = lift.GetUUID() });
			// A 250 kg crate placed at its draft in the pool of density 2000, 3 m above the sea: rho V_sub = m gives 0.125 m^3 of
			// its 1 m^3 under the pool's surface, its bottom 0.125 m under it and its centre 0.375 m above it, at y = 3.375.
			Entity crate = Test::AddBoxBody(scene, "Crate", glm::vec3(20.0f, 3.375f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(crate, [](RigidBodyComponent& body)
			{
				body.Mass = 250.0f;
			});
			crate.AddComponent<BuoyancyComponent>(BuoyancyComponent{ .Water = pool.GetUUID() });
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			Test::RunTicks(play, 600);
			const auto countInvalid = [&play](UUID entity)
			{
				return std::ranges::count_if(play.GetPhysics().GetDiagnostics(), [entity](const PhysicsDiagnostic& diagnostic)
				{
					return diagnostic.Entity == entity && diagnostic.Code == PhysicsBuoyancyInvalidCode;
				});
			};
			CHECK(countInvalid(lift.GetUUID()) == 1);
			CHECK(countInvalid(lost.GetUUID()) == 1);
			CHECK(countInvalid(stray.GetUUID()) == 1);
			CHECK(countInvalid(crate.GetUUID()) == 0);
			const auto expectAt = [&play](UUID entity, std::string_view component, std::string_view field)
			{
				for (const PhysicsDiagnostic& diagnostic : play.GetPhysics().GetDiagnostics())
				{
					if (diagnostic.Entity != entity || diagnostic.Code != PhysicsBuoyancyInvalidCode)
						continue;
					CHECK(diagnostic.Severity == DiagnosticSeverity::Warning);
					CHECK(diagnostic.Component == component);
					CHECK(diagnostic.Field == field);
				}
			};
			expectAt(lift.GetUUID(), "RigidBody", "Type");
			expectAt(lost.GetUUID(), "Buoyancy", "Water");
			expectAt(stray.GetUUID(), "Buoyancy", "Water");
			const std::optional<PhysicsBodyReport> lostReport = play.GetPhysics().GetBodyInfo(lost.GetUUID());
			REQUIRE(lostReport.has_value());
			REQUIRE(lostReport->Buoyancy.has_value());
			CHECK(lostReport->Buoyancy->TotalVolume == 0.0f);
			CHECK_FALSE(lostReport->Buoyancy->Water.IsValid());
			// The crate floats on the pool at the pool's density, with 0.125 m^3 of its 1 m^3 submerged.
			CHECK(Test::GetWorldPosition(play, "/Crate").y == doctest::Approx(3.375f).epsilon(0.01));
			std::optional<PhysicsBodyReport> crateReport = play.GetPhysics().GetBodyInfo(crate.GetUUID());
			REQUIRE(crateReport.has_value());
			REQUIRE(crateReport->Buoyancy.has_value());
			CHECK(crateReport->Buoyancy->Water == pool.GetUUID());
			CHECK(crateReport->Buoyancy->SubmergedVolume == doctest::Approx(0.125f).epsilon(0.05));
			// The pool drained: a disabled water holds no point, so the crate falls toward the sea, unreported.
			play.GetScene().FindEntityByPath("/Pool").SetActive(false);
			Test::RunTicks(play, 60);
			crateReport = play.GetPhysics().GetBodyInfo(crate.GetUUID());
			REQUIRE(crateReport.has_value());
			REQUIRE(crateReport->Buoyancy.has_value());
			CHECK(crateReport->Buoyancy->SubmergedVolume == 0.0f);
			CHECK(Test::GetWorldPosition(play, "/Crate").y < 3.25f);
			CHECK(countInvalid(crate.GetUUID()) == 0);
		}

		// The wake rule reads the settings too: a sleeping raft whose BuoyancyScale is written (its hull holed) is woken at the
		// next tick, though its planes are unchanged, and sinks to its new draft.
		TEST_CASE("Buoyancy: a write of a sleeping body's BuoyancyScale wakes it")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddSea(fixture.GetScene(), {}, 1.0f));
			static_cast<void>(AddRaft(fixture.GetScene(), "Raft", glm::vec3(0.0f, 0.1f, 0.0f)));
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			const UUID raft = Test::GetEntityId(play.GetScene(), "/Raft");
			Test::RunTicks(play, 900);
			std::optional<PhysicsBodyReport> report = play.GetPhysics().GetBodyInfo(raft);
			REQUIRE(report.has_value());
			REQUIRE(report->IsSleeping);
			REQUIRE(report->Buoyancy.has_value());
			const float restVolume = report->Buoyancy->SubmergedVolume;
			// 0.3 of the buoyancy: the 200 kg raft needs 0.2 / 0.3 = 0.67 m^3 of its 1 m^3 under water.
			play.GetScene().FindEntityByPath("/Raft").Patch<BuoyancyComponent>([](BuoyancyComponent& buoyancy)
			{
				buoyancy.BuoyancyScale = 0.3f;
			});
			Test::RunTicks(play, 1);
			report = play.GetPhysics().GetBodyInfo(raft);
			REQUIRE(report.has_value());
			CHECK_FALSE(report->IsSleeping);
			Test::RunTicks(play, 600);
			report = play.GetPhysics().GetBodyInfo(raft);
			REQUIRE(report.has_value());
			REQUIRE(report->Buoyancy.has_value());
			CAPTURE(restVolume);
			CHECK(report->Buoyancy->SubmergedVolume > restVolume + 0.3f);
			CHECK(report->Buoyancy->SubmergedVolume == doctest::Approx(0.2f / 0.3f).epsilon(0.05));
		}

		// Jolt gives a body whose translation is fully locked an inverse mass of 0, which its buoyancy divides by: a raft that may
		// only rock is skipped and reported once (RigidBody.LockTranslation), and its rotation stays finite.
		TEST_CASE("Buoyancy: a raft that cannot translate is reported once and its rotation stays finite")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(AddSea(fixture.GetScene(), MakeShortSwell(), 1.0f));
			Entity raft = AddRaft(fixture.GetScene(), "Raft", glm::vec3(0.0f));
			Test::PatchRigidBody(raft, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.InitialAngularVelocity = glm::vec3(0.0f, 0.0f, 0.5f);
			});
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			Test::RunTicks(play, 120);
			const UUID id = Test::GetEntityId(play.GetScene(), "/Raft");
			std::vector<PhysicsDiagnostic> reported;
			std::ranges::copy_if(play.GetPhysics().GetDiagnostics(), std::back_inserter(reported), [id](const PhysicsDiagnostic& diagnostic)
			{
				return diagnostic.Entity == id && diagnostic.Code == PhysicsBuoyancyInvalidCode;
			});
			REQUIRE(reported.size() == 1);
			CHECK(reported[0].Component == "RigidBody");
			CHECK(reported[0].Field == "LockTranslation");
			const std::optional<PhysicsBodyReport> report = play.GetPhysics().GetBodyInfo(id);
			REQUIRE(report.has_value());
			for (int axis = 0; axis < 3; ++axis)
			{
				CHECK(std::isfinite(report->AngularVelocity[axis]));
				CHECK(std::isfinite(report->Pose.Position[axis]));
			}
			for (int axis = 0; axis < 4; ++axis)
				CHECK(std::isfinite(report->Pose.Rotation[axis]));
			REQUIRE(report->Buoyancy.has_value());
			CHECK(report->Buoyancy->TotalVolume == 0.0f);
		}

		// The report's water when the Buoyancy names none: the water whose planes held the most of the submerged volume, a tie
		// going to the water of the lowest collider index (the raft's Left pontoon, its first child, is collider 0).
		TEST_CASE("Buoyancy: the report names the water holding the most submerged volume, a tie the lowest collider's")
		{
			// The raft's pontoons at y = 0 (each 1 m x 0.5 m x 1 m): Left over the sea at 0, Right in a pool around x = 1.5.
			const auto reportedWater = [](float poolHeight, float raftHeight)
			{
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				const Entity sea = AddSea(scene, {}, 1.0f);
				Entity pool = scene.CreateEntity("Pool");
				pool.Patch<TransformComponent>([poolHeight](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(1.5f, poolHeight, 0.0f);
				});
				pool.AddComponent<WaterComponent>(WaterComponent{ .Shape = WaterShape::Rectangle, .Size = glm::vec2(2.0f), .Depth = 5.0f, .Headroom = 2.0f });
				static_cast<void>(AddRaft(scene, "Raft", glm::vec3(0.0f, raftHeight, 0.0f)));
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				Test::RunTicks(**session, 1);
				const std::optional<PhysicsBodyReport> report = (*session)->GetPhysics().GetBodyInfo(Test::GetEntityId((*session)->GetScene(), "/Raft"));
				REQUIRE(report.has_value());
				REQUIRE(report->Colliders.size() == 2);
				CHECK(report->Colliders[0] == Test::GetEntityId((*session)->GetScene(), "/Raft/Left"));
				REQUIRE(report->Buoyancy.has_value());
				const UUID water = report->Buoyancy->Water;
				if (water == sea.GetUUID())
					return std::string("Sea");
				return water == pool.GetUUID() ? std::string("Pool") : std::string("none");
			};
			// The pool's surface at 0.5: Right is wholly under it (0.5 m^3), Left half under the sea (0.25 m^3).
			CHECK(reportedWater(0.5f, 0.0f) == "Pool");
			// At -0.1: Right holds 0.15 m^3 and Left still 0.25 m^3.
			CHECK(reportedWater(-0.1f, 0.0f) == "Sea");
			// Both wholly under their waters, 0.5 m^3 each: the tie goes to Left's, the sea.
			CHECK(reportedWater(0.0f, -2.0f) == "Sea");
		}

		// The fluid's velocity is the waves' plus the water's Current: a box floating on calm water with a current is carried by it.
		TEST_CASE("Buoyancy: a floating body is carried by its water's Current")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity sea = AddSea(scene, {}, 1.0f);
			sea.Patch<WaterComponent>([](WaterComponent& water)
			{
				water.Current = glm::vec2(1.0f, 0.0f);
			});
			Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(box, [](RigidBodyComponent& body)
			{
				body.Mass = 500.0f;
			});
			box.AddComponent<BuoyancyComponent>();
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Test::RunTicks(**session, 120);
			const std::optional<PhysicsBodyReport> report = (*session)->GetPhysics().GetBodyInfo(box.GetUUID());
			REQUIRE(report.has_value());
			CAPTURE(report->LinearVelocity.x);
			CHECK(report->LinearVelocity.x > 0.0f);
			CHECK(Test::GetWorldPosition(**session, "/Box").x > 0.0f);
		}

		// Phase 8's landing (ADR 0060's ledger, "Rules for every pass a later lander adds"; the merge review's finding 1): item 4c
		// visits the owners of a Buoyancy alone (Scene::CollectCanonical), as M43's "PhysicsSystem: a tick visits only the moving
		// bodies and the bodies whose transforms changed" counts the other passes. Rafts asleep on calm water among plain
		// entities: a step visits each raft twice, once in the write-back (every Dynamic body's owner) and once in 4c, and
		// nothing else, however many plain entities the scene holds.
		TEST_CASE("Buoyancy: item 4c visits only its Buoyancy owners")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddSea(scene, {}, 1.0f));
			constexpr int RaftCount = 3;
			std::vector<UUID> rafts;
			for (int index = 0; index < RaftCount; ++index)
				rafts.push_back(AddRaft(scene, "Raft", glm::vec3(8.0f * static_cast<float>(index), 0.1f, 0.0f)).GetUUID());
			const auto addProps = [&scene](int count)
			{
				for (int index = 0; index < count; ++index)
				{
					Entity prop = scene.CreateEntity("Prop");
					prop.Patch<TransformComponent>([index](TransformComponent& transform)
					{
						transform.Translation = glm::vec3(2.0f * static_cast<float>(index), 5.0f, 10.0f);
					});
				}
			};
			addProps(40);
			TransformSystem::Update(scene);
			PhysicsSystemSpecification specification;
			specification.RuntimeScene = &scene;
			Result<Scope<PhysicsSystem>> created = PhysicsSystem::Create(specification);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PhysicsSystem& physics = **created;
			// The transform update, then one step: the entities the step's passes visited.
			const auto stepVisits = [&scene, &physics](uint64_t tick)
			{
				TransformSystem::Update(scene);
				const uint64_t visited = scene.GetEntitiesVisited();
				const SimStep step = SimStep::FromTick(tick, 1.0 / 60.0);
				physics.PreStep(step);
				physics.Step(step);
				physics.PostStep(step);
				return scene.GetEntitiesVisited() - visited;
			};
			const auto asleep = [&physics, &rafts]()
			{
				return std::ranges::all_of(rafts, [&physics](const UUID raft)
				{
					const std::optional<PhysicsBodyReport> report = physics.GetBodyInfo(raft);
					return report.has_value() && report->IsSleeping;
				});
			};

			// Settled on calm, unchanged water, every raft sleeps.
			uint64_t tick = 0;
			for (; tick < 1800 && !asleep(); ++tick)
				static_cast<void>(stepVisits(tick));
			REQUIRE(asleep());
			CHECK(stepVisits(tick++) == 2 * RaftCount);
			// Forty more plain entities: the step that adds them may look at them, the next ones visit the rafts alone again.
			addProps(40);
			static_cast<void>(stepVisits(tick++));
			REQUIRE(asleep());
			CHECK(stepVisits(tick++) == 2 * RaftCount);
		}

		// Phase 8's landing (the merge review's finding 10: M47's wake rule with M48's light bodies): a raft lighter than the
		// character standing still on it, on calm water. Once the raft slept, item 4c skipped it (asleep, its planes and settings
		// bit-identical), and item 5's ground push (CharacterController's PushGround, at most the raft's own weight) woke it for
		// a step without buoyancy, which dropped it by about two ticks' fall (2 g dt^2, 5.4 mm) each time it fell asleep. A
		// sleeping body that a grounded character stood on in its last update is now woken and floated by 4c, before item 5, as
		// in an awake tick, so the raft keeps its draft, within 1 mm, over the 3 s after it first sleeps once settled, at Jolt
		// thread counts 0, 1 and 8 alike. The raft moves only up and down (its other axes locked), so its colliders' bounds keep
		// their centre's x and z to the bit, and its planes stay bit-identical while it rests: a raft that drifts a little takes
		// new planes at every tick, and 4c wakes it for them.
		TEST_CASE("Buoyancy: a raft a character stands on in calm water keeps its draft when its rider wakes it")
		{
			struct Ride
			{
				float Settled = 0.0f;
				float Lowest = 0.0f;
				uint32_t Sleeps = 0;
				uint64_t Hash = 0;
			};
			const auto ride = [](uint32_t workerThreads)
			{
				const Test::ScopedWorkerThreadCount threads(workerThreads);
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				static_cast<void>(AddSea(scene, {}, 1.0f));
				// A 50 kg raft 3 m x 0.5 m x 3 m, lighter than the 70 kg rider: its draft is (50 + 50) / (1000 x 9) m, about 1.1 cm.
				Entity raft = scene.CreateEntity("Raft");
				raft.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(0.0f, 0.24f, 0.0f);
				});
				RigidBodyComponent body;
				body.Type = BodyType::Dynamic;
				body.Mass = 50.0f;
				body.LockTranslation = glm::bvec3(true, false, true);
				body.LockRotation = glm::bvec3(true);
				raft.AddComponent<RigidBodyComponent>(body);
				raft.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(1.5f, 0.25f, 1.5f) });
				raft.AddComponent<BuoyancyComponent>();
				Entity rider = scene.CreateEntity("Rider");
				rider.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(0.0f, 1.6f, 0.0f);
				});
				rider.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				PlaySession& play = **session;
				const UUID raftId = Test::GetEntityId(play.GetScene(), "/Raft");
				const UUID riderId = Test::GetEntityId(play.GetScene(), "/Rider");
				const auto sleeping = [&play, raftId]()
				{
					const std::optional<PhysicsBodyReport> report = play.GetPhysics().GetBodyInfo(raftId);
					REQUIRE(report.has_value());
					return report->IsSleeping;
				};

				// The rider lands and stands still, and the raft settles under it for 10 s (Jolt may put a body to sleep while it
				// still rocks a little, so the first sleeps can stop it off its draft), then falls asleep at the end of a step.
				Test::RunTicks(play, 600);
				uint32_t tick = 600;
				for (; tick < 1800 && !sleeping(); ++tick)
					Test::RunTicks(play, 1);
				CAPTURE(tick);
				REQUIRE(sleeping());
				const Result<PhysicsCharacterState> standing = play.GetPhysics().GetCharacterState(riderId);
				REQUIRE(standing.has_value());
				REQUIRE(standing->IsGrounded);
				CHECK(standing->GroundCollider == raftId);

				// The next 3 s: the raft falls asleep again and again, and wakes under its rider each time.
				Ride result{ .Settled = Test::GetWorldPosition(play, "/Raft").y };
				result.Lowest = result.Settled;
				for (uint32_t step = 0; step < 180; ++step)
				{
					Test::RunTicks(play, 1);
					result.Lowest = std::min(result.Lowest, Test::GetWorldPosition(play, "/Raft").y);
					result.Sleeps += sleeping() ? 1 : 0;
				}
				result.Hash = play.ComputeStateHash();
				return result;
			};
			const Ride none = ride(0);
			for (const uint32_t workerThreads : { 0U, 1U, 8U })
			{
				CAPTURE(workerThreads);
				const Ride run = workerThreads == 0 ? none : ride(workerThreads);
				CAPTURE(run.Settled);
				CAPTURE(run.Lowest);
				CAPTURE(run.Sleeps);
				CHECK(run.Sleeps > 0);
				CHECK(run.Settled - run.Lowest < 0.001f);
				CHECK(run.Hash == none.Hash);
				CHECK(run.Lowest == none.Lowest);
			}
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
