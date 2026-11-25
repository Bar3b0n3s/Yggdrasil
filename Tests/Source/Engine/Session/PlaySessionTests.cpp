#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/PixelPerfectCameraComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/StaticComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scene/UiSystem.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"
#include "Support/ScriptTestFixture.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// PlaySession (Architecture §5.6, §5.7; Roadmap M7 acceptance; Docs/Decisions/0012-m7-decisions.md decisions 2 and 3):
// the serializer copy, the documented step order, the entity cap, the state hash and the session's seeded ids.

namespace Engine {

	namespace {

		// Records every phase the session reports (IPlaySessionObserver) and, when asked, moves an entity in the phases where
		// scripts will act (M13).
		class RecordingObserver final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& /*session*/, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				Phases.push_back(phase);
			}

			std::vector<PlaySessionPhase> Phases;
		};

		// Records, in every fixed update, whether the step view saw Space pressed and released.
		class InputProbe final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				if (phase != PlaySessionPhase::FixedUpdate)
					return;
				const InputState& devices = session.GetInput().GetDevices();
				Pressed.push_back(devices.WasKeyPressed(InputPhase::Step, Key::Space));
				Released.push_back(devices.WasKeyReleased(InputPhase::Step, Key::Space));
			}

			std::vector<bool> Pressed;
			std::vector<bool> Released;
		};

		// M22 C22.3: takes the UI events of every tick at StartFlush, right after the UiInput phase raised them.
		class UiEventTaker final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				if (phase != PlaySessionPhase::StartFlush)
					return;
				for (const UiEvent& event : session.GetUi().TakeEvents())
					Events.push_back(event);
			}

			std::vector<UiEvent> Events;
		};

		// Destroys "/Board" in the first fixed update (a script's Destroy, M13) and counts the entities marked for destruction
		// when the destroy flush starts and when the post-step transform update starts.
		class DestroyingObserver final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				Scene& scene = session.GetScene();
				if (phase == PlaySessionPhase::FixedUpdate && tick == 0)
					scene.DestroyEntity(scene.FindEntityByPath("/Board"));
				else if (phase == PlaySessionPhase::DestroyFlush)
					PendingAtFlush = CountPending(scene);
				else if (phase == PlaySessionPhase::PostStepTransformUpdate)
					PendingAfterFlush = CountPending(scene);
			}

			size_t PendingAtFlush = 0;
			size_t PendingAfterFlush = 0;
		private:
			static size_t CountPending(Scene& scene)
			{
				const auto pending = scene.GetRegistry().view<PendingDestroyTag>();
				return static_cast<size_t>(std::distance(pending.begin(), pending.end()));
			}
		};

		// An application host whose script environment reports its own window, as the editor's borrowed window does.
		class WindowHost final : public IPlaySessionHost
		{
		public:
			[[nodiscard]] ScriptEnvironment GetScriptEnvironment() const override { return Environment; }
			[[nodiscard]] Status SetScriptCursorMode(CursorMode /*mode*/) override { return MakeError(ErrorCode::Unsupported, "the test host has no cursor"); }
			[[nodiscard]] CursorMode GetScriptCursorMode() const override { return CursorMode::Normal; }
			void OnScriptError(const ScriptError& /*error*/, bool /*fatal*/) override {}
			void OnScriptPhase(std::string_view /*script*/, std::string_view /*callback*/) override {}

			ScriptEnvironment Environment{ .IsEditor = true, .IsHeadless = false, .IsFocused = true, .Platform = "Test", .Version = "1" };
		};

		// The specification of a session over the fixture's registry.
		PlaySessionSpecification MakeSpecification(Test::SceneTestFixture& fixture, IPlaySessionObserver* observer = nullptr)
		{
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = 42;
			specification.Observer = observer;
			return specification;
		}

		// M22 C22.2: a canvas holding the buttons "A" at (0, 0) and "B" at (0, 30), each 100 x 20 scene pixels with an image.
		void PopulateUiScene(Scene& scene)
		{
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			for (const auto& [name, y] : { std::pair<std::string_view, float>{ "A", 0.0f }, std::pair<std::string_view, float>{ "B", 30.0f } })
			{
				Entity button = scene.CreateEntity(name, canvas);
				button.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
					.AnchorMax = glm::vec2(0.0f),
					.Pivot = glm::vec2(0.0f),
					.Position = glm::vec2(0.0f, y),
					.Size = glm::vec2(100.0f, 20.0f) });
				button.AddComponent<UiImageComponent>(UiImageComponent{});
				button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			}
		}

		// A tap of `key` for `tick`, as a host or play.step queues it.
		void QueueKeyTap(PlaySession& session, uint64_t tick, Key key)
		{
			REQUIRE(session.GetInput().Queue(tick, PlayInputEvent{ .Type = PlayInputEventType::KeyInput, .State = PlayInputEventState::Tap, .KeyCode = key }).has_value());
		}

		void QueuePointer(PlaySession& session, uint64_t tick, glm::vec2 position)
		{
			REQUIRE(session.GetInput().Queue(tick, PlayInputEvent{ .Type = PlayInputEventType::MouseMove, .Position = position }).has_value());
		}

		// A left click at `position` (view coordinates): pressed at `tick`, released at the next.
		void QueueClick(PlaySession& session, uint64_t tick, glm::vec2 position)
		{
			const PlayInputEvent click{ .Type = PlayInputEventType::MouseButtonInput,
				.State = PlayInputEventState::Tap,
				.Button = MouseButton::Left,
				.HasPosition = true,
				.Position = position };
			REQUIRE(session.GetInput().Queue(tick, click).has_value());
		}

		// M22's milestone review: a World canvas of `pixels` x `pixels` canvas pixels at 4 per world unit, centred on its entity
		// at `position` and facing +Z, whose element "Button" (a UiButton with an image) covers it.
		Entity AddWorldButtonCanvas(Scene& scene, std::string_view name, glm::vec3 position, uint32_t pixels)
		{
			Entity canvas = scene.CreateEntity(name);
			canvas.Patch<TransformComponent>([position](TransformComponent& transform)
			{
				transform.Translation = position;
			});
			canvas.AddComponent<UiCanvasComponent>(
				UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = pixels, .WorldHeight = pixels, .PixelsPerUnit = 4.0f });
			Entity button = scene.CreateEntity("Button", canvas);
			button.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(1.0f), .Size = glm::vec2(0.0f) });
			button.AddComponent<UiImageComponent>(UiImageComponent{});
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			return canvas;
		}

		// The Click events of `events`, by target.
		std::vector<UUID> GetClicks(const std::vector<UiEvent>& events)
		{
			std::vector<UUID> clicks;
			for (const UiEvent& event : events)
			{
				if (event.Kind == UiEventKind::Click)
					clicks.push_back(event.Target);
			}
			return clicks;
		}

		// A small edit scene: a root with a child and a moved root.
		void PopulateScene(Scene& scene)
		{
			Entity board = scene.CreateEntity("Board");
			static_cast<void>(scene.CreateEntity("Cell", board));
			Entity ball = scene.CreateEntity("Ball");
			ball.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: game views retain each session's project rendering quality")
		{
			Test::SceneTestFixture fixture;
			SUBCASE("a primary camera")
			{
				fixture.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			}
			SUBCASE("a camera-less clear")
			{
			}
			PlaySessionSpecification first = MakeSpecification(fixture);
			first.Project.Rendering.ShadowMapSize = 512;
			first.Project.Rendering.SsaoHalfResolution = true;
			PlaySessionSpecification second = MakeSpecification(fixture);
			second.Project.Rendering.ShadowMapSize = 4096;
			Result<Scope<PlaySession>> firstSession = PlaySession::CreateFromScene(first, fixture.GetScene());
			Result<Scope<PlaySession>> secondSession = PlaySession::CreateFromScene(second, fixture.GetScene());
			REQUIRE(firstSession.has_value());
			REQUIRE(secondSession.has_value());
			// The session owns its settings; neither the caller nor an interleaved second view can replace them.
			first.Project.Rendering = {};
			(*firstSession)->Tick();
			(*secondSession)->Tick();
			(*firstSession)->SetViewSize(173, 91);
			(*firstSession)->Tick();
			CHECK((*firstSession)->GetLastExtraction().HasCamera == FindPrimaryCamera(fixture.GetScene()).IsValid());
			CHECK((*firstSession)->GetLastExtraction().Quality.ShadowMapSize == 512);
			CHECK((*firstSession)->GetLastExtraction().Quality.SsaoHalfResolution);
			CHECK((*secondSession)->GetLastExtraction().Quality.ShadowMapSize == 4096);
			CHECK_FALSE((*secondSession)->GetLastExtraction().Quality.SsaoHalfResolution);
		}

		TEST_CASE("PlaySession: an unchanged static scene visits no entity in a tick")
		{
			// M43: a static island of 40 pieces, each with a static collider, and 4 rocks whose colliders are meshes (bodies
			// with an asset input, S16's scenery: three the built-in cube, one a published copy of it), a camera and a sun: once
			// its first tick has placed everything, a tick in which nothing moves recomputes no matrix and visits no entity.
			Test::InMemoryAssetManager assets;
			const AssetHandle boulderMesh(0x424f554c44455201ull);
			const Result<AssetRef<Asset>> cube = assets.Load(BuiltinAssetHandles::CubeMesh);
			const Result<AssetRef<Asset>> plane = assets.Load(BuiltinAssetHandles::PlaneMesh);
			REQUIRE(cube.has_value());
			REQUIRE(plane.has_value());
			assets.Publish(boulderMesh, *cube);
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity island = scene.CreateEntity("Island");
			island.AddComponent<StaticComponent>();
			for (int index = 0; index < 40; ++index)
			{
				Entity piece = scene.CreateEntity("Piece", island);
				piece.Patch<TransformComponent>([index](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(2.0f * static_cast<float>(index % 8), 0.0f, 2.0f * static_cast<float>(index / 8));
				});
				piece.AddComponent<BoxColliderComponent>();
			}
			for (int index = 0; index < 4; ++index)
			{
				Entity rock = scene.CreateEntity("Rock", island);
				rock.Patch<TransformComponent>([index](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(4.0f * static_cast<float>(index), 0.0f, 20.0f);
				});
				const AssetHandle mesh = index == 3 ? boulderMesh : BuiltinAssetHandles::CubeMesh;
				rock.AddComponent<MeshColliderComponent>(MeshColliderComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(mesh) });
			}
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Entity sun = scene.CreateEntity("Sun");
			sun.AddComponent<DirectionalLightComponent>();
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.Assets = &assets;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			CHECK(session.GetLastTickWork().TransformsUpdated == 0);
			CHECK(session.GetLastTickWork().EntitiesVisited == 0);
			for (int tick = 0; tick < 3; ++tick)
				session.Tick();
			CHECK(session.GetLastTickWork().TransformsUpdated == 0);
			CHECK(session.GetLastTickWork().EntitiesVisited == 0);
			const UUID boulder = session.GetScene().FindEntityByPath("/Island/Rock[3]").GetUUID();
			const std::optional<Aabb> cubeBounds = session.GetPhysics().GetBodyBounds(boulder);
			REQUIRE(cubeBounds.has_value());
			CHECK(cubeBounds->GetSize().y == doctest::Approx(1.0f).epsilon(1.0e-3));

			// A new version of the boulder's mesh (a hot reload: the flat plane) moves the asset manager's version revision:
			// the next tick compares the rocks' mesh versions once and reshapes the boulder. The asset check is not counted
			// (asset events are not simulation input), so the tick still visits no entity; then nothing again.
			assets.Publish(boulderMesh, *plane);
			session.Tick();
			CHECK(session.GetLastTickWork().EntitiesVisited == 0);
			const std::optional<Aabb> planeBounds = session.GetPhysics().GetBodyBounds(boulder);
			REQUIRE(planeBounds.has_value());
			CHECK(planeBounds->GetSize().y < 0.1f);
			session.Tick();
			CHECK(session.GetLastTickWork().EntitiesVisited == 0);

			// A write to one piece (as a script's in the frame phase): its matrix recomputed, its body placed again (its shape
			// check and its pre-step visit); then nothing again.
			Entity piece = session.GetScene().FindEntityByPath("/Island/Piece[5]");
			piece.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 3.0f;
			});
			session.Tick();
			CHECK(session.GetLastTickWork().TransformsUpdated == 1);
			CHECK(session.GetLastTickWork().EntitiesVisited == 2);
			const std::optional<PhysicsBodyReport> body = session.GetPhysics().GetBodyInfo(piece.GetUUID());
			REQUIRE(body.has_value());
			CHECK(body->Pose.Position.y == doctest::Approx(3.0f));
			session.Tick();
			CHECK(session.GetLastTickWork().TransformsUpdated == 0);
			CHECK(session.GetLastTickWork().EntitiesVisited == 0);
		}

		TEST_CASE("PlaySession: an entity written to the same bits in the frame phase is not tagged")
		{
			// M43 keeps §5.2's semantics over the changed list: a write outside the fixed steps that changes the world matrix
			// renders without interpolation (InterpolationResetTag), and one that leaves its bits as they were does not.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity mover = scene.CreateEntity("Mover");
			mover.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
			static_cast<void>(scene.CreateEntity("Bystander"));
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture), scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			session.Tick();
			session.Tick();
			Entity runtime = session.GetScene().FindEntityByPath("/Mover");
			const Entity bystander = session.GetScene().FindEntityByPath("/Bystander");

			session.FixedStep();
			runtime.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
			session.FrameUpdate(FrameTime{ .DeltaTime = session.GetFixedDelta(), .Alpha = 0.5 });
			CHECK_FALSE(runtime.HasComponent<InterpolationResetTag>());
			CHECK_FALSE(bystander.HasComponent<InterpolationResetTag>());

			session.FixedStep();
			runtime.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(4.0f, 2.0f, 3.0f);
			});
			session.FrameUpdate(FrameTime{ .DeltaTime = session.GetFixedDelta(), .Alpha = 0.5 });
			CHECK(runtime.HasComponent<InterpolationResetTag>());
			CHECK_FALSE(bystander.HasComponent<InterpolationResetTag>());
			CHECK(TransformSystem::GetRenderPosition(runtime) == glm::vec3(4.0f, 2.0f, 3.0f));

			// The next step's snapshot clears the tag, and the write is the previous pose of the step after.
			session.FixedStep();
			CHECK_FALSE(runtime.HasComponent<InterpolationResetTag>());
			REQUIRE(runtime.HasComponent<PreviousWorldTransformComponent>());
			CHECK(runtime.GetComponent<PreviousWorldTransformComponent>().Matrix[3].x == 4.0f);
		}

		TEST_CASE("PlaySession: explicit capture quality does not replace game-view quality or advance simulation")
		{
			Test::SceneTestFixture fixture;
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.Project.Rendering.ShadowMapSize = 512;
			specification.Project.Rendering.SsaoHalfResolution = true;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(created.has_value());
			PlaySession& session = **created;
			session.Tick();
			session.SetPaused(true);
			const uint64_t hash = session.ComputeStateHash();
			const uint64_t tick = session.GetTick();
			const uint64_t extractions = session.GetExtractionCount();
			RenderExtractionRequest request;
			request.Camera = RenderCameraSource::Explicit;
			request.Width = 73;
			request.Height = 51;
			request.Quality = { .ShadowMapSize = 1024, .SsaoHalfResolution = false };
			const Result<RenderSnapshot> capture = session.ExtractView(request);
			REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
			CHECK(capture->Quality.ShadowMapSize == 1024);
			CHECK_FALSE(capture->Quality.SsaoHalfResolution);
			CHECK(capture->Camera.ViewportWidth == 73);
			CHECK(capture->Camera.ViewportHeight == 51);
			CHECK(session.GetLastExtraction().Quality.ShadowMapSize == 512);
			CHECK(session.GetLastExtraction().Quality.SsaoHalfResolution);
			CHECK(session.ComputeStateHash() == hash);
			CHECK(session.GetTick() == tick);
			CHECK(session.GetExtractionCount() == extractions);
		}

		TEST_CASE("PlaySession: play then stop leaves the edit scene byte-identical")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			const Result<std::string> before = SceneSerializer::SaveToString(fixture.GetScene());
			REQUIRE(before.has_value());
			const uint64_t revisionBefore = fixture.GetScene().GetRevision();
			{
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				// The session changes its own scene: none of it reaches the edit scene.
				Entity ball = (*session)->GetScene().FindEntityByPath("/Ball");
				REQUIRE(ball.IsValid());
				ball.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(9.0f);
				});
				REQUIRE((*session)->CreateEntity("Spawned").has_value());
				for (int tick = 0; tick < 10; ++tick)
					(*session)->Tick();
				CHECK((*session)->GetTick() == 10);
			}
			const Result<std::string> after = SceneSerializer::SaveToString(fixture.GetScene());
			REQUIRE(after.has_value());
			CHECK(*after == *before);
			CHECK(fixture.GetScene().GetRevision() == revisionBefore);
		}

		TEST_CASE("PlaySession: step order matches the documented sequence")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			RecordingObserver observer;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture, &observer), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());

			(*session)->Tick();
			// §5.7: the fixed step (0 to 10, TextLatch and UiInput after ApplyInput, UiEvents after StartFlush, M21's
			// SpriteAnimation 4b and M23's AnimationSystem 4c after the gameplay code, TransformSystem::Update before
			// PhysicsSystem::PreStep, M21's 2D phases 7a to 7c after the 3D post-step, UiLayout last), then the frame phase
			// (no start flush since ADR 0038 decision 14, FrameUiLayout after FrameTransformUpdate, M26's Particles right before
			// RenderExtraction).
			const std::vector<PlaySessionPhase> expected = {
				PlaySessionPhase::InterpolationSnapshot,
				PlaySessionPhase::ApplyInput,
				PlaySessionPhase::TextLatch,
				PlaySessionPhase::UiInput,
				PlaySessionPhase::StartFlush,
				PlaySessionPhase::UiEvents,
				PlaySessionPhase::FixedUpdate,
				PlaySessionPhase::Tasks,
				PlaySessionPhase::SpriteAnimation,
				PlaySessionPhase::AnimationSystem,
				PlaySessionPhase::PreStepTransformUpdate,
				PlaySessionPhase::PhysicsPreStep,
				PlaySessionPhase::PhysicsStep,
				PlaySessionPhase::PhysicsPostStep,
				PlaySessionPhase::Physics2DPreStep,
				PlaySessionPhase::Physics2DStep,
				PlaySessionPhase::Physics2DPostStep,
				PlaySessionPhase::DestroyFlush,
				PlaySessionPhase::PostStepTransformUpdate,
				PlaySessionPhase::UiLayout,
				PlaySessionPhase::LatchFrame,
				PlaySessionPhase::Update,
				PlaySessionPhase::LateUpdate,
				PlaySessionPhase::FrameDestroyFlush,
				PlaySessionPhase::FrameTransformUpdate,
				PlaySessionPhase::FrameUiLayout,
				PlaySessionPhase::AudioUpdate,
				PlaySessionPhase::Particles,
				PlaySessionPhase::RenderExtraction,
			};
			CHECK(observer.Phases == expected);

			// FixedStep alone runs the fixed step only; FrameUpdate alone the frame phase only.
			observer.Phases.clear();
			(*session)->FixedStep();
			CHECK(observer.Phases == std::vector<PlaySessionPhase>(expected.begin(), expected.begin() + 20));
			observer.Phases.clear();
			(*session)->FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 0.5, .FrameIndex = 0 });
			CHECK(observer.Phases == std::vector<PlaySessionPhase>(expected.begin() + 20, expected.end()));
		}

		TEST_CASE("PlaySession: Simulate mode skips the script and audio phases")
		{
			Test::SceneTestFixture fixture;
			RecordingObserver observer;
			PlaySessionSpecification specification = MakeSpecification(fixture, &observer);
			specification.Mode = PlayMode::Simulate;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			(*session)->Tick();
			const std::vector<PlaySessionPhase> expected = {
				PlaySessionPhase::InterpolationSnapshot,
				PlaySessionPhase::ApplyInput,
				PlaySessionPhase::TextLatch,
				PlaySessionPhase::UiInput,
				PlaySessionPhase::SpriteAnimation,
				PlaySessionPhase::AnimationSystem,
				PlaySessionPhase::PreStepTransformUpdate,
				PlaySessionPhase::PhysicsPreStep,
				PlaySessionPhase::PhysicsStep,
				PlaySessionPhase::PhysicsPostStep,
				PlaySessionPhase::Physics2DPreStep,
				PlaySessionPhase::Physics2DStep,
				PlaySessionPhase::Physics2DPostStep,
				PlaySessionPhase::DestroyFlush,
				PlaySessionPhase::PostStepTransformUpdate,
				PlaySessionPhase::UiLayout,
				PlaySessionPhase::LatchFrame,
				PlaySessionPhase::FrameDestroyFlush,
				PlaySessionPhase::FrameTransformUpdate,
				PlaySessionPhase::FrameUiLayout,
				PlaySessionPhase::Particles,
				PlaySessionPhase::RenderExtraction,
			};
			CHECK(observer.Phases == expected);
		}

		// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 3; stream A, which also adds the two phases to the documented
		// sequence above): ADR 0028's phase-2 ledger fixes their positions. The step's layout is at the session's view size.
		TEST_CASE("PlaySession: UiLayout ends the fixed step and FrameUiLayout follows FrameTransformUpdate, in Play and Simulate")
		{
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				CAPTURE(mode == PlayMode::Play);
				Test::SceneTestFixture fixture;
				Entity canvas = fixture.GetScene().CreateEntity("Canvas");
				canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
				Entity title = fixture.GetScene().CreateEntity("Title", canvas);
				title.AddComponent<UiRectComponent>(UiRectComponent{});
				RecordingObserver observer;
				PlaySessionSpecification specification = MakeSpecification(fixture, &observer);
				specification.Mode = mode;
				specification.ViewWidth = 320;
				specification.ViewHeight = 180;
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				(*session)->Tick();
				const auto postStep = std::find(observer.Phases.begin(), observer.Phases.end(), PlaySessionPhase::PostStepTransformUpdate);
				REQUIRE(postStep != observer.Phases.end());
				REQUIRE(std::next(postStep) != observer.Phases.end());
				CHECK(*std::next(postStep) == PlaySessionPhase::UiLayout);
				CHECK(*std::next(postStep, 2) == PlaySessionPhase::LatchFrame);
				const auto frameTransforms = std::find(observer.Phases.begin(), observer.Phases.end(), PlaySessionPhase::FrameTransformUpdate);
				REQUIRE(frameTransforms != observer.Phases.end());
				REQUIRE(std::next(frameTransforms) != observer.Phases.end());
				CHECK(*std::next(frameTransforms) == PlaySessionPhase::FrameUiLayout);
				const UiSystem& ui = (*session)->GetUi();
				CHECK(ui.IsFrameLayoutCurrent());
				CHECK(ui.GetStepLayout().SceneSize == glm::uvec2(320, 180));
				CHECK(ui.GetLayout().Nodes.size() == 1);
			}
		}

		// M22 (C22.1, decision 3; stream A; the C22.1 review): the current layout describes the current scene from the
		// session's start, and right after a Scene.Load replaces the scene at the end of a frame, before the next step, so
		// ui.tree and W22.2's UiInput never read an empty table or a replaced scene's rectangles.
		TEST_CASE("PlaySession: the current UI layout is the scene's from the start and the loaded scene's right after a Scene.Load")
		{
			Test::ScriptTestFixture fixture;
			const UiRectComponent topLeft{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Size = glm::vec2(10.0f) };
			Entity menu = fixture.GetScene().CreateEntity("Menu");
			menu.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			fixture.GetScene().CreateEntity("Start", menu).AddComponent<UiRectComponent>(topLeft);
			// The loaded scene's element is elsewhere, so its rectangle tells the two layouts apart whatever the ids.
			Test::SceneTestFixture levelFixture;
			Entity hud = levelFixture.GetScene().CreateEntity("Hud");
			hud.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			UiRectComponent scoreRect = topLeft;
			scoreRect.Position = glm::vec2(20.0f, 30.0f);
			levelFixture.GetScene().CreateEntity("Score", hud).AddComponent<UiRectComponent>(scoreRect);
			Result<Json> levelDocument = SceneSerializer::ToJson(levelFixture.GetScene());
			REQUIRE_MESSAGE(levelDocument.has_value(), levelDocument.error().ToString());
			auto level = CreateRef<SceneData>();
			level->Document = CreateRef<const Json>(std::move(*levelDocument));
			fixture.GetAssetManager().Publish(AssetHandle(0x7100a1), level, "Assets/Scenes/Level.scene");

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 47;
			specification.ViewWidth = 320;
			specification.ViewHeight = 180;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			session.SetExtractionEnabled(false);

			// Laid out when the session starts, before its first step.
			const UiLayoutTable& started = session.GetUi().GetLayout();
			CHECK(started.SceneSize == glm::uvec2(320, 180));
			REQUIRE(started.Nodes.size() == 1);
			CHECK(started.Nodes[0].Entity == session.GetScene().FindEntityByPath("/Menu/Start").GetUUID());
			CHECK(started.Nodes[0].Min == glm::vec2(0.0f));

			// Scene.Load replaces the scene at the end of the frame, which lays the new scene out before the next step.
			const auto requested = session.GetScripts()->Evaluate("Scene.Load(Assets.Load(\"Assets/Scenes/Level.scene\"))\nreturn true", {});
			REQUIRE_MESSAGE(requested.has_value(), requested.error().ToString());
			session.FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 1.0, .FrameIndex = 0 });
			REQUIRE(session.GetSceneGeneration() == 2);
			CHECK_FALSE(session.GetUi().IsFrameLayoutCurrent());
			const UiLayoutTable& loaded = session.GetUi().GetLayout();
			REQUIRE(loaded.Nodes.size() == 1);
			CHECK(loaded.Nodes[0].Entity == session.GetScene().FindEntityByPath("/Hud/Score").GetUUID());
			CHECK(loaded.Nodes[0].Min == glm::vec2(20.0f, 30.0f));
			REQUIRE(loaded.Canvases.size() == 1);
			CHECK(loaded.Canvases[0].Canvas == session.GetScene().FindEntityByPath("/Hud").GetUUID());
		}

		// M22 (C22.2, Docs/Decisions/0033-m22-contract.md decision 24; stream A): a scene replacement drops the replaced scene's
		// focus, hover and queued UI events (UiSystem::ResetInteraction), so the loaded scene starts without a ui block and no
		// event of the old scene reaches the new one's callbacks.
		TEST_CASE("PlaySession: a Scene.Load drops the replaced scene's UI focus, hover and queued events")
		{
			Test::ScriptTestFixture fixture;
			PopulateUiScene(fixture.GetScene());
			Test::SceneTestFixture levelFixture;
			static_cast<void>(levelFixture.GetScene().CreateEntity("Level"));
			Result<Json> levelDocument = SceneSerializer::ToJson(levelFixture.GetScene());
			REQUIRE_MESSAGE(levelDocument.has_value(), levelDocument.error().ToString());
			auto level = CreateRef<SceneData>();
			level->Document = CreateRef<const Json>(std::move(*levelDocument));
			fixture.GetAssetManager().Publish(AssetHandle(0x7100a2), level, "Assets/Scenes/Level.scene");

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 48;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			session.SetExtractionEnabled(false);

			// The pointer over A hovers it, and a script focuses B: the session holds interaction state and a queued event.
			QueuePointer(session, 0, glm::vec2(10.0f));
			session.Tick();
			REQUIRE(session.GetUi().GetInteraction().Hovered == session.GetScene().FindEntityByPath("/Canvas/A").GetUUID());
			REQUIRE(session.GetUi().SetFocus(session.GetScene(), session.GetScene().FindEntityByPath("/Canvas/B").GetUUID()).has_value());
			REQUIRE(session.GetUi().GetQueuedEventCount() > 0);

			const auto requested = session.GetScripts()->Evaluate("Scene.Load(Assets.Load(\"Assets/Scenes/Level.scene\"))\nreturn true", {});
			REQUIRE_MESSAGE(requested.has_value(), requested.error().ToString());
			session.FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 1.0, .FrameIndex = 1 });
			REQUIRE(session.GetSceneGeneration() == 2);
			CHECK(session.GetUi().GetInteraction().IsEmpty());
			CHECK(session.GetUi().GetQueuedEventCount() == 0);
		}

		// C22.2 (Docs/Decisions/0033-m22-contract.md decisions 24 and 27; ADR 0028's phase-2 execution order): UiInput after
		// ApplyInput (C22.3's TextLatch goes between them) and UiEvents after StartFlush; Simulate runs UiInput, whose writes
		// are simulation, but no script phase, and drops the UI events. Streams A (UiInput) and B (UiEvents); the cases "step
		// order matches the documented sequence" and "Simulate mode skips the script and audio phases" list them too, and M23's
		// AnimationSystem after M21's SpriteAnimation on phase 2's merged tree (Docs/Decisions/0034-m23-contract.md, "Landing").
		TEST_CASE("PlaySession: UiInput follows ApplyInput and UiEvents follows StartFlush, and Simulate drops the UI events")
		{
			Test::SceneTestFixture fixture;
			PopulateUiScene(fixture.GetScene());
			RecordingObserver observer;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture, &observer), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			(*session)->FixedStep();
			CHECK(observer.Phases == std::vector<PlaySessionPhase>{ PlaySessionPhase::InterpolationSnapshot, PlaySessionPhase::ApplyInput, PlaySessionPhase::TextLatch, PlaySessionPhase::UiInput, PlaySessionPhase::StartFlush, PlaySessionPhase::UiEvents, PlaySessionPhase::FixedUpdate, PlaySessionPhase::Tasks, PlaySessionPhase::SpriteAnimation, PlaySessionPhase::AnimationSystem, PlaySessionPhase::PreStepTransformUpdate, PlaySessionPhase::PhysicsPreStep, PlaySessionPhase::PhysicsStep, PlaySessionPhase::PhysicsPostStep, PlaySessionPhase::Physics2DPreStep, PlaySessionPhase::Physics2DStep, PlaySessionPhase::Physics2DPostStep, PlaySessionPhase::DestroyFlush, PlaySessionPhase::PostStepTransformUpdate, PlaySessionPhase::UiLayout });

			RecordingObserver simulateObserver;
			PlaySessionSpecification specification = MakeSpecification(fixture, &simulateObserver);
			specification.Mode = PlayMode::Simulate;
			Result<Scope<PlaySession>> simulate = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(simulate.has_value(), simulate.error().ToString());
			QueuePointer(**simulate, 0, glm::vec2(10.0f));
			(*simulate)->FixedStep();
			CHECK(simulateObserver.Phases == std::vector<PlaySessionPhase>{ PlaySessionPhase::InterpolationSnapshot, PlaySessionPhase::ApplyInput, PlaySessionPhase::TextLatch, PlaySessionPhase::UiInput, PlaySessionPhase::SpriteAnimation, PlaySessionPhase::AnimationSystem, PlaySessionPhase::PreStepTransformUpdate, PlaySessionPhase::PhysicsPreStep, PlaySessionPhase::PhysicsStep, PlaySessionPhase::PhysicsPostStep, PlaySessionPhase::Physics2DPreStep, PlaySessionPhase::Physics2DStep, PlaySessionPhase::Physics2DPostStep, PlaySessionPhase::DestroyFlush, PlaySessionPhase::PostStepTransformUpdate, PlaySessionPhase::UiLayout });
			CHECK((*simulate)->GetUi().GetInteraction().Hovered == (*simulate)->GetScene().FindEntityByPath("/Canvas/A").GetUUID());
			CHECK((*simulate)->GetUi().GetQueuedEventCount() == 0);
		}

		// Roadmap M22's acceptance "scenes without UI keep their hash" (decision 29): hovering writes nothing to the scene, so
		// two sessions differ only by the ui block while one hovers, and hash alike again once its state is empty (the
		// UiEvents phase empties the queue every tick).
		TEST_CASE("PlaySession: the state hash holds the ui block only while the UI interaction state or event queue is not empty")
		{
			Test::SceneTestFixture fixture;
			PopulateUiScene(fixture.GetScene());
			Result<Scope<PlaySession>> hovering = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			Result<Scope<PlaySession>> away = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(hovering.has_value(), hovering.error().ToString());
			REQUIRE_MESSAGE(away.has_value(), away.error().ToString());
			CHECK((*hovering)->ComputeStateHash() == (*away)->ComputeStateHash());

			QueuePointer(**hovering, 0, glm::vec2(10.0f));
			QueuePointer(**away, 0, glm::vec2(500.0f));
			(*hovering)->Tick();
			(*away)->Tick();
			REQUIRE_FALSE((*hovering)->GetUi().GetInteraction().IsEmpty());
			CHECK((*away)->GetUi().GetInteraction().IsEmpty());
			CHECK((*hovering)->ComputeStateHash() != (*away)->ComputeStateHash());

			QueuePointer(**hovering, 1, glm::vec2(500.0f));
			(*hovering)->Tick();
			(*away)->Tick();
			CHECK((*hovering)->GetUi().GetInteraction().IsEmpty());
			CHECK((*hovering)->ComputeStateHash() == (*away)->ComputeStateHash());
		}

		// M22's landing (Docs/Decisions/0033-m22-contract.md decisions 3 and 24, "Landing"): under M21's PixelPerfectCamera the
		// game view draws the UI at the camera's reference resolution and upscales it, so the session's UI phases lay the UI out
		// at that resolution and UiInput maps the pointer's view coordinates through the same output mapping
		// (ComputeGameViewOutputMapping, the one place both take it from): a 200 x 120 reference resolution in a 640 x 360 view
		// is upscaled 3 times with 20-pixel bars at the left and right, so the view point (170, 120) shows scene pixel (50, 40),
		// which lies on button B ((0, 30) to (100, 50)).
		TEST_CASE("PlaySession: under a PixelPerfectCamera the UI is laid out at its reference resolution and the pointer maps through its upscale")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			PopulateUiScene(scene);
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			camera.AddComponent<PixelPerfectCameraComponent>(PixelPerfectCameraComponent{ .ReferenceWidth = 200, .ReferenceHeight = 120, .PixelsPerUnit = 16.0f });
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.ViewWidth = 640;
			specification.ViewHeight = 360;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			const UUID b = session.GetScene().FindEntityByPath("/Canvas/B").GetUUID();

			QueuePointer(session, 0, glm::vec2(170.0f, 120.0f));
			session.Tick();
			// The layout the session hit-tests is at the reference resolution, the one the game view drew.
			CHECK(session.GetUi().GetLayout().SceneSize == glm::uvec2(200, 120));
			const RenderSnapshot& drawn = session.GetLastExtraction();
			CHECK(drawn.Output.SceneWidth == 200);
			CHECK(drawn.Output.SceneHeight == 120);
			const auto image = std::ranges::find_if(drawn.Ui.Items, [b](const UiDrawItem& item)
			{
				return item.Entity == b && item.Kind == UiDrawKind::Image;
			});
			REQUIRE(image != drawn.Ui.Items.end());
			CHECK(image->Min == glm::vec2(0.0f, 30.0f));
			CHECK(image->Max == glm::vec2(100.0f, 50.0f));
			// The pointer at view (170, 120) is over B in the scene pixels the image shows there.
			CHECK(session.GetUi().GetInteraction().Hovered == b);
			CHECK(session.GetUi().GetInteraction().PointerOverUi);
			// A view point in the left letterbox bar lies outside the scene image, over nothing.
			QueuePointer(session, 1, glm::vec2(10.0f, 100.0f));
			session.Tick();
			CHECK_FALSE(session.GetUi().GetInteraction().Hovered.IsValid());
		}

		// The milestone review (decisions 24 and 50, the lead's decision 2): a pointer outside the scene image, in a
		// PixelPerfectCamera's letterbox, reaches no UI, as viewport.pick takes nothing there: neither a Screen canvas's element
		// whose rectangle reaches past the image's edge nor a World canvas that the pointer's ray meets beyond the view's edge,
		// where neither draws. The 200 x 120 reference resolution in the 640 x 360 view is upscaled 3 times between 20-pixel
		// bars; "Bleed" covers scene pixels [-10, 30) x [90, 110), and the World canvas "Board" (16 x 16 units, x -16 to 0,
		// 5 units in front of the camera, whose view spans x -6.25 to 6.25) reaches past the image's left edge. A click inside
		// the image on each still clicks it.
		TEST_CASE("PlaySession: under a PixelPerfectCamera a click in the letterbox reaches no UI, though an element or a World canvas reaches past the image")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			PopulateUiScene(scene);
			Entity bleed = scene.CreateEntity("Bleed", scene.FindEntityByPath("/Canvas"));
			bleed.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(-10.0f, 90.0f),
				.Size = glm::vec2(40.0f, 20.0f) });
			bleed.AddComponent<UiImageComponent>(UiImageComponent{});
			bleed.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			camera.AddComponent<PixelPerfectCameraComponent>(PixelPerfectCameraComponent{ .ReferenceWidth = 200, .ReferenceHeight = 120, .PixelsPerUnit = 16.0f });
			static_cast<void>(AddWorldButtonCanvas(scene, "Board", glm::vec3(-8.0f, 0.0f, -5.0f), 64));
			UiEventTaker events;
			PlaySessionSpecification specification = MakeSpecification(fixture, &events);
			specification.ViewWidth = 640;
			specification.ViewHeight = 360;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			const UUID bleedId = session.GetScene().FindEntityByPath("/Canvas/Bleed").GetUUID();
			const UUID boardButton = session.GetScene().FindEntityByPath("/Board/Button").GetUUID();

			// Ticks 0 and 2: view (10, 300), scene (-3.3, 100), on Bleed's rectangle; view (10, 210), scene (-3.3, 70), whose ray
			// meets the Board at x -6.46. Ticks 4 and 6: view (50, 300), scene (10, 100), on Bleed; view (170, 210), scene
			// (50, 70), whose ray meets the Board at x -3.1.
			QueueClick(session, 0, glm::vec2(10.0f, 300.0f));
			QueueClick(session, 2, glm::vec2(10.0f, 210.0f));
			QueueClick(session, 4, glm::vec2(50.0f, 300.0f));
			QueueClick(session, 6, glm::vec2(170.0f, 210.0f));
			for (const uint64_t bar : { uint64_t{ 0 }, uint64_t{ 2 } })
			{
				while (session.GetTick() <= bar)
					session.Tick();
				INFO("tick " << bar);
				CHECK_FALSE(session.GetUi().GetInteraction().Hovered.IsValid());
				CHECK_FALSE(session.GetUi().GetInteraction().Pressed.IsValid());
				CHECK_FALSE(session.GetUi().GetInteraction().PointerOverUi);
			}
			while (session.GetTick() < 8)
				session.Tick();
			CHECK(GetClicks(events.Events) == std::vector<UUID>{ bleedId, boardButton });
		}

		// The milestone review (decision 50, the lead's decision 10): a World canvas takes a click through a PixelPerfectCamera's
		// ray, the orthographic projection the camera draws with whatever its Camera says (its own projection is perspective
		// here). "Sign", 2 x 2 units at (4, 2, -50), is at scene pixel (164, 28) of the 200 x 120 reference resolution, view
		// (512, 84) of the 640 x 360 view; the Camera's perspective ray through that view point would pass it at x 31.
		TEST_CASE("PlaySession: under a PixelPerfectCamera a click on a World canvas's button through the camera clicks it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			camera.AddComponent<PixelPerfectCameraComponent>(PixelPerfectCameraComponent{ .ReferenceWidth = 200, .ReferenceHeight = 120, .PixelsPerUnit = 16.0f });
			static_cast<void>(AddWorldButtonCanvas(scene, "Sign", glm::vec3(4.0f, 2.0f, -50.0f), 8));
			UiEventTaker events;
			PlaySessionSpecification specification = MakeSpecification(fixture, &events);
			specification.ViewWidth = 640;
			specification.ViewHeight = 360;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			const UUID button = session.GetScene().FindEntityByPath("/Sign/Button").GetUUID();
			QueueClick(session, 0, glm::vec2(512.0f, 84.0f));
			session.Tick();
			CHECK(session.GetUi().GetInteraction().Hovered == button);
			CHECK(session.GetUi().GetInteraction().Pressed == button);
			CHECK(session.GetUi().GetInteraction().PointerOverUi);
			session.Tick();
			CHECK(GetClicks(events.Events) == std::vector<UUID>{ button });
		}

		// The milestone review (decision 28, the lead's decision 4): a focus that Ui.SetFocus sets outside the session's phases
		// (script.eval {context: Play} between two ticks) on a menu enabled after the last layout is kept at the next UiInput:
		// the stale-state step checks a focus the stored layout does not hold against the scene as it is, the layout SetFocus
		// checked it against, so the focus neither receives Focus (false) nor is lost.
		TEST_CASE("PlaySession: a focus a script sets between two ticks on a menu it has just shown is kept at the next tick")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			PopulateUiScene(scene);
			Entity pause = scene.CreateEntity("Pause");
			pause.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .SortOrder = 1 });
			Entity resume = scene.CreateEntity("Resume", pause);
			resume.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(0.0f, 60.0f),
				.Size = glm::vec2(100.0f, 20.0f) });
			resume.AddComponent<UiImageComponent>(UiImageComponent{});
			resume.AddComponent<UiButtonComponent>(UiButtonComponent{});
			pause.SetActive(false);
			UiEventTaker events;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture, &events), scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			QueuePointer(session, 0, glm::vec2(500.0f));
			session.Tick();
			const UUID resumeId = session.GetScene().FindEntityByPath("/Pause/Resume").GetUUID();
			REQUIRE(FindUiLayoutNode(session.GetUi().GetLayout(), resumeId) == nullptr);

			const auto shown = session.GetScripts()->Evaluate(R"(
local pause = Scene.FindByName("Pause")
pause:SetActive(true)
Ui.SetFocus(Scene.FindByName("Resume"))
return Ui.GetFocus() == Scene.FindByName("Resume")
)",
				{});
			REQUIRE_MESSAGE(shown.has_value(), (shown ? "" : shown.error().ToString()));
			CHECK(shown->Value.Get() == Json(true));
			REQUIRE(session.GetUi().GetInteraction().Focused == resumeId);

			session.Tick();
			CHECK(session.GetUi().GetInteraction().Focused == resumeId);
			CHECK(events.Events == std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Focus, .Target = resumeId, .Flag = true } });
			session.Tick();
			CHECK(session.GetUi().GetInteraction().Focused == resumeId);
		}

		// The milestone review (decision 24, the lead's decision 6: an accepted limit, Backlog §6): UI input, events and
		// callbacks run in the fixed step, for determinism, so while the time scale is 0 live play runs frames but no tick, and
		// a click reaches no widget; it is processed once the scale lets ticks run again. A pause menu keeps the time scale and
		// pauses gameplay with a flag its behaviours check. The frames are driven as a live host drives them, through a
		// FixedStepScheduler advanced at the session's time scale.
		TEST_CASE("PlaySession: with a time scale of 0 live play runs no tick, so a click fires nothing until the scale returns")
		{
			Test::SceneTestFixture fixture;
			PopulateUiScene(fixture.GetScene());
			UiEventTaker events;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture, &events), fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			const UUID a = session.GetScene().FindEntityByPath("/Canvas/A").GetUUID();
			FixedStepScheduler scheduler(FrameLoopConfig{ .FixedHz = 60 });
			uint64_t frameIndex = 0;
			const auto frame = [&session, &scheduler, &frameIndex]()
			{
				constexpr double Delta = 1.0 / 60.0;
				const FrameSteps steps = scheduler.Advance(Delta, session.GetTimeScale());
				for (uint32_t step = 0; step < steps.StepCount; ++step)
					session.FixedStep();
				session.FrameUpdate(FrameTime{ .DeltaTime = Delta * session.GetTimeScale(), .UnscaledDeltaTime = Delta, .Alpha = steps.Alpha, .FrameIndex = frameIndex++ });
			};

			REQUIRE(session.SetTimeScale(0.0).has_value());
			QueueClick(session, 0, glm::vec2(10.0f));
			for (int count = 0; count < 30; ++count)
				frame();
			CHECK(session.GetTick() == 0);
			CHECK(session.GetUi().GetInteraction().IsEmpty());
			CHECK(events.Events.empty());

			REQUIRE(session.SetTimeScale(1.0).has_value());
			for (int count = 0; count < 30 && session.GetTick() < 2; ++count)
				frame();
			REQUIRE(session.GetTick() >= 2);
			CHECK(GetClicks(events.Events) == std::vector<UUID>{ a });
		}

		// Decision 25 (the review of W22.2's tests major, the lead's decision 3): the session passes its interaction state to
		// the extraction of its game view (the RenderExtraction phase) and to ExtractView (what viewport.screenshot of the play
		// scene captures), so a widget the pointer hovers or presses, or that has focus, takes its canvas's tint for that state
		// in both, while the others keep Normal.
		TEST_CASE("PlaySession: the game view and ExtractView tint the widgets by the session's interaction state")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			PopulateUiScene(scene);
			const UiTheme theme{ .Normal = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f),
				.Hovered = glm::vec4(0.5f, 1.0f, 1.0f, 1.0f),
				.Pressed = glm::vec4(1.0f, 0.5f, 1.0f, 1.0f),
				.Focused = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f) };
			scene.FindEntityByPath("/Canvas").GetComponent<UiCanvasComponent>().Theme = theme;
			const glm::vec4 color(0.8f, 0.6f, 0.4f, 1.0f);
			for (const std::string_view path : { "/Canvas/A", "/Canvas/B" })
				scene.FindEntityByPath(path).GetComponent<UiImageComponent>().Color = color;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture), scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			const UUID a = session.GetScene().FindEntityByPath("/Canvas/A").GetUUID();
			const UUID b = session.GetScene().FindEntityByPath("/Canvas/B").GetUUID();
			const auto imageColor = [](const RenderSnapshot& snapshot, UUID entity)
			{
				const auto found = std::ranges::find_if(snapshot.Ui.Items, [entity](const UiDrawItem& item)
				{
					return item.Entity == entity && item.Kind == UiDrawKind::Image;
				});
				REQUIRE(found != snapshot.Ui.Items.end());
				return found->Color;
			};
			// The tints of A and B in the game view of the last frame and in an ExtractView of the play scene.
			const auto checkTints = [&session, &imageColor, a, b, color](glm::vec4 tintA, glm::vec4 tintB)
			{
				CHECK(imageColor(session.GetLastExtraction(), a) == color * tintA);
				CHECK(imageColor(session.GetLastExtraction(), b) == color * tintB);
				RenderExtractionRequest request;
				request.Camera = RenderCameraSource::Primary;
				request.Width = 320;
				request.Height = 180;
				const Result<RenderSnapshot> view = session.ExtractView(request);
				REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
				CHECK(imageColor(*view, a) == color * tintA);
				CHECK(imageColor(*view, b) == color * tintB);
			};
			const auto mouse = [&session](uint64_t tick, PlayInputEventState state, glm::vec2 position)
			{
				REQUIRE(session.GetInput()
						.Queue(tick,
							PlayInputEvent{ .Type = PlayInputEventType::MouseButtonInput,
								.State = state,
								.Button = MouseButton::Left,
								.HasPosition = true,
								.Position = position })
						.has_value());
			};

			// The pointer away from the UI: Normal. Over A: Hovered. A held press on A: Pressed (over its focus and hover).
			// Released away from it: A keeps the focus the press gave it, Focused, and B stays Normal throughout.
			QueuePointer(session, 0, glm::vec2(500.0f));
			session.Tick();
			checkTints(theme.Normal, theme.Normal);
			QueuePointer(session, 1, glm::vec2(10.0f));
			session.Tick();
			REQUIRE(session.GetUi().GetInteraction().Hovered == a);
			checkTints(theme.Hovered, theme.Normal);
			mouse(2, PlayInputEventState::Down, glm::vec2(10.0f));
			session.Tick();
			REQUIRE(session.GetUi().GetInteraction().Pressed == a);
			checkTints(theme.Pressed, theme.Normal);
			mouse(3, PlayInputEventState::Up, glm::vec2(500.0f));
			session.Tick();
			REQUIRE(session.GetUi().GetInteraction().Focused == a);
			REQUIRE_FALSE(session.GetUi().GetInteraction().Hovered.IsValid());
			checkTints(theme.Focused, theme.Normal);
		}

		// Roadmap M22's acceptance "scenes without UI keep their hash" (decision 24 step 1, the C22.2 review's major): in a
		// scene without a canvas, UiInput reads no input, so a held gamepad stick (with the pointer pressed and moved) leaves
		// the interaction state empty and the ui block absent at every tick: the session hashes as one without that input.
		// Live: it holds on the contract tree, where no session runs UiInput yet, and keeps holding once stream A adds it.
		TEST_CASE("PlaySession: a scene without a canvas keeps its state hash while a gamepad stick is held")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			Result<Scope<PlaySession>> held = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			Result<Scope<PlaySession>> idle = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(held.has_value(), held.error().ToString());
			REQUIRE_MESSAGE(idle.has_value(), idle.error().ToString());
			REQUIRE((*held)->GetInput().Queue(0, PlayInputEvent{ .Type = PlayInputEventType::GamepadAxisInput, .Value = 0.75f, .Gamepad = 0, .Axis = GamepadAxis::LeftY }).has_value());
			REQUIRE((*held)->GetInput().Queue(0, PlayInputEvent{ .Type = PlayInputEventType::MouseButtonInput, .Button = MouseButton::Left, .HasPosition = true, .Position = glm::vec2(0.0f) }).has_value());
			QueuePointer(**held, 1, glm::vec2(10.0f));
			for (int tick = 0; tick < 3; ++tick)
			{
				(*held)->Tick();
				(*idle)->Tick();
				CHECK((*held)->GetUi().GetInteraction().IsEmpty());
				CHECK((*held)->GetUi().GetQueuedEventCount() == 0);
				CHECK((*held)->ComputeStateHash() == (*idle)->ComputeStateHash());
			}
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 46; stream T): the text latch between ApplyInput and UiInput, in
		// Play and Simulate, and the step view holds the tick's text. Relative order only (the lead's decision 7 of the review
		// of C22.3): the full phase lists differ with the phases M21 and M23 insert, whichever lands first, so this case
		// holds on every merged tree; the documented-sequence cases, which stream T extends, pin the whole lists.
		TEST_CASE("PlaySession: TextLatch follows ApplyInput and precedes UiInput, in Play and Simulate")
		{
			Test::SceneTestFixture fixture;
			PopulateUiScene(fixture.GetScene());
			const auto indexOf = [](const std::vector<PlaySessionPhase>& phases, PlaySessionPhase phase)
			{
				return std::ranges::find(phases, phase) - phases.begin();
			};
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				CAPTURE(mode == PlayMode::Play);
				RecordingObserver observer;
				PlaySessionSpecification specification = MakeSpecification(fixture, &observer);
				specification.Mode = mode;
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				REQUIRE((*session)->GetInput().Queue(0, PlayInputEvent{ .Type = PlayInputEventType::Text, .Text = "ok" }).has_value());
				(*session)->FixedStep();
				const auto latch = indexOf(observer.Phases, PlaySessionPhase::TextLatch);
				REQUIRE(latch < std::ssize(observer.Phases));
				CHECK(std::ranges::count(observer.Phases, PlaySessionPhase::TextLatch) == 1);
				CHECK(latch == indexOf(observer.Phases, PlaySessionPhase::ApplyInput) + 1);
				CHECK(indexOf(observer.Phases, PlaySessionPhase::UiInput) == latch + 1);
				CHECK((*session)->GetInput().GetText(InputPhase::Step) == "ok");
			}
		}

		// C22.3's guard (decision 54, the lead's lesson from C22.2's and C21.2's reviews): a scene without a text input or a
		// World canvas keeps its state hash exactly whatever is typed (typing reaches no component while no text input has
		// focus, and the editing keys and Enter change nothing), and its UI interaction ignores where the primary camera looks
		// (no pointer ray is cast without a World canvas). It holds on the contract tree, where no session latches text or
		// casts a ray, and guards streams T and W.
		TEST_CASE("PlaySession: a scene with Screen UI and no text input or World canvas keeps its state hash while text is typed")
		{
			Test::SceneTestFixture fixture;
			PopulateUiScene(fixture.GetScene());
			fixture.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Result<Scope<PlaySession>> typing = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			Result<Scope<PlaySession>> turning = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			Result<Scope<PlaySession>> idle = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(typing.has_value(), typing.error().ToString());
			REQUIRE_MESSAGE(turning.has_value(), turning.error().ToString());
			REQUIRE_MESSAGE(idle.has_value(), idle.error().ToString());
			// All three hover button A; one types and presses the editing keys, one turns its camera.
			for (PlaySession* session : { typing->get(), turning->get(), idle->get() })
				QueuePointer(*session, 0, glm::vec2(10.0f));
			REQUIRE((*typing)->GetInput().Queue(0, PlayInputEvent{ .Type = PlayInputEventType::Text, .Text = "abc \xD7\x90" }).has_value());
			for (const Key key : { Key::Backspace, Key::Delete, Key::Left, Key::Right, Key::Home, Key::End, Key::Enter })
				QueueKeyTap(**typing, 1, key);
			REQUIRE((*typing)->GetInput().Queue(2, PlayInputEvent{ .Type = PlayInputEventType::Text, .Text = "d" }).has_value());
			for (int tick = 0; tick < 4; ++tick)
			{
				(*turning)->GetScene().FindEntityByPath("/Camera").Patch<TransformComponent>([tick](TransformComponent& transform)
				{
					transform.Rotation = glm::angleAxis(0.25f * static_cast<float>(tick + 1), glm::vec3(0.0f, 1.0f, 0.0f));
				});
				(*typing)->Tick();
				(*turning)->Tick();
				(*idle)->Tick();
				CHECK((*typing)->GetUi().GetInteraction() == (*idle)->GetUi().GetInteraction());
				CHECK((*turning)->GetUi().GetInteraction() == (*idle)->GetUi().GetInteraction());
				CHECK((*typing)->ComputeStateHash() == (*idle)->ComputeStateHash());
			}
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decisions 46 and 47; stream T): the session gives UiInput the text latch's
		// characters, the fixed editing keys (KeypadEnter as Enter) and the commands a gamepad pressed, so a focused text input
		// is typed into and edited, a keyboard binding (Up, Space as Submit) neither navigates nor submits while it has focus,
		// and a gamepad's Submit and direction do.
		TEST_CASE("PlaySession: a focused text input takes the typed text and the editing keys, and only gamepad commands leave it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			PopulateUiScene(scene);
			Entity field = scene.CreateEntity("Field", scene.FindEntityByPath("/Canvas"));
			field.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(0.0f, 60.0f),
				.Size = glm::vec2(100.0f, 20.0f) });
			field.AddComponent<UiImageComponent>(UiImageComponent{});
			field.AddComponent<UiTextInputComponent>(UiTextInputComponent{});
			UiEventTaker events;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture, &events), scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			PlayInput& input = session.GetInput();
			// Tick 0: a click focuses the field. Tick 1: "ab", "c", then Left. Tick 3: Backspace and the keypad's Enter.
			// Tick 5: the keyboard's Up and Space. Tick 7: the gamepad's South (Submit). Tick 9: its DPadUp.
			REQUIRE(input
					.Queue(0, PlayInputEvent{ .Type = PlayInputEventType::MouseButtonInput, .State = PlayInputEventState::Tap, .Button = MouseButton::Left, .HasPosition = true, .Position = glm::vec2(10.0f, 70.0f) })
					.has_value());
			REQUIRE(input.Queue(1, PlayInputEvent{ .Type = PlayInputEventType::Text, .Text = "ab" }).has_value());
			REQUIRE(input.Queue(1, PlayInputEvent{ .Type = PlayInputEventType::Text, .Text = "c" }).has_value());
			QueueKeyTap(session, 1, Key::Left);
			QueueKeyTap(session, 3, Key::Backspace);
			QueueKeyTap(session, 3, Key::KeypadEnter);
			QueueKeyTap(session, 5, Key::Up);
			QueueKeyTap(session, 5, Key::Space);
			for (const auto& [tick, button] : { std::pair{ uint64_t{ 7 }, GamepadButton::South }, std::pair{ uint64_t{ 9 }, GamepadButton::DPadUp } })
			{
				REQUIRE(input
						.Queue(tick, PlayInputEvent{ .Type = PlayInputEventType::GamepadButtonInput, .State = PlayInputEventState::Tap, .Gamepad = 0, .GamepadButtonCode = button })
						.has_value());
			}

			for (int tick = 0; tick < 7; ++tick)
				session.Tick();
			// "abc" with the caret moved left, then Backspace removed "b"; the keyboard's Up and Space left the focus alone.
			const UiTextInputComponent& typed = session.GetScene().FindEntityByPath("/Canvas/Field").GetComponent<UiTextInputComponent>();
			CHECK(typed.Text == "ac");
			CHECK(typed.Caret == 1u);
			const UUID fieldId = field.GetUUID();
			CHECK(session.GetUi().GetInteraction().Focused == fieldId);
			for (int tick = 7; tick < 11; ++tick)
				session.Tick();
			const UUID b = session.GetScene().FindEntityByPath("/Canvas/B").GetUUID();
			CHECK(session.GetUi().GetInteraction().Focused == b);
			CHECK(events.Events
				== std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Hover, .Target = fieldId, .Flag = true },
					UiEvent{ .Kind = UiEventKind::Focus, .Target = fieldId, .Flag = true }, UiEvent{ .Kind = UiEventKind::TextChanged, .Target = fieldId },
					UiEvent{ .Kind = UiEventKind::TextChanged, .Target = fieldId }, UiEvent{ .Kind = UiEventKind::Submit, .Target = fieldId },
					UiEvent{ .Kind = UiEventKind::Submit, .Target = fieldId }, UiEvent{ .Kind = UiEventKind::Focus, .Target = fieldId, .Flag = false },
					UiEvent{ .Kind = UiEventKind::Focus, .Target = b, .Flag = true } });
		}

		// The milestone review (decision 47, the lead's decision 7): while a text input has focus, the keys it edits with
		// (Backspace, Delete, Left, Right, Home and End) do not fire Cancel, Next or Previous when the Ui settings bind them to
		// those commands, so Backspace bound to Cancel ("back") only deletes and Home bound to Next only moves the caret; a key
		// it does not edit with (Escape, Tab) still fires its command.
		TEST_CASE("PlaySession: a key a focused text input edits with fires no Cancel, Next or Previous bound to it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			PopulateUiScene(scene);
			Entity field = scene.CreateEntity("Field", scene.FindEntityByPath("/Canvas"));
			field.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(0.0f, 60.0f),
				.Size = glm::vec2(100.0f, 20.0f) });
			field.AddComponent<UiImageComponent>(UiImageComponent{});
			field.AddComponent<UiTextInputComponent>(UiTextInputComponent{});
			UiEventTaker events;
			PlaySessionSpecification specification = MakeSpecification(fixture, &events);
			specification.Project.Ui.Cancel = { "Key.Escape", "Key.Backspace", "Gamepad.East" };
			specification.Project.Ui.Next = { "Key.Tab", "Key.Home" };
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			const UUID fieldId = session.GetScene().FindEntityByPath("/Canvas/Field").GetUUID();
			// Tick 0: a click focuses the field. Tick 1: "ab". Tick 3: Backspace. Tick 5: Home. Tick 7: Escape. Tick 9: Tab.
			QueueClick(session, 0, glm::vec2(10.0f, 70.0f));
			REQUIRE(session.GetInput().Queue(1, PlayInputEvent{ .Type = PlayInputEventType::Text, .Text = "ab" }).has_value());
			QueueKeyTap(session, 3, Key::Backspace);
			QueueKeyTap(session, 5, Key::Home);
			QueueKeyTap(session, 7, Key::Escape);
			QueueKeyTap(session, 9, Key::Tab);

			while (session.GetTick() < 7)
				session.Tick();
			const UiTextInputComponent& typed = session.GetScene().FindEntityByPath("/Canvas/Field").GetComponent<UiTextInputComponent>();
			CHECK(typed.Text == "a");
			CHECK(typed.Caret == 0u);
			CHECK(session.GetUi().GetInteraction().Focused == fieldId);
			while (session.GetTick() < 11)
				session.Tick();
			CHECK(session.GetUi().GetInteraction().Focused != fieldId);
			std::vector<UiEvent> received;
			std::ranges::copy_if(events.Events, std::back_inserter(received), [](const UiEvent& event)
			{
				return event.Kind != UiEventKind::Hover;
			});
			REQUIRE(received.size() == 6);
			CHECK(received[0] == UiEvent{ .Kind = UiEventKind::Focus, .Target = fieldId, .Flag = true });
			CHECK(received[1] == UiEvent{ .Kind = UiEventKind::TextChanged, .Target = fieldId });
			CHECK(received[2] == UiEvent{ .Kind = UiEventKind::TextChanged, .Target = fieldId });
			CHECK(received[3] == UiEvent{ .Kind = UiEventKind::Cancel, .Target = fieldId });
			CHECK(received[4] == UiEvent{ .Kind = UiEventKind::Focus, .Target = fieldId, .Flag = false });
			CHECK(received[5].Kind == UiEventKind::Focus);
			CHECK(received[5].Flag);
		}

		// Roadmap M22's "every widget works by keyboard and gamepad", for the text input (decision 47, amended by W22.3's
		// review): a gamepad alone focuses a text input (by navigation), submits it and leaves it; it types nothing into it,
		// since characters come only from a text source (the keyboard's committed characters) and there is no on-screen
		// keyboard (an accepted limit, Backlog §6).
		TEST_CASE("PlaySession: a gamepad focuses, submits and leaves a text input, and types nothing into it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			PopulateUiScene(scene);
			Entity canvas = scene.FindEntityByPath("/Canvas");
			const UUID b = scene.FindEntityByPath("/Canvas/B").GetUUID();
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = b;
			Entity field = scene.CreateEntity("Field", canvas);
			field.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(0.0f, 60.0f),
				.Size = glm::vec2(100.0f, 20.0f) });
			field.AddComponent<UiImageComponent>(UiImageComponent{});
			field.AddComponent<UiTextInputComponent>(UiTextInputComponent{});
			const UUID fieldId = field.GetUUID();
			UiEventTaker events;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture, &events), scene);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			// Gamepad 0's taps, two ticks apart: DPadDown focuses the DefaultFocus B, DPadDown moves to the field, South submits
			// it, every other face button presses nothing into it, and DPadUp leaves it for B.
			const std::vector<std::pair<uint64_t, GamepadButton>> taps = { { 0, GamepadButton::DPadDown }, { 2, GamepadButton::DPadDown },
				{ 4, GamepadButton::South }, { 6, GamepadButton::West }, { 6, GamepadButton::North }, { 8, GamepadButton::DPadUp } };
			for (const auto& [tick, button] : taps)
			{
				const PlayInputEvent tap{
					.Type = PlayInputEventType::GamepadButtonInput,
					.State = PlayInputEventState::Tap,
					.Gamepad = 0,
					.GamepadButtonCode = button,
				};
				REQUIRE(session.GetInput().Queue(tick, tap).has_value());
			}
			const std::vector<std::pair<uint64_t, UUID>> focus = { { 0, b }, { 2, fieldId }, { 4, fieldId }, { 6, fieldId }, { 8, b } };
			for (const auto& [tick, focused] : focus)
			{
				while (session.GetTick() <= tick)
					session.Tick();
				INFO("tick " << tick);
				CHECK(session.GetUi().GetInteraction().Focused == focused);
			}
			CHECK(session.GetScene().FindEntityByPath("/Canvas/Field").GetComponent<UiTextInputComponent>().Text.empty());
			std::vector<UiEvent> received;
			std::ranges::copy_if(events.Events, std::back_inserter(received), [](const UiEvent& event)
			{
				return event.Kind != UiEventKind::Hover;
			});
			const auto focusEvent = [](UUID target, bool focused)
			{
				return UiEvent{ .Kind = UiEventKind::Focus, .Target = target, .Flag = focused };
			};
			CHECK(received
				== std::vector<UiEvent>{ focusEvent(b, true), focusEvent(b, false), focusEvent(fieldId, true),
					UiEvent{ .Kind = UiEventKind::Submit, .Target = fieldId }, focusEvent(fieldId, false), focusEvent(b, true) });
		}

		// Decision 30: the Ui settings name the controls of each command and can turn navigation off; a binding that does not
		// name a key or a gamepad button fails the session's start at its location, as an action's binding does. Next with a
		// Shift key down is Previous, and a gamepad command counts on any connected gamepad (gamepad 1 here). Navigation
		// starts from the canvas's DefaultFocus (decision 23). Taps are two ticks apart, so each press is a new edge.
		TEST_CASE("PlaySession: the Ui settings' bindings drive navigation, and a binding that is not a key or gamepad button fails the start")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			PopulateUiScene(scene);
			const UUID a = scene.FindEntityByPath("/Canvas/A").GetUUID();
			const UUID b = scene.FindEntityByPath("/Canvas/B").GetUUID();
			Entity canvas = scene.FindEntityByPath("/Canvas");
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = a;
			// A toggle "C" below B, at (0, 60), whose IsOn shows a gamepad's Submit.
			Entity toggle = scene.CreateEntity("C", canvas);
			toggle.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(0.0f, 60.0f),
				.Size = glm::vec2(100.0f, 20.0f) });
			toggle.AddComponent<UiImageComponent>(UiImageComponent{});
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{});
			const UUID c = toggle.GetUUID();
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.Project.Ui.Next = { "Key.N" };
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			const auto gamepadTap = [&play](uint64_t tick, GamepadButton button)
			{
				REQUIRE(play.GetInput().Queue(tick, PlayInputEvent{ .Type = PlayInputEventType::GamepadButtonInput, .State = PlayInputEventState::Tap, .Gamepad = 1, .GamepadButtonCode = button }).has_value());
			};
			const auto key = [&play](uint64_t tick, Key code, PlayInputEventState state)
			{
				REQUIRE(play.GetInput().Queue(tick, PlayInputEvent{ .Type = PlayInputEventType::KeyInput, .State = state, .KeyCode = code }).has_value());
			};
			// The focus after each tick: N focuses the DefaultFocus, Tab is no longer Next, N moves on, Shift+N (Previous)
			// moves back, gamepad 1's DPadDown moves down twice and its South flips the toggle.
			QueueKeyTap(play, 0, Key::N);
			QueueKeyTap(play, 2, Key::Tab);
			QueueKeyTap(play, 4, Key::N);
			key(6, Key::LeftShift, PlayInputEventState::Down);
			QueueKeyTap(play, 6, Key::N);
			key(7, Key::LeftShift, PlayInputEventState::Up);
			gamepadTap(8, GamepadButton::DPadDown);
			gamepadTap(10, GamepadButton::DPadDown);
			gamepadTap(12, GamepadButton::South);
			const std::vector<std::pair<uint64_t, UUID>> focus = { { 0, a }, { 2, a }, { 4, b }, { 6, a }, { 8, b }, { 10, c }, { 12, c } };
			for (const auto& [tick, focused] : focus)
			{
				while (play.GetTick() <= tick)
					play.Tick();
				INFO("tick " << tick);
				CHECK(play.GetUi().GetInteraction().Focused == focused);
			}
			CHECK(play.GetScene().FindEntityByPath("/Canvas/C").GetComponent<UiToggleComponent>().IsOn);

			specification.Project.Ui.Navigation = false;
			Result<Scope<PlaySession>> off = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(off.has_value(), off.error().ToString());
			QueueKeyTap(**off, 0, Key::N);
			(*off)->Tick();
			CHECK_FALSE((*off)->GetUi().GetInteraction().Focused.IsValid());

			for (const std::string_view binding : { "Mouse.Left", "Gamepad.LeftX", "Key.Nope" })
			{
				INFO(std::string(binding));
				PlaySessionSpecification refused = MakeSpecification(fixture);
				refused.Project.Ui.Submit = { "Key.Enter", std::string(binding) };
				const Result<Scope<PlaySession>> failed = PlaySession::CreateFromScene(refused, scene);
				REQUIRE_FALSE(failed.has_value());
				CHECK(failed.error().GetCode() == ErrorCode::Validation);
				REQUIRE_FALSE(failed.error().GetIssues().empty());
				CHECK(failed.error().GetIssues()[0].JsonPointer == "/Ui/Submit/1");
			}
		}

		// W-CLOCK (M22, C22.1, decision 12; stream K): the session previews device input in the frame view only in live play.
		TEST_CASE("PlaySession: the frame preview is on in live play and off in lockstep")
		{
			Test::SceneTestFixture fixture;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			play.FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 0.5, .FrameIndex = 0 });
			CHECK(play.GetInput().IsFramePreviewEnabled());
			play.SetLockstep(true);
			play.Tick();
			CHECK_FALSE(play.GetInput().IsFramePreviewEnabled());
		}

		// W-CLOCK (decision 12; stream K): a recording replays with the frame views its ticks gave, so while it records the
		// session latches the frame view of the applied ticks only; live play's frame without a step sees the device press.
		TEST_CASE("PlaySession: the frame preview is off while recording, and live play previews device input before its tick")
		{
			Test::ScriptTestFixture fixture;
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 47;
			const FrameTime frame{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 0.5, .FrameIndex = 0 };
			const Event space = KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Pressed, .Modifiers = KeyModifiers::None, .Handled = false };

			Result<Scope<PlaySession>> live = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(live.has_value(), live.error().ToString());
			(*live)->SetExtractionEnabled(false);
			(*live)->GetInput().QueueDeviceEvent(space);
			(*live)->FrameUpdate(frame);
			CHECK((*live)->GetInput().IsFramePreviewEnabled());
			CHECK((*live)->GetInput().GetDevices().WasKeyPressed(InputPhase::Frame, Key::Space));
			CHECK_FALSE((*live)->GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::Space));

			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& recording = **created;
			recording.SetExtractionEnabled(false);
			ReplayHeader header{};
			header.Scene = { AssetHandle(0x7100b1), "Assets/Scenes/Preview.scene" };
			header.Seed = specification.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";
			const Status began = recording.GetRecorder()->Begin(header, recording);
			REQUIRE_MESSAGE(began.has_value(), began.error().ToString());
			recording.GetInput().QueueDeviceEvent(space);
			recording.FrameUpdate(frame);
			CHECK_FALSE(recording.GetInput().IsFramePreviewEnabled());
			CHECK_FALSE(recording.GetInput().GetDevices().IsKeyDown(InputPhase::Frame, Key::Space));
			// The tick applies the press, and its frame phase reports it.
			recording.Tick();
			CHECK(recording.GetInput().GetDevices().WasKeyPressed(InputPhase::Frame, Key::Space));
			CHECK(recording.GetInput().GetDevices().WasKeyPressed(InputPhase::Step, Key::Space));

			// Once the recording ends, the session previews again.
			recording.GetRecorder()->Cancel();
			recording.FrameUpdate(frame);
			CHECK(recording.GetInput().IsFramePreviewEnabled());
		}

		TEST_CASE("PlaySession: entity cap raises an error, never crashes")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene()); // 3 entities
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.Project.Simulation.MaxEntities = 4;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			CHECK(play.GetMaxEntities() == 4);
			CHECK(play.CheckEntityCapacity(1).has_value());
			REQUIRE(play.CreateEntity("Fourth").has_value());

			const Result<Entity> fifth = play.CreateEntity("Fifth");
			REQUIRE_FALSE(fifth.has_value());
			CHECK(fifth.error().GetCode() == ErrorCode::InvalidState);
			CHECK(fifth.error().GetMessageText().find("entity limit 4 reached") != std::string::npos);
			CHECK(play.GetScene().GetEntityCount() == 4);
			CHECK_FALSE(play.CheckEntityCapacity(1).has_value());
			play.Tick(); // the session keeps running after a refused spawn

			// A document over the cap does not start.
			specification.Project.Simulation.MaxEntities = 2;
			const Result<Scope<PlaySession>> tooMany = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_FALSE(tooMany.has_value());
			CHECK(tooMany.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("PlaySession: entities destroyed in a tick count toward twice the cap until the flush")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene()); // 3 entities
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.Project.Simulation.MaxEntities = 4;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			// Each create-and-destroy pair leaves one more entity in the registry until the end-of-tick flush: live, new
			// and pending entities together stay within 2 * 4.
			for (int pair = 0; pair < 5; ++pair)
			{
				const Result<Entity> created = play.CreateEntity("Transient");
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				play.GetScene().DestroyEntity(*created);
			}
			CHECK(play.GetScene().GetEntityCount() == 3);
			CHECK(play.GetScene().GetPendingDestroyCount() == 5);
			const Result<Entity> refused = play.CreateEntity("Refused");
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			CHECK(refused.error().GetMessageText().find("end-of-tick flush") != std::string::npos);
			CHECK(play.GetScene().GetEntityCount() == 3);
			play.Tick();
			CHECK(play.GetScene().GetPendingDestroyCount() == 0);
			CHECK(play.CreateEntity("After the flush").has_value());
		}

		TEST_CASE("PlaySession: identical sessions report identical state hashes, different seeds different ones")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			const auto run = [&fixture](uint64_t seed)
			{
				PlaySessionSpecification specification = MakeSpecification(fixture);
				specification.Seed = seed;
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				REQUIRE((*session)->CreateEntity("Spawned").has_value());
				for (int tick = 0; tick < 30; ++tick)
					(*session)->Tick();
				static_cast<void>((*session)->GetRandom().NextU64());
				return (*session)->ComputeStateHash();
			};
			const uint64_t first = run(7);
			CHECK(first != 0);
			CHECK(run(7) == first);
			CHECK(run(8) != first);
		}

		TEST_CASE("PlaySession: runtime spawns get the seeded deterministic ids of the session")
		{
			Test::SceneTestFixture fixture;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			const Result<Entity> spawned = (*session)->CreateEntity("Spawned");
			REQUIRE(spawned.has_value());
			UUIDGenerator expected = UUIDGenerator::CreateDeterministic(42);
			CHECK(spawned->GetUUID() == expected.Next());
		}

		TEST_CASE("PlaySession: a session starts only from a strictly valid document")
		{
			Test::SceneTestFixture fixture;
			Json document = Json::object();
			document["Format"] = "Scene";
			document["Version"] = 1;
			document["Entities"] = "not an array";
			const Result<Scope<PlaySession>> session = PlaySession::Create(MakeSpecification(fixture), document);
			REQUIRE_FALSE(session.has_value());
			CHECK(session.error().GetCode() == ErrorCode::Validation);

			PlaySessionSpecification noRegistry = MakeSpecification(fixture);
			noRegistry.Registry = nullptr;
			const Result<Scope<PlaySession>> invalid = PlaySession::CreateFromScene(noRegistry, fixture.GetScene());
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PlaySession: the project settings are checked and kept with the session")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.Project.Simulation.FixedHz = 0;
			const Result<Scope<PlaySession>> noRate = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_FALSE(noRate.has_value());
			CHECK(noRate.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(noRate.error().GetMessageText().contains("FixedHz"));

			// A 30 Hz project: every tick lasts 1/30 s, and the session keeps its copy of the settings.
			specification.Project.Simulation.FixedHz = 30;
			specification.Project.Simulation.MaxStepsPerFrame = 3;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			CHECK((*session)->GetFixedDelta() == doctest::Approx(1.0 / 30.0));
			CHECK((*session)->GetProjectSettings().Simulation.FixedHz == 30);
			CHECK((*session)->GetProjectSettings().Simulation.MaxStepsPerFrame == 3);

			// The other members a session cannot run without.
			for (const auto& [member, change] : std::vector<std::pair<std::string, void (*)(PlaySessionSpecification&)>>{
					 { "MaxStepsPerFrame", [](PlaySessionSpecification& changed)
			{
				changed.Project.Simulation.MaxStepsPerFrame = 0;
			} },
					 { "MaxEntities", [](PlaySessionSpecification& changed)
			{
				changed.Project.Simulation.MaxEntities = 0;
			} },
					 { "ViewWidth", [](PlaySessionSpecification& changed)
			{
				changed.ViewWidth = 0;
			} } })
			{
				CAPTURE(member);
				PlaySessionSpecification invalid = MakeSpecification(fixture);
				change(invalid);
				const Result<Scope<PlaySession>> refused = PlaySession::CreateFromScene(invalid, fixture.GetScene());
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(refused.error().GetMessageText().contains(member));
			}
		}

		TEST_CASE("PlaySession: the input stamped for a tick is applied at its step 1, before its fixed update")
		{
			Test::SceneTestFixture fixture;
			InputProbe probe;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture, &probe), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlayInputEvent space;
			space.Type = PlayInputEventType::KeyInput;
			space.KeyCode = Key::Space;
			space.State = PlayInputEventState::Tap;
			REQUIRE((*session)->GetInput().Queue(1, space).has_value());
			for (int tick = 0; tick < 4; ++tick)
				(*session)->Tick();
			// Down at tick 1, up at tick 2 (a tap), seen by that tick's fixed update.
			CHECK(probe.Pressed == std::vector<bool>{ false, true, false, false });
			CHECK(probe.Released == std::vector<bool>{ false, false, true, false });
			CHECK((*session)->GetInput().GetNextTick() == 4);
		}

		TEST_CASE("PlaySession: paused and lockstep sessions advance only through Tick")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			const FrameTime frame{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 0.5, .FrameIndex = 0 };

			// A running session follows the host's loop.
			session.AdvanceLoopStep();
			session.AdvanceLoopFrame(frame);
			CHECK(session.GetTick() == 1);
			CHECK(session.GetExtractionCount() == 1);
			CHECK(session.GetViewAlpha() == doctest::Approx(0.5f));

			session.SetPaused(true);
			session.AdvanceLoopStep();
			session.AdvanceLoopFrame(frame);
			CHECK(session.GetTick() == 1);
			CHECK(session.GetExtractionCount() == 1);
			CHECK(session.GetViewAlpha() == 1.0f);

			session.SetPaused(false);
			session.SetLockstep(true, 5);
			CHECK(session.IsLockstep());
			CHECK(session.GetLockstepOwner() == 5);
			session.AdvanceLoopStep();
			session.AdvanceLoopFrame(frame);
			CHECK(session.GetTick() == 1);
			session.Tick();
			CHECK(session.GetTick() == 2);
			CHECK(session.GetExtractionCount() == 2);

			// Leaving lockstep keeps the paused state, and forgets the owner.
			session.SetLockstep(false, 5);
			CHECK(session.GetLockstepOwner() == NoClient);
			CHECK_FALSE(session.IsPaused());
			session.SetExtractionEnabled(false);
			session.Tick();
			CHECK(session.GetExtractionCount() == 2);
			CHECK(session.GetViewAlpha() == 1.0f);
		}

		TEST_CASE("PlaySession: entities destroyed in a tick are gone after its destroy flush")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			DestroyingObserver destroyer;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture, &destroyer), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			(*session)->FixedStep();
			CHECK(destroyer.PendingAtFlush == 2); // the board and its cell, marked in the fixed update
			CHECK(destroyer.PendingAfterFlush == 0);
			CHECK((*session)->GetScene().GetEntityCount() == 1);
			CHECK_FALSE((*session)->GetScene().FindEntityByPath("/Board").IsValid());
		}

		TEST_CASE("PlaySession: the state hash covers the scene, the tick, the random stream and the spawn counter")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			std::vector<uint64_t> hashes = { session.ComputeStateHash() };
			session.Tick();
			hashes.push_back(session.ComputeStateHash());
			static_cast<void>(session.GetRandom().NextU64());
			hashes.push_back(session.ComputeStateHash());
			static_cast<void>(session.GetIdGenerator().Next());
			hashes.push_back(session.ComputeStateHash());
			Entity ball = session.GetScene().FindEntityByPath("/Ball");
			ball.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 5.0f;
			});
			hashes.push_back(session.ComputeStateHash());
			for (size_t first = 0; first < hashes.size(); ++first)
			{
				for (size_t second = first + 1; second < hashes.size(); ++second)
					CHECK(hashes[first] != hashes[second]);
			}
			// Reading the hash changes nothing.
			CHECK(session.ComputeStateHash() == hashes.back());
		}

		TEST_CASE("PlaySession: the run state is kept as set, and the time scale is checked")
		{
			Test::SceneTestFixture fixture;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			CHECK(session.GetMode() == PlayMode::Play);
			CHECK(session.GetSeed() == 42);
			CHECK(session.GetTimeScale() == 1.0);
			REQUIRE(session.SetTimeScale(0.0).has_value());
			REQUIRE(session.SetTimeScale(PlaySession::MaxTimeScale).has_value());
			for (const double invalid : { -0.5, PlaySession::MaxTimeScale + 1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
			{
				CAPTURE(invalid);
				const Status refused = session.SetTimeScale(invalid);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK(session.GetTimeScale() == PlaySession::MaxTimeScale);

			CHECK_FALSE(session.IsStepping());
			session.SetStepping(true);
			CHECK(session.IsStepping());
			CHECK_FALSE(session.IsModified());
			session.MarkModified();
			CHECK(session.IsModified());
			CHECK(session.IsExtractionEnabled());
			session.SetViewSize(320, 180);
			CHECK_FALSE(session.GetLastExtraction().HasCamera);
		}

		TEST_CASE("PlaySession: the session seed is Project Seed xor Scene Seed")
		{
			// Implemented by the contract (a constexpr in the frozen header).
			static_assert(PlaySession::ComputeSessionSeed(1337u, 42u) == (1337u ^ 42u));
			CHECK(PlaySession::ComputeSessionSeed(0u, 0u) == 0u);
			CHECK(PlaySession::ComputeSessionSeed(0xffffffffu, 0x0000ffffu) == 0xffff0000u);
		}

		TEST_CASE("PlaySession: the physics phases simulate bodies in Simulate mode as in Play mode")
		{
			// M11 (Docs/Decisions/0014-m11-decisions.md decision 14): Simulate is Play without scripts and audio, so a falling box
			// lands in both, at the same place.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Box", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			std::vector<glm::vec3> landed;
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 42, mode));
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				Test::RunTicks(**session, 120);
				landed.push_back(Test::GetWorldPosition(**session, "/Box"));
			}
			CHECK(landed[0].y == doctest::Approx(0.48f).epsilon(0.02));
			CHECK(landed[1] == landed[0]);
		}

		TEST_CASE("PlaySession: the state hash covers the bodies' velocities")
		{
			// §5.1 "+ physics velocities at runtime": a velocity that has not moved anything yet changes the hash.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Box", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			const uint64_t before = (*session)->ComputeStateHash();
			const UUID box = Test::GetEntityId((*session)->GetScene(), "/Box");
			REQUIRE((*session)->GetPhysics().SetLinearVelocity(box, glm::vec3(1.0f, 0.0f, 0.0f)).has_value());
			CHECK((*session)->ComputeStateHash() != before);
		}

		TEST_CASE("PlaySession: physics settings that cannot make a world fail the start with its context")
		{
			Test::SceneTestFixture fixture;
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Project.Physics.Layers = { "Track" };
			specification.Project.Physics.Collisions = {};
			const Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_FALSE(session.has_value());
			CHECK(session.error().GetCode() == ErrorCode::Validation);
			CHECK(session.error().ToString().contains("while starting the play session"));
		}

		TEST_CASE("PlaySessionPhase: every phase has its enumerator name")
		{
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::InterpolationSnapshot) == "InterpolationSnapshot");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::PreStepTransformUpdate) == "PreStepTransformUpdate");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::RenderExtraction) == "RenderExtraction");
			// M22 (Docs/Decisions/0033-m22-contract.md decision 3): the UI's layout phases.
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::UiLayout) == "UiLayout");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::FrameUiLayout) == "FrameUiLayout");
			// M22 C22.2 (decisions 24 and 27): the UI's interaction phases.
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::UiInput) == "UiInput");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::UiEvents) == "UiEvents");
			// M22 C22.3 (decision 46): the text latch.
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::TextLatch) == "TextLatch");
		}

		TEST_CASE("PlaySession: scripts measure screen coordinates in the session's view, never the host's window")
		{
			Test::ScriptTestFixture fixture;
			fixture.GetScene().CreateEntity("Camera").AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			// An editor host: its window is much larger than, and differently shaped from, the game view it renders into.
			WindowHost host;
			host.Environment.WindowSize = glm::vec2(1920.0f, 1080.0f);
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 47;
			specification.Serial = 3;
			specification.Host = &host;
			specification.ViewWidth = 800;
			specification.ViewHeight = 600;
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
			PlaySession& session = **created;
			const auto evaluate = [&session](std::string_view code)
			{
				const auto result = session.GetScripts()->Evaluate(code, {});
				REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
				return result->Value.Get();
			};
			CHECK(session.GetViewSize() == glm::uvec2(800, 600));
			CHECK(evaluate("return Application.GetWindowSize() == vector.create(800, 600, 0)") == Json(true));
			CHECK(evaluate("return Application.IsFocused()") == Json(true)); // the host still supplies the application facts
			// The centre pixel of the 800x600 view looks straight ahead; against the host's window it would not.
			CHECK(evaluate(R"(
local origin, direction = Scene.FindByName("Camera"):GetComponent("Camera"):ScreenToWorldRay(399.5, 299.5)
return vector.magnitude(direction - vector.create(0, 0, -1)) < 0.00001)")
				== Json(true));
			CHECK(evaluate(R"(
local screen = Scene.FindByName("Camera"):GetComponent("Camera"):WorldToScreen(vector.create(0, 0, -5))
return math.abs(screen.x - 399.5) < 0.0001 and math.abs(screen.y - 299.5) < 0.0001)")
				== Json(true));
			session.SetViewSize(320, 180);
			CHECK(session.GetViewSize() == glm::uvec2(320, 180));
			CHECK(evaluate("return Application.GetWindowSize() == vector.create(320, 180, 0)") == Json(true));
			CHECK(evaluate(R"(
local origin, direction = Scene.FindByName("Camera"):GetComponent("Camera"):ScreenToWorldRay(159.5, 89.5)
return vector.magnitude(direction - vector.create(0, 0, -1)) < 0.00001)")
				== Json(true));
		}

		TEST_CASE("PlaySession: view coordinates map a display rectangle linearly onto the view's pixels")
		{
			// A 1920x1080 view shown letterboxed at (400, 80), 800x450 logical units.
			const glm::vec2 minimum(400.0f, 80.0f);
			const glm::vec2 size(800.0f, 450.0f);
			const glm::uvec2 view(1920, 1080);
			CHECK(MapToViewCoordinates({ 400.0f, 80.0f }, minimum, size, view) == glm::vec2(0.0f, 0.0f));
			CHECK(MapToViewCoordinates({ 800.0f, 305.0f }, minimum, size, view) == glm::vec2(960.0f, 540.0f));
			CHECK(MapToViewCoordinates({ 1200.0f, 530.0f }, minimum, size, view) == glm::vec2(1920.0f, 1080.0f));
			// Unclamped: a cursor beyond the image, in the bars or past the window, keeps moving continuously.
			CHECK(MapToViewCoordinates({ 300.0f, 40.0f }, minimum, size, view) == glm::vec2(-240.0f, -96.0f));
			// The Runtime's view fills its window: the mapping is the framebuffer scale.
			CHECK(MapToViewCoordinates({ 10.0f, 20.0f }, glm::vec2(0.0f), { 640.0f, 360.0f }, { 1280, 720 }) == glm::vec2(20.0f, 40.0f));
			CHECK_FALSE(MapToViewCoordinates({ 1.0f, 1.0f }, minimum, { 0.0f, 450.0f }, view).has_value());
			CHECK_FALSE(MapToViewCoordinates({ 1.0f, 1.0f }, minimum, size, { 0, 1080 }).has_value());
			CHECK_FALSE(MapToViewCoordinates({ std::numeric_limits<float>::infinity(), 1.0f }, minimum, size, view).has_value());
			CHECK_FALSE(MapToViewCoordinates({ 1.0f, 1.0f }, { std::numeric_limits<float>::quiet_NaN(), 0.0f }, size, view).has_value());
		}

		TEST_CASE("PlaySession: a fixed view maps an image point through both output mappings to the scene pixel the image shows")
		{
			// M21 (Docs/Decisions/0032-m21-contract.md decision 6): a PixelPerfectCamera's 100x60 reference resolution fills an
			// 800x450 image at scale 7 from offset (50, 15) and a 320x180 view at scale 3 from offset (10, 0), so image point
			// (123.5, 158.5), scene point (10.5, 20.5), is view point (41.5, 61.5); a stretch would give (49.4, 63.4).
			const RenderOutputSettings pixelArt{ .SceneWidth = 100, .SceneHeight = 60, .PixelArt = true };
			const glm::uvec2 image(800, 450);
			const glm::uvec2 view(320, 180);
			CHECK(MapImageToViewCoordinates({ 123.5f, 158.5f }, glm::vec2(0.0f), glm::vec2(image), image, view, pixelArt) == glm::vec2(41.5f, 61.5f));
			// The letterbox stays outside the scene image, unclamped: the image's top-left corner lies 50/7 scene pixels left
			// of the scene and 15/7 above it, which the view shows 150/7 and 45/7 pixels from its own offset (10, 0).
			const std::optional<glm::vec2> corner = MapImageToViewCoordinates({ 0.0f, 0.0f }, glm::vec2(0.0f), glm::vec2(image), image, view, pixelArt);
			REQUIRE(corner.has_value());
			CHECK(corner->x == doctest::Approx(10.0 - 150.0 / 7.0));
			CHECK(corner->y == doctest::Approx(-45.0 / 7.0));
			// The Runtime: window coordinates (logical, half the 1600x900 framebuffer image) reach the image's pixels first.
			CHECK(MapImageToViewCoordinates({ 400.0f, 225.0f }, glm::vec2(0.0f), { 800.0f, 450.0f }, { 1600, 900 }, view, pixelArt)
				== glm::vec2(160.0f, 90.0f));
			// Without a PixelPerfectCamera (default settings: each scene is its view), and for an image of the view's size,
			// it is MapToViewCoordinates's stretch exactly.
			for (const glm::vec2 point : { glm::vec2(123.5f, 158.5f), glm::vec2(-7.25f, 1000.0f), glm::vec2(0.1f, 0.3f) })
			{
				CHECK(MapImageToViewCoordinates(point, glm::vec2(0.0f), glm::vec2(image), image, view, RenderOutputSettings{})
					== MapToViewCoordinates(point, glm::vec2(0.0f), glm::vec2(image), view));
				CHECK(MapImageToViewCoordinates(point, glm::vec2(0.0f), { 640.0f, 360.0f }, view, view, pixelArt)
					== MapToViewCoordinates(point, glm::vec2(0.0f), { 640.0f, 360.0f }, view));
			}
			CHECK_FALSE(MapImageToViewCoordinates({ 1.0f, 1.0f }, glm::vec2(0.0f), glm::vec2(image), { 0, 450 }, view, pixelArt).has_value());
			CHECK_FALSE(MapImageToViewCoordinates({ 1.0f, 1.0f }, glm::vec2(0.0f), glm::vec2(image), image, { 320, 0 }, pixelArt).has_value());
			CHECK_FALSE(MapImageToViewCoordinates({ std::numeric_limits<float>::infinity(), 1.0f }, glm::vec2(0.0f), glm::vec2(image), image, view, pixelArt)
					.has_value());
			CHECK_FALSE(MapImageToViewCoordinates({ 1.0f, 1.0f }, glm::vec2(0.0f), { 0.0f, 450.0f }, image, view, pixelArt).has_value());
		}

		TEST_CASE("PlaySession: a deterministic session's view is the project's window size in logical pixels")
		{
			CHECK(GetDeterministicViewSize(WindowSettings{}) == glm::uvec2(1600, 900));
			CHECK(GetDeterministicViewSize(WindowSettings{ .Width = 320, .Height = 180 }) == glm::uvec2(320, 180));
			CHECK(GetDeterministicViewSize(WindowSettings{ .Width = 0, .Height = 0 }) == glm::uvec2(1, 1));
		}
	}

}
