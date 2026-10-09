#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/AudioListenerComponent.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/GlmApprox.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <cstdint>
#include <utility>
#include <vector>

// The play session's audio hook (Architecture §5.6, §5.7 frame phase step 3, §10.1 time ownership, §10.2 pause and stop;
// Docs/Decisions/0015-m12-decisions.md decision 13). Every test but the first runs a device-less AudioEngine with
// deterministic decoding. The sources play the built-in silent clip (looping, so the voices live across many ticks); the
// session has no asset manager, so AudioSystem plays the silent clip for every clip without loading anything. The last
// test hears a physics body (Support/PhysicsTestScene) at its rendered pose through SystemClock-like frames
// (Docs/Decisions/0016-m8-m11-m12-integration.md decision 8).

namespace Engine {

	namespace {

		// A device-less engine with deterministic decoding; fails the test case on error.
		Scope<AudioEngine> CreateSessionEngine(const VirtualFileSystem& vfs)
		{
			const AudioEngineSpecification deviceLess{ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic };
			Result<Scope<AudioEngine>> engine = AudioEngine::Create(deviceLess, vfs);
			REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
			return std::move(*engine);
		}

		// An edit scene with a listener and two looping sources, one of them PlayOnStart.
		void PopulateAudioScene(Scene& scene)
		{
			Entity listener = scene.CreateEntity("Listener");
			listener.AddComponent<AudioListenerComponent>();
			AudioSourceComponent source;
			source.Clip.SetHandle(BuiltinAssetHandles::SilentClip);
			source.Loop = true;
			source.PlayOnStart = true;
			Entity autoplay = scene.CreateEntity("Autoplay");
			autoplay.AddComponent<AudioSourceComponent>(source);
			source.PlayOnStart = false;
			Entity manual = scene.CreateEntity("Manual");
			manual.AddComponent<AudioSourceComponent>(source);
		}

		PlaySessionSpecification MakeAudioSpecification(Test::SceneTestFixture& fixture, AudioEngine* audio, PlayMode mode = PlayMode::Play)
		{
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = 42;
			specification.Mode = mode;
			specification.Audio = audio;
			return specification;
		}

		// A session of the fixture's scene; fails the test case on error.
		Scope<PlaySession> StartSession(Test::SceneTestFixture& fixture, const PlaySessionSpecification& specification)
		{
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			return std::move(*session);
		}

		// The one live voice of the engine; fails the test case when there is not exactly one.
		AudioVoiceInfo GetOnlyVoice(const AudioEngine& engine)
		{
			std::vector<AudioVoiceInfo> voices = engine.GetVoices();
			REQUIRE(voices.size() == 1);
			return std::move(voices.front());
		}

		// A frame phase of 1/60 s that follows no fixed step of its own (a ScriptedClock frame).
		constexpr FrameTime SixtiethFrame{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 0.0, .FrameIndex = 0 };

		// The constant velocity of the ball that FollowCamera follows, and where the camera sits relative to it.
		constexpr glm::vec3 BallVelocity(10.0f, 0.0f, 0.0f);
		constexpr glm::vec3 FollowOffset(0.0f, 2.0f, 6.0f);

