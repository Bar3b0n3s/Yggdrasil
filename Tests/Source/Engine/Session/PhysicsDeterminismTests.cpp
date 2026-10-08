#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Core/Hash.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

// Physics determinism through the play session's state hash (Architecture §9.1, §9.7: "A 200-body pile stepped 600 times
// gives the same ComputeStateHash() across repeated runs and with Jolt thread counts 0, 1 and 8. Contact-event order is
// identical across runs. The same pile's final hash is a committed constant checked in the Debug and Release unit runs";
// Roadmap M11 acceptance, names verbatim). The acceptance cases are skipped skeletons of the M11 contract
// (Docs/Decisions/0014-m11-decisions.md): stream B makes them pass and removes the skips.

namespace Engine {

	namespace {

		// The pile's state hash after 600 ticks, recorded by stream B from the first run of the finished implementation (the
		// same in Debug, Release and Dist, §9.1; ADR 0014 decision 19). A change of physics or of the pile that legitimately
		// changes the simulation updates it with the reason in the change's review (§13.6 "Re-recording").
		constexpr uint64_t CommittedPileHash = 0x0000000000000000;

		constexpr uint32_t PileTicks = 600;

		// 200 boxes of three sizes in a 5 x 5 footprint, 8 layers high, each layer offset and rotated a little so the pile
		// topples and keeps many contacts busy, over a ground; positions are exact binary fractions.
		void BuildPile(Scene& scene)
		{
			static_cast<void>(Test::AddGround(scene));
			for (int layer = 0; layer < 8; ++layer)
			{
				for (int row = 0; row < 5; ++row)
				{
					for (int column = 0; column < 5; ++column)
					{
						const float size = 0.25f + 0.125f * static_cast<float>((row + column + layer) % 3);
						const glm::vec3 position(static_cast<float>(column) * 1.25f + 0.0625f * static_cast<float>(layer % 2),
							0.5f + static_cast<float>(layer) * 1.25f, static_cast<float>(row) * 1.25f - 0.0625f * static_cast<float>(layer % 3));
						Entity box = Test::AddBoxBody(scene, "Box", position, glm::vec3(size), BodyType::Dynamic);
						box.Patch<TransformComponent>([layer](TransformComponent& transform)
						{
							transform.Rotation = glm::normalize(glm::quat(1.0f, 0.0f, 0.125f * static_cast<float>(layer % 4), 0.0f));
						});
					}
				}
			}
		}

		// Sets the process's Jolt worker thread count for its lifetime and restores the previous count on every exit path, a
		// failed REQUIRE included, so no later test runs with another count (tests never depend on order, CodeStyle §14).
		class ScopedWorkerThreadCount
		{
		public:
			explicit ScopedWorkerThreadCount(uint32_t count)
				: m_Original(PhysicsEngine::GetWorkerThreadCount())
			{
				REQUIRE(PhysicsEngine::SetWorkerThreadCount(count).has_value());
			}

			~ScopedWorkerThreadCount()
			{
				CHECK(PhysicsEngine::SetWorkerThreadCount(m_Original).has_value());
			}

			ScopedWorkerThreadCount(const ScopedWorkerThreadCount&) = delete;
			ScopedWorkerThreadCount& operator=(const ScopedWorkerThreadCount&) = delete;
		private:
			uint32_t m_Original = 0;
		};

		// The pile's hash after PileTicks ticks with `workerThreads` Jolt threads; `events`, when given, receives every event.
		// It REQUIREs, so callers keep it out of CHECK expressions.
		uint64_t RunPile(uint32_t workerThreads, std::vector<PhysicsEvent>* events = nullptr)
		{
			const ScopedWorkerThreadCount threads(workerThreads);
			Test::SceneTestFixture fixture;
			BuildPile(fixture.GetScene());
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 1234));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Test::RecordingPhysicsListener listener;
			(*session)->GetPhysics().SetEventListener(&listener);
			Test::RunTicks(**session, PileTicks);
			const uint64_t hash = (*session)->ComputeStateHash();
			(*session)->GetPhysics().SetEventListener(nullptr);
			if (events != nullptr)
				*events = listener.Events;
			return hash;
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("Physics: state hash identical with 0, 1 and 8 Jolt threads" * doctest::skip(true))
		{
			const uint64_t none = RunPile(0);
			const uint64_t one = RunPile(1);
			const uint64_t eight = RunPile(8);
			// And across repeated runs with the same count.
			const uint64_t again = RunPile(0);
			CHECK(one == none);
			CHECK(eight == none);
			CHECK(again == none);
		}

		TEST_CASE("Physics: 200-body pile hash equals the committed constant" * doctest::skip(true))
		{
			// One committed value for every configuration (§9.1: a configuration-dependent result is a bug).
			const uint64_t hash = RunPile(PhysicsEngine::GetWorkerThreadCount());
			INFO("pile hash: ", std::format("{:016x}", hash));
			CHECK(hash == CommittedPileHash);
		}

		TEST_CASE("Physics: contact event order identical across runs" * doctest::skip(true))
		{
			std::vector<PhysicsEvent> first;
			std::vector<PhysicsEvent> second;
			std::vector<PhysicsEvent> threaded;
			static_cast<void>(RunPile(1, &first));
			static_cast<void>(RunPile(1, &second));
			static_cast<void>(RunPile(8, &threaded));
			REQUIRE(first.size() > 200);
			CHECK(second == first);
			CHECK(threaded == first);
		}

		TEST_CASE("PlaySession: a session without physics components hashes as it did before physics")
		{
			// AppendStateHash appends nothing without bodies, so M7's hashes of physics-free scenes stay valid.
			Test::SceneTestFixture fixture;
			static_cast<void>(fixture.GetScene().CreateEntity("Marker"));
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE(session.has_value());
			Test::RunTicks(**session, 5);
			XXH64Hasher withPhysics(0);
			(*session)->GetPhysics().AppendStateHash(withPhysics);
			CHECK(withPhysics.Digest() == XXH64Hasher(0).Digest());
		}
	}

}
