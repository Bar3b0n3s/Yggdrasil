#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/ParticleStateComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/ParticleSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// The play session's particles (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decisions 5 and 6): the Particles
// frame phase's place (ADR 0028's phase-3 execution order), the live guard that particles never change the state hash, and
// the phase's stepping, which stream P-A implements.

namespace Engine {

	namespace {

		class PhaseRecorder final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& /*session*/, PlaySessionPhase phase, uint64_t /*tick*/) override { Phases.push_back(phase); }

			std::vector<PlaySessionPhase> Phases;
		};

		// The index of `phase` in `phases`, or the list's size when it is absent.
		size_t FindPhase(const std::vector<PlaySessionPhase>& phases, PlaySessionPhase phase)
		{
			return static_cast<size_t>(std::distance(phases.begin(), std::find(phases.begin(), phases.end(), phase)));
		}

		// A busy emitter riding a falling box: it emits every tick, bursts, moves with its body and draws from its own seeded
		// stream, so a phase that wrote any hashed state would show in the session's hash.
		Entity AddFallingEmitter(Scene& scene)
		{
			Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Entity emitter = scene.CreateEntity("Sparks");
			REQUIRE(scene.SetParent(emitter, box, std::nullopt, false));
			emitter.AddComponent<ParticleEmitterComponent>(ParticleEmitterComponent{ .Rate = 600.0f,
				.Burst = 32,
				.MaxParticles = 512,
				.Seed = 7,
				.Shape = ParticleEmitterShape::Sphere,
				.Acceleration = glm::vec3(0.0f, -9.81f, 0.0f),
				.Drag = 0.5f });
			return emitter;
		}

	}

	TEST_SUITE("Session")
	{
		// The ledger fixes the place: a frame phase after AudioUpdate (and so after Update, LateUpdate and the frame's transform
		// update, which move emitters and queue emits) and right before RenderExtraction, which copies the particles.
		TEST_CASE("PlaySessionPhase: Particles has its enumerator name and follows AudioUpdate, right before RenderExtraction")
		{
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::Particles) == "Particles");
			CHECK(std::to_underlying(PlaySessionPhase::Particles) > std::to_underlying(PlaySessionPhase::AudioUpdate));
			CHECK(std::to_underlying(PlaySessionPhase::Particles) + 1 == std::to_underlying(PlaySessionPhase::RenderExtraction));
			CHECK(std::to_underlying(PlaySessionPhase::Particles) > std::to_underlying(PlaySessionPhase::FrameTransformUpdate));
		}

		// The live guard (decision 6; ADR 0028's phase-3 ledger: "Particles are presentation and enter neither the scene's
		// serialization nor a block, so they never change the state hash"): a scene holding a busy emitter on a falling body
		// has, after 60 lockstep ticks, the state hashes it had at the contract, where the session entered no Particles phase
		// and simulated no particle. So stream P-A's phase, its runtime state and its stepping add no hash block, no serialized
		// state and no hashed write, in Play and in Simulate. A scene without an emitter is the common case of the same check.
		TEST_CASE("PlaySession: particles never change the state hash")
		{
			// The scene holds no script, so Play and Simulate step it alike and give one hash each.
			for (const auto& [withEmitter, expected] : { std::pair{ true, std::string_view("777f0d69345840c8") }, std::pair{ false, std::string_view("492ca3cbba8a1f07") } })
			{
				for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
				{
					CAPTURE(withEmitter);
					CAPTURE(static_cast<int>(mode));
					Test::SceneTestFixture fixture;
					Scene& scene = fixture.GetScene();
					static_cast<void>(Test::AddGround(scene));
					scene.CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
					if (withEmitter)
						static_cast<void>(AddFallingEmitter(scene));
					else
						static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
					Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 42, mode));
					REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
					Test::RunTicks(**session, 60);
					CHECK(std::format("{:016x}", (*session)->ComputeStateHash()) == expected);
				}
			}
		}

		// Stream P-A enters the phase: in Play and Simulate, after AudioUpdate (Play only) and right before RenderExtraction,
		// with the session's ParticleSystem; the streams that enter a phase add it to PlaySessionTests.cpp's phase lists at its
		// place (ADR 0028's phase-3 ledger).
		TEST_CASE("PlaySession: the Particles phase runs before the extraction, in Simulate too" * doctest::skip(true))
		{
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				CAPTURE(static_cast<int>(mode));
				Test::SceneTestFixture fixture;
				PhaseRecorder recorder;
				PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, 42, mode);
				specification.Observer = &recorder;
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				REQUIRE((*session)->GetParticles() != nullptr);
				(*session)->Tick();
				const size_t particles = FindPhase(recorder.Phases, PlaySessionPhase::Particles);
				REQUIRE(particles < recorder.Phases.size());
				CHECK(FindPhase(recorder.Phases, PlaySessionPhase::FrameTransformUpdate) < particles);
				if (mode == PlayMode::Play)
					CHECK(FindPhase(recorder.Phases, PlaySessionPhase::AudioUpdate) < particles);
				CHECK(particles + 1 == FindPhase(recorder.Phases, PlaySessionPhase::RenderExtraction));
			}
		}

		// Decision 6: one emitter step per tick the frame ran, never per frame: a paused session's frames step nothing, and a
		// frame after three fixed steps steps three times, so the particles depend on the ticks alone.
		TEST_CASE("ParticleSystem: the phase steps each emitter once per tick the frame ran, so a paused session freezes its particles" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(AddFallingEmitter(scene));
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 42, PlayMode::Simulate));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Test::RunTicks(**session, 10);
			const Entity sparks = (*session)->GetScene().FindEntityByPath("/Box/Sparks");
			REQUIRE(sparks.IsValid());
			REQUIRE(sparks.HasComponent<ParticleStateComponent>());
			CHECK(sparks.GetComponent<ParticleStateComponent>().State.Steps == 10);
			const std::optional<uint64_t> before = FindParticleDigest(sparks);
			REQUIRE(before.has_value());
			// Frames without a tick (a paused session's) step nothing.
			(*session)->FrameUpdate(FrameTime{ .DeltaTime = 0.25, .UnscaledDeltaTime = 0.25, .Alpha = 0.5, .FrameIndex = 100 });
			CHECK(FindParticleDigest(sparks) == before);
			// Three fixed steps, then one frame: three steps.
			for (int step = 0; step < 3; ++step)
				(*session)->FixedStep();
			(*session)->FrameUpdate(FrameTime{ .DeltaTime = 0.05, .UnscaledDeltaTime = 0.05, .Alpha = 1.0, .FrameIndex = 101 });
			CHECK(sparks.GetComponent<ParticleStateComponent>().State.Steps == 13);
			CHECK((*session)->GetParticles()->GetStats().Steps == 3);
		}

		// Decision 6 (C26.1's review): a state starts at its first Particles phase, whoever made it and whenever. An emitter
		// spawned at tick 600 and emitted at once (as a script's OnUpdate would) gets its state from Emit, unanchored; the next
		// frame's phase anchors it and steps it once, so its queued particles show in that frame, newly born, instead of being
		// spawned and aged through the 600 ticks before the emitter existed.
		TEST_CASE("ParticleSystem: an emitter spawned and emitted at tick 600 starts there, its particles shown in the next frame" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 42, PlayMode::Simulate));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Test::RunTicks(**session, 600);
			REQUIRE((*session)->GetTick() == 600);
			Entity burst = (*session)->GetScene().CreateEntity("Burst");
			burst.AddComponent<ParticleEmitterComponent>(ParticleEmitterComponent{ .Rate = 0.0f, .MaxParticles = 64 });
			REQUIRE(QueueParticleEmit(burst, 50));
			REQUIRE(burst.HasComponent<ParticleStateComponent>());
			CHECK_FALSE(burst.GetComponent<ParticleStateComponent>().LastTick.has_value());
			CHECK_FALSE(FindParticleDigest(burst).has_value());

			(*session)->Tick();
			const ParticleStateComponent& state = burst.GetComponent<ParticleStateComponent>();
			CHECK(state.LastTick == std::optional<uint64_t>(601));
			CHECK(state.State.Steps == 1);
			CHECK((*session)->GetParticles()->GetStats().Steps == 1);
			REQUIRE(state.State.Particles.size() == 50);
			for (const Particle& particle : state.State.Particles)
				CHECK(particle.Age == 0.0f);
			CHECK(FindParticleDigest(burst).has_value());
		}
	}

}