		// Stands in for a follow camera's script (M13), as the Rolling Ball design places its camera: in every LateUpdate,
		// "Ear" moves to the rendered position of "Ball" (Transform.RenderPosition, §11.5) plus FollowOffset.
		class FollowCamera final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				if (phase != PlaySessionPhase::LateUpdate)
					return;
				Scene& scene = session.GetScene();
				const glm::vec3 ball = TransformSystem::GetRenderPosition(scene.FindEntityByPath("/Ball"));
				scene.FindEntityByPath("/Ear").Patch<TransformComponent>([&ball](TransformComponent& transform)
				{
					transform.Translation = ball + FollowOffset;
				});
			}
		};

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: a session without an audio engine has no audio and steps as before")
		{
			// The hook wired by the contract: without PlaySessionSpecification::Audio nothing of it runs.
			Test::SceneTestFixture fixture;
			PopulateAudioScene(fixture.GetScene());
			PlaySessionSpecification specification = MakeAudioSpecification(fixture, nullptr);
			specification.OwnsAudioTime = true;
			const Scope<PlaySession> session = StartSession(fixture, specification);
			CHECK(session->GetAudioSystem() == nullptr);
			CHECK_FALSE(session->IsAudioTimeOwned());
			session->SetLockstep(true);
			CHECK_FALSE(session->IsAudioTimeOwned());
			session->SetPaused(true);
			session->Tick();
			session->SetPaused(false);
			session->SetLockstep(false);
			session->FrameUpdate(SixtiethFrame);
			CHECK(session->GetTick() == 1);
		}

		TEST_CASE("PlaySession: a Play session starts its PlayOnStart sources held until its first frame phase")
		{
			Test::SceneTestFixture fixture;
			PopulateAudioScene(fixture.GetScene());
			VirtualFileSystem vfs;
			const Scope<AudioEngine> engine = CreateSessionEngine(vfs);
			{
				const Scope<PlaySession> session = StartSession(fixture, MakeAudioSpecification(fixture, engine.get()));
				AudioSystem* audio = session->GetAudioSystem();
				REQUIRE(audio != nullptr);
				const Scene& scene = session->GetScene();
				CHECK(audio->IsPlaying(scene.FindEntityByPath("/Autoplay")));
				CHECK_FALSE(audio->IsPlaying(scene.FindEntityByPath("/Manual")));
				// The voice exists at tick 0, held at its start until the first AudioUpdate phase.
				const AudioVoiceInfo held = GetOnlyVoice(*engine);
				CHECK(held.Paused);
				CHECK(held.CursorFrames == 0);
				session->Tick();
				const AudioVoiceInfo released = GetOnlyVoice(*engine);
				CHECK_FALSE(released.Paused);
				CHECK(audio->GetListener().Source == AudioListenerSource::Listener);
			}
			// Stop (§5.6): the session's voices are released with it.
			CHECK(engine->GetStats().LiveVoices == 0);
		}

		TEST_CASE("PlaySession: Simulate and engine-less sessions have no audio")
		{
			Test::SceneTestFixture fixture;
			PopulateAudioScene(fixture.GetScene());
			VirtualFileSystem vfs;
			const Scope<AudioEngine> engine = CreateSessionEngine(vfs);
			PlaySessionSpecification simulateSpecification = MakeAudioSpecification(fixture, engine.get(), PlayMode::Simulate);
			simulateSpecification.OwnsAudioTime = true;
			const Scope<PlaySession> simulate = StartSession(fixture, simulateSpecification);
			CHECK(simulate->GetAudioSystem() == nullptr);
			simulate->SetLockstep(true);
			CHECK_FALSE(simulate->IsAudioTimeOwned());
			CHECK_FALSE(engine->IsSimulationTimeOwned());
			const Scope<PlaySession> silent = StartSession(fixture, MakeAudioSpecification(fixture, nullptr));
			CHECK(silent->GetAudioSystem() == nullptr);
			CHECK(engine->GetStats().LiveVoices == 0);
		}

		TEST_CASE("PlaySession: lockstep owns the audio engine's time and every tick pulls 800 frames")
		{
			Test::SceneTestFixture fixture;
			PopulateAudioScene(fixture.GetScene());
			VirtualFileSystem vfs;
			const Scope<AudioEngine> engine = CreateSessionEngine(vfs);
			{
				const Scope<PlaySession> session = StartSession(fixture, MakeAudioSpecification(fixture, engine.get()));
				CHECK_FALSE(session->IsAudioTimeOwned());
				CHECK_FALSE(engine->IsSimulationTimeOwned());
				session->SetLockstep(true);
				CHECK(session->IsAudioTimeOwned());
				CHECK(engine->IsSimulationTimeOwned());
				CHECK(engine->GetTimeSource() == AudioTimeSource::Simulation);
				const uint64_t before = engine->GetStats().PulledFrames;
				const AudioVoiceInfo started = GetOnlyVoice(*engine);
				CHECK(started.CursorFrames == 0);
				session->Tick();
				const AudioVoiceInfo ticked = GetOnlyVoice(*engine);
				CHECK(ticked.CursorFrames == 800);
				for (int tick = 1; tick < 600; ++tick)
					session->Tick();
				CHECK(engine->GetStats().PulledFrames - before == 600 * 800);
				// The silent clip is 4,800 frames long and loops: 480,000 frames later the cursor is back at its start.
				const AudioVoiceInfo looped = GetOnlyVoice(*engine);
				CHECK(looped.CursorFrames == 0);
				session->SetLockstep(false);
				CHECK_FALSE(session->IsAudioTimeOwned());
				CHECK_FALSE(engine->IsSimulationTimeOwned());
				session->SetLockstep(true);
				CHECK(engine->IsSimulationTimeOwned());
			}
			// Destroying the session gives time back.
			CHECK_FALSE(engine->IsSimulationTimeOwned());
			CHECK(engine->GetTimeSource() == AudioTimeSource::Host);
		}

		TEST_CASE("PlaySession: a test run owns audio time and pulls one tick of frames per tick run since the last pull")
		{
			// §10.1 "or a test run is active": a ScriptedClock suite (§11.10) is not in lockstep and runs 0 to MaxStepsPerFrame
			// steps per frame.
			Test::SceneTestFixture fixture;
			PopulateAudioScene(fixture.GetScene());
			VirtualFileSystem vfs;
			const Scope<AudioEngine> engine = CreateSessionEngine(vfs);
			PlaySessionSpecification specification = MakeAudioSpecification(fixture, engine.get());
			specification.OwnsAudioTime = true;
			{
				const Scope<PlaySession> session = StartSession(fixture, specification);
				CHECK(session->IsAudioTimeOwned());
				CHECK(engine->GetTimeSource() == AudioTimeSource::Simulation);
				const uint64_t before = engine->GetStats().PulledFrames;
				for (int step = 0; step < 3; ++step)
					session->FixedStep();
				session->FrameUpdate(SixtiethFrame);
				CHECK(engine->GetStats().PulledFrames - before == 3 * 800);
				const AudioVoiceInfo voice = GetOnlyVoice(*engine);
				CHECK(voice.CursorFrames == 3 * 800);
				// A frame without a step pulls nothing.
				session->FrameUpdate(SixtiethFrame);
				CHECK(engine->GetStats().PulledFrames - before == 3 * 800);
				// Lockstep changes nothing about a test run's ownership.
				session->SetLockstep(true);
				session->SetLockstep(false);
				CHECK(session->IsAudioTimeOwned());
				session->Tick();
				CHECK(engine->GetStats().PulledFrames - before == 4 * 800);
			}
			CHECK_FALSE(engine->IsSimulationTimeOwned());
		}

		TEST_CASE("PlaySession: a paused lockstep session's voices follow its ticks and pause when it leaves lockstep")
		{
			Test::SceneTestFixture fixture;
			PopulateAudioScene(fixture.GetScene());
			VirtualFileSystem vfs;
			const Scope<AudioEngine> engine = CreateSessionEngine(vfs);
			const Scope<PlaySession> session = StartSession(fixture, MakeAudioSpecification(fixture, engine.get()));
			// play.start {lockstep: true, paused: true}: play.step is the session's only clock, so its voices advance.
			session->SetPaused(true);
			session->SetLockstep(true);
			session->Tick();
			const AudioVoiceInfo stepped = GetOnlyVoice(*engine);
			CHECK_FALSE(stepped.Paused);
			CHECK(stepped.CursorFrames == 800);
			// play.pause's order: paused (already), then out of lockstep; the voices pause before time goes back.
			session->SetPaused(true);
			session->SetLockstep(false);
			CHECK_FALSE(engine->IsSimulationTimeOwned());
			// A paused session's play.step advances only the simulation (ADR 0015 decision 5).
			session->Tick();
			const AudioVoiceInfo paused = GetOnlyVoice(*engine);
			CHECK(paused.Paused);
			CHECK(paused.CursorFrames == 800);
		}

		TEST_CASE("PlaySession: pausing the session pauses its voices")
		{
			Test::SceneTestFixture fixture;
			PopulateAudioScene(fixture.GetScene());
			VirtualFileSystem vfs;
			const Scope<AudioEngine> engine = CreateSessionEngine(vfs);
			const Scope<PlaySession> session = StartSession(fixture, MakeAudioSpecification(fixture, engine.get()));
			session->SetPaused(true);
			AudioSystem* audio = session->GetAudioSystem();
			REQUIRE(audio != nullptr);
			CHECK(audio->IsPaused());
			const AudioVoiceInfo paused = GetOnlyVoice(*engine);
			CHECK(paused.Paused);
			// Resumed before its first frame phase, the session still holds its voices until that phase.
			session->SetPaused(false);
			const AudioVoiceInfo held = GetOnlyVoice(*engine);
			CHECK(held.Paused);
			session->FrameUpdate(SixtiethFrame);
			const AudioVoiceInfo running = GetOnlyVoice(*engine);
			CHECK_FALSE(running.Paused);
			session->SetPaused(true);
			const AudioVoiceInfo pausedAgain = GetOnlyVoice(*engine);
			CHECK(pausedAgain.Paused);
			session->SetPaused(false);
			const AudioVoiceInfo resumed = GetOnlyVoice(*engine);
			CHECK_FALSE(resumed.Paused);
		}

		TEST_CASE("PlaySession: a body that physics moves is heard at its speed on every frame, at any frame rate")
		{
			// Physics moves bodies only inside the fixed steps, of which a SystemClock frame runs 0 to MaxStepsPerFrame
			// (FixedStepScheduler::Advance): at 144 Hz most frames run none and the others one, at 45 Hz each runs one or two.
			// A ball rolls at a constant 10 m/s and a follow camera, the listener, keeps a constant offset from where the ball
			// is drawn. Both are heard at their rendered poses, so both move at 10 m/s on every frame and the ball's sound
			// has no Doppler shift relative to the camera (Docs/Decisions/0016-m8-m11-m12-integration.md decision 8).
			for (const uint32_t displayHz : { 144u, 45u })
			{
				CAPTURE(displayHz);
				Test::SceneTestFixture fixture;
				Scene& edit = fixture.GetScene();
				Entity ball = Test::AddSphereBody(edit, "Ball", glm::vec3(0.0f, 10.0f, 0.0f), 0.5f, BodyType::Dynamic);
				Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
				{
					body.GravityFactor = 0.0f;
					body.LinearDamping = 0.0f;
					body.AllowSleeping = false;
					body.InitialLinearVelocity = BallVelocity;
				});
				AudioSourceComponent source;
				source.Clip.SetHandle(BuiltinAssetHandles::SilentClip);
				source.Loop = true;
				source.PlayOnStart = true;
				source.Spatial = true;
				ball.AddComponent<AudioSourceComponent>(source);
				Entity ear = edit.CreateEntity("Ear");
				ear.AddComponent<AudioListenerComponent>();

				VirtualFileSystem vfs;
				const Scope<AudioEngine> engine = CreateSessionEngine(vfs);
				FollowCamera camera;
				PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
				specification.Audio = engine.get();
				specification.Observer = &camera;
				const Scope<PlaySession> session = StartSession(fixture, specification);
				const Entity runtimeBall = session->GetScene().FindEntityByPath("/Ball");
				REQUIRE(runtimeBall.IsValid());

				// The host's loop (FrameLoop::RunFrameSteps) at displayHz. The first frames warm up: before the first step
				// nothing has a previous pose, and the first frame after it starts from the pose the session was created at.
				FixedStepScheduler scheduler(FrameLoopConfig{ .FixedHz = 60 });
				const double delta = 1.0 / static_cast<double>(displayHz);
				const uint32_t warmUpFrames = displayHz / 10;
				for (uint32_t frame = 0; frame < warmUpFrames + displayHz / 2; ++frame)
				{
					const FrameSteps steps = scheduler.Advance(delta, 1.0);
					for (uint32_t step = 0; step < steps.StepCount; ++step)
						session->FixedStep();
					session->FrameUpdate(FrameTime{ .DeltaTime = delta, .UnscaledDeltaTime = delta, .Alpha = steps.Alpha, .FrameIndex = frame });
					if (frame < warmUpFrames)
						continue;
					CAPTURE(frame);
					CAPTURE(steps.StepCount);
					const AudioVoiceInfo voice = GetOnlyVoice(*engine);
					const glm::vec3 drawn(ComputeRenderedWorldMatrix(runtimeBall, static_cast<float>(steps.Alpha))[3]);
					CHECK(Test::ApproxEqual(voice.Transform.Position, drawn));
					CHECK(Test::ApproxEqual(voice.Transform.Velocity, BallVelocity, 0.01f));
					CHECK(Test::ApproxEqual(engine->GetListener().Velocity, BallVelocity, 0.01f));
				}
			}
		}
	}

}
