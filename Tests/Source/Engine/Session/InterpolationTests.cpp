#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"

// Render interpolation through a play session (Architecture §5.2, §5.7 step 0 and frame phase; Roadmap M7 acceptance).
// The observer stands in for the scripts of M13: it moves an entity in FixedUpdate (a script's OnFixedUpdate) and writes
// one in Update (a script's OnUpdate). Skipped skeleton of the M7 contract (Docs/Decisions/0012-m7-decisions.md): stream A
// implements the session's tagging, stream B the extraction it uses.

namespace Engine {

	namespace {

		// Moves "Mover" by +1 on X in every fixed update and writes "Writer" to x = 10 * (the number of Update calls) in every
		// Update; in the fixed update of tick 1 spawns "Spawned" and teleports "Teleported" to x = 100.
		class ScriptStandIn final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				Scene& scene = session.GetScene();
				if (phase == PlaySessionPhase::FixedUpdate)
				{
					Entity mover = scene.FindEntityByPath("/Mover");
					mover.Patch<TransformComponent>([](TransformComponent& transform)
					{
						transform.Translation.x += 1.0f;
					});
					if (tick == 1)
					{
						REQUIRE(session.CreateEntity("Spawned").has_value());
						Entity teleported = scene.FindEntityByPath("/Teleported");
						teleported.Patch<TransformComponent>([](TransformComponent& transform)
						{
							transform.Translation.x = 100.0f;
						});
						session.MarkTeleported(teleported);
					}
				}
				else if (phase == PlaySessionPhase::Update)
				{
					++m_Updates;
					Entity writer = scene.FindEntityByPath("/Writer");
					writer.Patch<TransformComponent>([x = 10.0f * static_cast<float>(m_Updates)](TransformComponent& transform)
					{
						transform.Translation.x = x;
					});
				}
			}
		private:
			int m_Updates = 0;
		};

		// The x translation of `entity`'s extracted world matrix at `alpha`.
		float ExtractedX(Scene& scene, std::string_view path, float alpha)
		{
			const Entity entity = scene.FindEntityByPath(path);
			REQUIRE(entity.IsValid());
			return ComputeRenderedWorldMatrix(entity, alpha)[3].x;
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("Interpolation: script-moved entities interpolate; frame-phase writes, teleports and new entities do not"
			* doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& edit = fixture.GetScene();
			static_cast<void>(edit.CreateEntity("Mover"));
			Entity writer = edit.CreateEntity("Writer");
			static_cast<void>(edit.CreateEntity("WriterChild", writer));
			static_cast<void>(edit.CreateEntity("Teleported"));

			ScriptStandIn standIn;
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Observer = &standIn;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, edit);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;

			// Tick 0 with alpha 1 (a ManualClock frame): the mover is at x = 1 everywhere.
			session.Tick();
			// Tick 1 as a SystemClock frame with alpha 0.25: the mover moved 1 -> 2 inside the fixed step.
			session.FixedStep();
			const FrameTime frame{ .DeltaTime = 1.0 / 240.0, .UnscaledDeltaTime = 1.0 / 240.0, .Alpha = 0.25, .FrameIndex = 1 };
			session.FrameUpdate(frame);
			Scene& scene = session.GetScene();

			// Script-moved in the fixed phase: interpolates between the previous and the current step.
			CHECK(ExtractedX(scene, "/Mover", 0.25f) == doctest::Approx(1.25f));
			// Written in the frame phase (and its child with it): 10 after tick 0's frame, 20 now, rendered at 20, never at
			// the interpolated 12.5.
			CHECK(scene.FindEntityByPath("/Writer").HasComponent<InterpolationResetTag>());
			CHECK(ExtractedX(scene, "/Writer", 0.25f) == doctest::Approx(20.0f));
			CHECK(ExtractedX(scene, "/Writer/WriterChild", 0.25f) == doctest::Approx(20.0f));
			// Teleported in the fixed phase: renders at the current pose.
			CHECK(ExtractedX(scene, "/Teleported", 0.25f) == doctest::Approx(100.0f));
			// Created since the last snapshot: renders at the current pose.
			CHECK(ExtractedX(scene, "/Spawned", 0.25f) == doctest::Approx(0.0f));

			// The session's own extraction used the frame's alpha.
			CHECK(session.GetLastExtraction().Alpha == doctest::Approx(0.25f));

			// The next snapshot clears the tags: the teleported entity interpolates again from its new pose.
			session.FixedStep();
			CHECK_FALSE(scene.FindEntityByPath("/Teleported").HasComponent<InterpolationResetTag>());
			CHECK(ExtractedX(scene, "/Teleported", 0.5f) == doctest::Approx(100.0f));
			CHECK(ExtractedX(scene, "/Mover", 0.5f) == doctest::Approx(2.5f));
		}

		TEST_CASE("Interpolation: a write at the safe point is snapshotted current, never interpolated from its stale pose"
			* doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(fixture.GetScene().CreateEntity("Moved"));
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			session.Tick();

			// Automation writes between frames (entity.update {target: "play"}), then a frame runs a step at alpha 0.5.
			Entity moved = session.GetScene().FindEntityByPath("/Moved");
			moved.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x = 10.0f;
			});
			session.FixedStep();
			session.FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 120.0, .UnscaledDeltaTime = 1.0 / 120.0, .Alpha = 0.5, .FrameIndex = 2 });
			CHECK(ExtractedX(session.GetScene(), "/Moved", 0.5f) == doctest::Approx(10.0f));
		}

		TEST_CASE("Interpolation: a view between ticks shows a write made since the last step at its new pose" * doctest::skip(true))
		{
			// viewport.screenshot of the play scene and --screenshot-at render PlaySession::ExtractView: after a pause and
			// entity.update {target: "play"} at the safe point, the image shows the new pose, never the stale WorldTransform.
			Test::SceneTestFixture fixture;
			Entity cube = fixture.GetScene().CreateEntity("Cube");
			cube.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;

			// A SystemClock frame: the view alpha is the frame's.
			session.Tick();
			session.FixedStep();
			session.FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 240.0, .UnscaledDeltaTime = 1.0 / 240.0, .Alpha = 0.25, .FrameIndex = 1 });
			CHECK(session.GetViewAlpha() == doctest::Approx(0.25f));
			// Paused (play.pause): the view alpha is 1.
			session.SetPaused(true);
			CHECK(session.GetViewAlpha() == 1.0f);

			Entity played = session.GetScene().FindEntityByPath("/Cube");
			played.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x = 7.0f;
			});
			const uint64_t hash = session.ComputeStateHash();
			const Result<RenderSnapshot> view = session.ExtractView({ .Camera = RenderCameraSource::Explicit, .Width = 64, .Height = 64, .Alpha = 0.0f });
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			REQUIRE(view->Meshes.size() == 1);
			CHECK(view->Meshes[0].World[3].x == doctest::Approx(7.0f));
			CHECK(view->Alpha == 1.0f);
			// Bringing the render state up to date changed nothing the simulation hashes.
			CHECK(session.ComputeStateHash() == hash);
		}
	}

}
