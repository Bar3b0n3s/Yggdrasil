#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <vector>

// M21's place in the play session (ADR 0028's phase-2 execution order; Docs/Decisions/0032-m21-contract.md decisions 5 and
// 10). The phase names are live; the phases' order is skipped until streams B and P2 enter them (they then add the new phases
// to PlaySessionTests.cpp's lists, at these places).

namespace Engine {

	namespace {

		class Recording2DPhases final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& /*session*/, PlaySessionPhase phase, uint64_t /*tick*/) override { Phases.push_back(phase); }

			std::vector<PlaySessionPhase> Phases{};
		};

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: M21's phases have their enumerator names")
		{
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::SpriteAnimation) == "SpriteAnimation");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::Physics2DPreStep) == "Physics2DPreStep");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::Physics2DStep) == "Physics2DStep");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::Physics2DPostStep) == "Physics2DPostStep");
			// The ledger's order: SpriteAnimation after NativeStep, the 2D phases after the 3D post-step.
			CHECK(static_cast<int>(PlaySessionPhase::SpriteAnimation) == static_cast<int>(PlaySessionPhase::NativeStep) + 1);
			CHECK(static_cast<int>(PlaySessionPhase::Physics2DPreStep) == static_cast<int>(PlaySessionPhase::PhysicsPostStep) + 1);
			CHECK(static_cast<int>(PlaySessionPhase::DestroyFlush) == static_cast<int>(PlaySessionPhase::Physics2DPostStep) + 1);
		}

		TEST_CASE("PlaySession: SpriteAnimation runs before the transform update and the 2D phases after the 3D post-step, in Simulate too" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Recording2DPhases observer;
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = 42;
			specification.Observer = &observer;
			specification.Mode = PlayMode::Simulate;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			(*session)->Tick();
			const std::vector<PlaySessionPhase> expected = {
				PlaySessionPhase::InterpolationSnapshot,
				PlaySessionPhase::ApplyInput,
				PlaySessionPhase::SpriteAnimation,
				PlaySessionPhase::PreStepTransformUpdate,
				PlaySessionPhase::PhysicsPreStep,
				PlaySessionPhase::PhysicsStep,
				PlaySessionPhase::PhysicsPostStep,
				PlaySessionPhase::Physics2DPreStep,
				PlaySessionPhase::Physics2DStep,
				PlaySessionPhase::Physics2DPostStep,
				PlaySessionPhase::DestroyFlush,
				PlaySessionPhase::PostStepTransformUpdate,
				PlaySessionPhase::LatchFrame,
				PlaySessionPhase::FrameDestroyFlush,
				PlaySessionPhase::FrameTransformUpdate,
				PlaySessionPhase::RenderExtraction,
			};
			CHECK(observer.Phases == expected);
			CHECK((*session)->GetPhysics2D() != nullptr);
		}
	}

}
