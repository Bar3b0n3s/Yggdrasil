#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Time.h"
#include "Engine/Scene/Components/MaterialParametersComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <cmath>

// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 3): the play session gives render extraction the time its view
// renders at, the time between its last two simulated states that the frame's alpha interpolates, so a MaterialParameters'
// scroll moves smoothly with the rendered poses, by frame and not by tick, and stands still while nothing ticks; edit-time
// requests leave it 0. The scroll is presentation: the state hash never sees it.

namespace Engine {

	namespace {

		// A session at 64 Hz over a scene holding one plane whose texture scrolls one unit every 8 s along U.
		struct ScrollingSession
		{
			Test::SceneTestFixture Fixture{};
			Scope<PlaySession> Session{};

			ScrollingSession()
			{
				Entity lava = Fixture.GetScene().CreateEntity("Lava");
				MeshRendererComponent renderer;
				renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::PlaneMesh);
				lava.AddComponent<MeshRendererComponent>(renderer);
				lava.AddComponent<MaterialParametersComponent>(MaterialParametersComponent{ .UVScroll = glm::vec2(0.125f, 0.0f) });
				PlaySessionSpecification specification;
				specification.Registry = &Fixture.GetRegistry();
				specification.Seed = 7;
				specification.Project.Simulation.FixedHz = 64;
				Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, Fixture.GetScene());
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				Session = std::move(*created);
			}

			[[nodiscard]] float GetScroll() const
			{
				const RenderSnapshot& snapshot = Session->GetLastExtraction();
				REQUIRE(snapshot.Meshes.size() == 1);
				return snapshot.Meshes[0].Parameters.UVOffset.x;
			}
		};

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: its views render a scroll at the time between its last two states that the frame's alpha picks")
		{
			ScrollingSession scrolling;
			PlaySession& session = *scrolling.Session;
			// 32 lockstep ticks: half a second, a sixteenth of the scroll's period.
			for (int tick = 0; tick < 32; ++tick)
				session.Tick();
			CHECK(scrolling.GetScroll() == 0.0625f);
			const uint64_t hash = session.ComputeStateHash();

			// A frame without a tick at alpha one half renders halfway into the last tick: 31.5 ticks of 1/64 s.
			session.FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 128.0, .UnscaledDeltaTime = 1.0 / 128.0, .Alpha = 0.5, .FrameIndex = 0 });
			CHECK(scrolling.GetScroll() == static_cast<float>(31.5 / 64.0 * 0.125));
			// The views extracted between ticks take the same time as the frame's own view.
			RenderExtractionRequest request;
			request.Camera = RenderCameraSource::Explicit;
			const Result<RenderSnapshot> view = session.ExtractView(request);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			REQUIRE(view->Meshes.size() == 1);
			CHECK(view->Meshes[0].Parameters.UVOffset.x == scrolling.GetScroll());
			// Presentation only: the frames moved the scroll, not the state.
			CHECK(session.ComputeStateHash() == hash);

			// 512 more ticks (8 s, one whole period) bring the scroll back to where it was.
			for (int tick = 0; tick < 512; ++tick)
				session.Tick();
			CHECK(std::abs(scrolling.GetScroll() - 0.0625f) < 1e-6f);
		}

		// The review of W31.1 (finding 6): a paused session runs no frame and its views take alpha 1, so they render the scroll
		// at its last tick, the same value every time.
		TEST_CASE("PlaySession: a paused session's views render a scroll at its last tick, every time")
		{
			ScrollingSession scrolling;
			PlaySession& session = *scrolling.Session;
			for (int tick = 0; tick < 32; ++tick)
				session.Tick();
			session.FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 128.0, .UnscaledDeltaTime = 1.0 / 128.0, .Alpha = 0.5, .FrameIndex = 0 });
			REQUIRE(scrolling.GetScroll() == static_cast<float>(31.5 / 64.0 * 0.125));
			session.SetPaused(true);
			const auto viewScroll = [&session]()
			{
				const Result<RenderSnapshot> view = session.ExtractView(RenderExtractionRequest{ .Camera = RenderCameraSource::Explicit });
				REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
				REQUIRE(view->Meshes.size() == 1);
				return view->Meshes[0].Parameters.UVOffset.x;
			};
			const float paused = viewScroll();
			CHECK(paused == 0.0625f);
			CHECK(viewScroll() == paused);
			CHECK(viewScroll() == paused);
		}

		// The review of W31.1 (finding 6): at a time scale of 0 a live host runs frames but no tick and holds the alpha, so every
		// frame renders the scroll where it stood. The frames are driven as a live host drives them, through a
		// FixedStepScheduler advanced at the session's time scale.
		TEST_CASE("PlaySession: at a time scale of 0 its frames render a scroll standing still")
		{
			ScrollingSession scrolling;
			PlaySession& session = *scrolling.Session;
			FixedStepScheduler scheduler(FrameLoopConfig{ .FixedHz = 64 });
			uint64_t frameIndex = 0;
			const auto frame = [&session, &scheduler, &frameIndex]()
			{
				constexpr double Delta = 1.0 / 100.0;
				const FrameSteps steps = scheduler.Advance(Delta, session.GetTimeScale());
				for (uint32_t step = 0; step < steps.StepCount; ++step)
					session.FixedStep();
				session.FrameUpdate(FrameTime{ .DeltaTime = Delta * session.GetTimeScale(), .UnscaledDeltaTime = Delta, .Alpha = steps.Alpha, .FrameIndex = frameIndex++ });
			};
			for (int count = 0; count < 20; ++count)
				frame();
			REQUIRE(session.GetTick() > 0);
			REQUIRE(session.SetTimeScale(0.0).has_value());
			frame();
			const uint64_t tick = session.GetTick();
			const float held = scrolling.GetScroll();
			for (int count = 0; count < 10; ++count)
			{
				frame();
				CHECK(scrolling.GetScroll() == held);
			}
			CHECK(session.GetTick() == tick);
		}

		TEST_CASE("PlaySession: its first view renders a scroll at time 0")
		{
			ScrollingSession scrolling;
			const Result<RenderSnapshot> view = scrolling.Session->ExtractView(RenderExtractionRequest{ .Camera = RenderCameraSource::Explicit });
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			REQUIRE(view->Meshes.size() == 1);
			CHECK(view->Meshes[0].Parameters.UVOffset.x == 0.0f);
		}
	}

}
