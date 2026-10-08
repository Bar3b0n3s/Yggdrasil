#include "TestsPCH.h"

#include "Engine/Scene/RenderExtraction.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"

#include <limits>

// Render extraction (Architecture §5.2, §8.2): the snapshot of one view built from a scene, with the interpolation rule
// shared with TransformSystem's render pose (Docs/Decisions/0012-m7-decisions.md decisions 5 and 6).

namespace Engine {

	namespace {

		Entity CreateCamera(Scene& scene, std::string_view name, bool primary)
		{
			Entity camera = scene.CreateEntity(name);
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = primary });
			return camera;
		}

		void SetTranslation(Entity entity, const glm::vec3& translation)
		{
			entity.Patch<TransformComponent>([translation](TransformComponent& transform)
			{
				transform.Translation = translation;
			});
		}

		// Gives `entity` its current world matrix as its previous pose, as the play session's interpolation snapshot does.
		void SnapshotPreviousPose(Entity entity)
		{
			entity.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = entity.GetComponent<WorldTransformComponent>().Matrix });
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderExtraction: the primary camera is the first effectively enabled one in canonical order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(CreateCamera(scene, "Secondary", false));
			Entity disabled = CreateCamera(scene, "DisabledPrimary", true);
			disabled.SetActive(false);
			const Entity primary = CreateCamera(scene, "Primary", true);
			static_cast<void>(CreateCamera(scene, "LaterPrimary", true));
			CHECK(FindPrimaryCamera(scene) == ConstEntity(primary));

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 640, .Height = 360 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->HasCamera);
			CHECK(snapshot->Camera.Entity == primary.GetUUID());
			CHECK(snapshot->Camera.ViewportWidth == 640);
		}

		TEST_CASE("RenderExtraction: a game view without a primary camera has no camera and is no error")
		{
			Test::SceneTestFixture fixture;
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK_FALSE(snapshot->HasCamera);
			CHECK_FALSE(FindPrimaryCamera(fixture.GetScene()).IsValid());
		}

		TEST_CASE("RenderExtraction: visible meshes and lights of enabled entities are extracted in canonical order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity first = scene.CreateEntity("First");
			first.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			Entity hidden = scene.CreateEntity("Hidden");
			MeshRendererComponent& invisible = hidden.AddComponent<MeshRendererComponent>();
			invisible.Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			invisible.Visible = false;
			Entity disabled = scene.CreateEntity("Disabled");
			disabled.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::SphereMesh);
			disabled.SetActive(false);
			Entity sun = scene.CreateEntity("Sun");
			sun.AddComponent<DirectionalLightComponent>();
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 1);
			CHECK(snapshot->Meshes[0].Entity == first.GetUUID());
			REQUIRE(snapshot->Lights.size() == 1);
			CHECK(snapshot->Lights[0].Type == RenderLightType::Directional);
			CHECK(Test::ApproxEqual(snapshot->Lights[0].Direction, glm::vec3(0.0f, 0.0f, -1.0f), 1e-6f));
		}

		TEST_CASE("RenderExtraction: meshes keep their materials and flags, and children of disabled entities are left out")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity parent = scene.CreateEntity("Parent");
			SetTranslation(parent, glm::vec3(1.0f, 2.0f, 3.0f));
			Entity child = scene.CreateEntity("Child", parent);
			MeshRendererComponent& renderer = child.AddComponent<MeshRendererComponent>();
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::SphereMesh);
			renderer.Materials = { TypedAssetHandle<AssetType::Material>(BuiltinAssetHandles::ErrorMaterial), {} };
			renderer.CastShadows = false;
			Entity noMesh = scene.CreateEntity("NoMesh");
			static_cast<void>(noMesh.AddComponent<MeshRendererComponent>());
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 1);
			const MeshDrawItem& item = snapshot->Meshes[0];
			CHECK(item.Entity == child.GetUUID());
			CHECK(item.Mesh == BuiltinAssetHandles::SphereMesh);
			REQUIRE(item.Materials.size() == 2);
			CHECK(item.Materials[0] == BuiltinAssetHandles::ErrorMaterial);
			CHECK_FALSE(item.Materials[1].IsValid());
			CHECK_FALSE(item.CastShadows);
			CHECK(item.ReceiveShadows);
			CHECK(Test::ApproxEqual(glm::vec3(item.World[3]), glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f));

			parent.SetActive(false);
			const Result<RenderSnapshot> withoutChild = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(withoutChild.has_value(), withoutChild.error().ToString());
			CHECK(withoutChild->Meshes.empty());
		}

		TEST_CASE("RenderExtraction: lights take their pose from the rendered world matrix and their fields from the component")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity sun = scene.CreateEntity("Sun");
			sun.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Color = glm::vec3(1.0f, 0.5f, 0.25f), .Intensity = 2.0f });
			// Rotated 90 degrees about +Y: local -Z points along world -X.
			sun.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
			});
			Entity lamp = scene.CreateEntity("Lamp");
			SetTranslation(lamp, glm::vec3(0.0f, 4.0f, 0.0f));
			lamp.AddComponent<PointLightComponent>(PointLightComponent{ .Range = 7.0f });
			Entity spot = scene.CreateEntity("Spot");
			spot.AddComponent<SpotLightComponent>(SpotLightComponent{ .InnerConeAngle = 10.0f, .OuterConeAngle = 25.0f, .CastShadows = true });
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Lights.size() == 3);
			const LightData& directional = snapshot->Lights[0];
			CHECK(directional.Type == RenderLightType::Directional);
			CHECK(directional.Entity == sun.GetUUID());
			CHECK(directional.Intensity == 2.0f);
			CHECK(Test::ApproxEqual(directional.Color, glm::vec3(1.0f, 0.5f, 0.25f), 1e-6f));
			CHECK(Test::ApproxEqual(directional.Direction, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-5f));
			const LightData& point = snapshot->Lights[1];
			CHECK(point.Type == RenderLightType::Point);
			CHECK(point.Range == 7.0f);
			CHECK(Test::ApproxEqual(point.Position, glm::vec3(0.0f, 4.0f, 0.0f), 1e-6f));
			const LightData& cone = snapshot->Lights[2];
			CHECK(cone.Type == RenderLightType::Spot);
			CHECK(cone.InnerConeAngle == 10.0f);
			CHECK(cone.OuterConeAngle == 25.0f);
			CHECK(cone.CastShadows);
		}

		TEST_CASE("RenderExtraction: the environment and post-process settings are the first enabled components, or the defaults")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Result<RenderSnapshot> defaults = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(defaults.has_value(), defaults.error().ToString());
			CHECK(defaults->Environment.FallbackColor == RenderEnvironment{}.FallbackColor);
			CHECK(defaults->Post.Tonemap == RenderTonemapper::AgX);

			Entity disabled = scene.CreateEntity("DisabledLook");
			disabled.AddComponent<PostProcessComponent>(PostProcessComponent{ .ExposureEV = 5.0f });
			disabled.SetActive(false);
			Entity look = scene.CreateEntity("Look");
			look.AddComponent<EnvironmentComponent>(EnvironmentComponent{ .Environment = {}, .Intensity = 0.5f, .FallbackColor = glm::vec3(0.1f, 0.2f, 0.3f) });
			look.AddComponent<PostProcessComponent>(PostProcessComponent{ .ExposureEV = -1.0f, .Tonemap = Tonemapper::Linear });

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Environment.Intensity == 0.5f);
			CHECK(snapshot->Environment.FallbackColor == glm::vec3(0.1f, 0.2f, 0.3f));
			CHECK(snapshot->Post.ExposureEV == -1.0f);
			CHECK(snapshot->Post.Tonemap == RenderTonemapper::Linear);
		}

		TEST_CASE("RenderExtraction: a camera's view matrix is the inverse of its pose without scale, and its projection reverse-Z")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity camera = CreateCamera(scene, "Camera", true);
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 1.0f, 5.0f);
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
				transform.Scale = glm::vec3(2.0f);
			});
			camera.Patch<CameraComponent>([](CameraComponent& component)
			{
				component.ClearColor = glm::vec3(0.5f, 0.25f, 0.125f);
				component.Clear = ClearMode::Color;
			});
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 200, .Height = 100 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			const CameraData& data = snapshot->Camera;
			CHECK(Test::ApproxEqual(data.Position, glm::vec3(0.0f, 1.0f, 5.0f), 1e-6f));
			// The camera's position maps to the view-space origin, and its -Z axis (world -X here) to view -Z, at unit scale.
			CHECK(Test::ApproxEqual(glm::vec3(data.View * glm::vec4(data.Position, 1.0f)), glm::vec3(0.0f), 1e-5f));
			CHECK(Test::ApproxEqual(glm::vec3(data.View * glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f)), glm::vec3(0.0f, 0.0f, -1.0f), 1e-5f));
			CHECK(Test::ApproxEqual(data.Projection, ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 1000.0f, 200, 100), 1e-6f));
			CHECK(data.ClearColor == glm::vec3(0.5f, 0.25f, 0.125f));
			CHECK_FALSE(data.ClearToSkybox);
		}

		TEST_CASE("RenderExtraction: entity and explicit cameras render through their own pose, and invalid ones are errors")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity camera = CreateCamera(scene, "Side", false);
			const Entity notCamera = scene.CreateEntity("NotACamera");

			RenderExtractionRequest request{ .Camera = RenderCameraSource::Entity, .CameraEntity = camera.GetUUID(), .Width = 32, .Height = 32 };
			const Result<RenderSnapshot> side = ExtractRenderSnapshot(scene, request);
			REQUIRE_MESSAGE(side.has_value(), side.error().ToString());
			CHECK(side->Camera.Entity == camera.GetUUID());

			request.CameraEntity = notCamera.GetUUID();
			const Result<RenderSnapshot> wrong = ExtractRenderSnapshot(scene, request);
			REQUIRE_FALSE(wrong.has_value());
			CHECK(wrong.error().GetCode() == ErrorCode::InvalidArgument);

			request.CameraEntity = UUID(0x1234567890abcdefULL);
			const Result<RenderSnapshot> missing = ExtractRenderSnapshot(scene, request);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);

			const Result<RenderSnapshot> editor = ExtractRenderSnapshot(scene, { .Camera = RenderCameraSource::Explicit, .Width = 32, .Height = 32 });
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			CHECK(editor->HasCamera);
			CHECK(editor->Camera.Entity == UUID());
			CHECK(Test::ApproxEqual(editor->Camera.Position, ExplicitRenderCamera{}.Position, 1e-6f));

			const Result<RenderSnapshot> empty = ExtractRenderSnapshot(scene, { .Width = 0, .Height = 32 });
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("RenderExtraction: an explicit camera looks at its target, also straight down, and may not sit on it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			RenderExtractionRequest request{ .Camera = RenderCameraSource::Explicit, .Width = 16, .Height = 16 };
			const Result<RenderSnapshot> standard = ExtractRenderSnapshot(scene, request);
			REQUIRE_MESSAGE(standard.has_value(), standard.error().ToString());
			// The default camera at (0, 3, 10) looks at the origin: the origin is straight ahead, on the view's -Z axis.
			const glm::vec3 origin(standard->Camera.View * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
			CHECK(Test::ApproxEqual(glm::vec2(origin), glm::vec2(0.0f), 1e-5f));
			CHECK(origin.z < 0.0f);

			// Looking straight down: the view's up is world -Z, and the target is still ahead.
			request.ExplicitCamera.Position = glm::vec3(0.0f, 10.0f, 0.0f);
			const Result<RenderSnapshot> top = ExtractRenderSnapshot(scene, request);
			REQUIRE_MESSAGE(top.has_value(), top.error().ToString());
			CHECK(Test::ApproxEqual(glm::vec3(top->Camera.View * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f), 1e-5f));
			CHECK(Test::ApproxEqual(glm::vec3(top->Camera.View * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)), glm::vec3(0.0f, 0.0f, -10.0f), 1e-5f));

			request.ExplicitCamera.Target = request.ExplicitCamera.Position;
			const Result<RenderSnapshot> degenerate = ExtractRenderSnapshot(scene, request);
			REQUIRE_FALSE(degenerate.has_value());
			CHECK(degenerate.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("RenderExtraction: an alpha outside [0, 1] or not finite is InvalidArgument")
		{
			Test::SceneTestFixture fixture;
			for (const float alpha : { -0.25f, 1.5f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
			{
				CAPTURE(alpha);
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 8, .Height = 8, .Alpha = alpha });
				REQUIRE_FALSE(snapshot.has_value());
				CHECK(snapshot.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 8, .Height = 8, .Alpha = 0.75f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Alpha == 0.75f);
		}

		TEST_CASE("RenderExtraction: runtime scenes interpolate between the previous and the current pose")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity moving = scene.CreateEntity("Moving");
			Entity child = scene.CreateEntity("Child", moving);
			TransformSystem::Update(scene);
			SnapshotPreviousPose(moving);
			SnapshotPreviousPose(child);
			SetTranslation(moving, glm::vec3(4.0f, 0.0f, 0.0f));
			TransformSystem::Update(scene);

			CHECK(ComputeRenderedWorldMatrix(moving, 0.25f)[3].x == doctest::Approx(1.0f));
			CHECK(ComputeRenderedWorldMatrix(child, 0.25f)[3].x == doctest::Approx(1.0f));

			// A reset tag on the parent stops interpolation for its subtree.
			moving.AddComponent<InterpolationResetTag>();
			CHECK(ComputeRenderedWorldMatrix(moving, 0.25f)[3].x == doctest::Approx(4.0f));
			CHECK(ComputeRenderedWorldMatrix(child, 0.25f)[3].x == doctest::Approx(4.0f));
		}

		TEST_CASE("RenderExtraction: edit scenes and entities without a previous pose render at their current pose")
		{
			// An edit scene with a previous pose never interpolates.
			Test::SceneTestFixture edit;
			Entity edited = edit.GetScene().CreateEntity("Edited");
			TransformSystem::Update(edit.GetScene());
			SnapshotPreviousPose(edited);
			SetTranslation(edited, glm::vec3(4.0f, 0.0f, 0.0f));
			TransformSystem::Update(edit.GetScene());
			CHECK(ComputeRenderedWorldMatrix(edited, 0.25f)[3].x == doctest::Approx(4.0f));

			// A runtime entity created since the last snapshot has no previous pose; one without a world matrix yet renders at
			// its chain-walked pose.
			Test::SceneTestFixture runtime(1, true);
			Entity created = runtime.GetScene().CreateEntity("Created");
			SetTranslation(created, glm::vec3(0.0f, 2.0f, 0.0f));
			CHECK_FALSE(created.HasComponent<WorldTransformComponent>());
			CHECK(ComputeRenderedWorldMatrix(created, 0.5f)[3].y == doctest::Approx(2.0f));
			TransformSystem::Update(runtime.GetScene());
			CHECK(ComputeRenderedWorldMatrix(created, 0.5f)[3].y == doctest::Approx(2.0f));

			// Extraction uses the request's alpha for meshes and cameras alike.
			created.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			SnapshotPreviousPose(created);
			SetTranslation(created, glm::vec3(0.0f, 6.0f, 0.0f));
			TransformSystem::Update(runtime.GetScene());
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(runtime.GetScene(), { .Width = 8, .Height = 8, .Alpha = 0.5f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 1);
			CHECK(snapshot->Meshes[0].World[3].y == doctest::Approx(4.0f));
		}

		TEST_CASE("RenderExtraction: the rendered pose is TransformSystem's render position and rotation")
		{
			// One rule for the image and for scripts' Transform.RenderPosition/RenderRotation (RenderExtraction.h), the
			// fallback for a previous pose that does not decompose included.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity turning = scene.CreateEntity("Turning");
			Entity shrunk = scene.CreateEntity("Shrunk");
			TransformSystem::Update(scene);
			SnapshotPreviousPose(turning);
			// A previous pose with a zero scale cannot be decomposed: the translation still interpolates, the rotation is the
			// current one.
			shrunk.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = glm::mat4(0.0f) });
			for (Entity entity : { turning, shrunk })
			{
				entity.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(2.0f, 4.0f, -6.0f);
					transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
				});
			}
			TransformSystem::Update(scene);
			scene.SetInterpolationAlpha(0.5f);

			for (const Entity entity : { turning, shrunk })
			{
				INFO(std::string(entity.GetName()));
				const glm::mat4 rendered = ComputeRenderedWorldMatrix(entity, 0.5f);
				CHECK(Test::ApproxEqual(glm::vec3(rendered[3]), TransformSystem::GetRenderPosition(entity), 1e-5f));
				const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(rendered);
				REQUIRE(decomposed.has_value());
				// q and -q are the same rotation: compare what they do to a vector.
				const glm::vec3 axis(1.0f, 0.0f, 0.0f);
				CHECK(Test::ApproxEqual(decomposed->Rotation * axis, TransformSystem::GetRenderRotation(entity) * axis, 1e-5f));
			}
			CHECK(Test::ApproxEqual(glm::vec3(ComputeRenderedWorldMatrix(shrunk, 0.5f)[3]), glm::vec3(1.0f, 2.0f, -3.0f), 1e-5f));
		}

		TEST_CASE("RenderExtraction: interpolated scale lerps between the poses")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity growing = scene.CreateEntity("Growing");
			TransformSystem::Update(scene);
			SnapshotPreviousPose(growing);
			growing.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(3.0f);
			});
			TransformSystem::Update(scene);
			const Result<TransformDecomposition> halfway = TransformSystem::DecomposeMatrix(ComputeRenderedWorldMatrix(growing, 0.5f));
			REQUIRE(halfway.has_value());
			CHECK(Test::ApproxEqual(halfway->Scale, glm::vec3(2.0f), 1e-5f));
			// Alpha 0 is the previous pose and alpha 1 the current one.
			CHECK(Test::ApproxEqual(ComputeRenderedWorldMatrix(growing, 0.0f), glm::mat4(1.0f), 1e-6f));
			CHECK(ComputeRenderedWorldMatrix(growing, 1.0f) == growing.GetComponent<WorldTransformComponent>().Matrix);
		}

		// M8 (Docs/Decisions/0013-m8-decisions.md decision 11): texts.

		TEST_CASE("RenderExtraction: texts of enabled entities are extracted in canonical order with every field")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity title = scene.CreateEntity("Title");
			title.AddComponent<TextComponent>(TextComponent{ .Text = "Score: 10",
				.Font = TypedAssetHandle<AssetType::Font>(AssetHandle(0x5555)),
				.Size = 48.0f,
				.Color = glm::vec4(1.0f, 0.5f, 0.25f, 0.75f),
				.Space = TextSpace::Screen,
				.Anchor = glm::vec2(0.5f, 0.0f),
				.Pivot = glm::vec2(0.5f, 0.0f),
				.Offset = glm::vec2(0.0f, 24.0f),
				.Alignment = TextAlignment::Left,
				.Billboard = false });
			Entity empty = scene.CreateEntity("Empty");
			empty.AddComponent<TextComponent>(); // no text: not extracted
			Entity hidden = scene.CreateEntity("Hidden");
			hidden.AddComponent<TextComponent>().Text = "Hidden";
			hidden.SetActive(false);
			Entity label = scene.CreateEntity("Label");
			label.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
			TextComponent& labelText = label.AddComponent<TextComponent>();
			labelText.Text = "World";
			labelText.Space = TextSpace::World;
			labelText.Alignment = TextAlignment::Right;
			labelText.Billboard = true;
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Texts.size() == 2);
			const TextItem& screen = snapshot->Texts[0];
			CHECK(screen.Entity == title.GetUUID());
			CHECK(screen.Text == "Score: 10");
			CHECK(screen.Font == AssetHandle(0x5555));
			CHECK(screen.Size == 48.0f);
			CHECK(screen.Color == glm::vec4(1.0f, 0.5f, 0.25f, 0.75f));
			CHECK(screen.Space == RenderTextSpace::Screen);
			CHECK(screen.Anchor == glm::vec2(0.5f, 0.0f));
			CHECK(screen.Pivot == glm::vec2(0.5f, 0.0f));
			CHECK(screen.Offset == glm::vec2(0.0f, 24.0f));
			CHECK(screen.Alignment == RenderTextAlignment::Left);
			const TextItem& world = snapshot->Texts[1];
			CHECK(world.Entity == label.GetUUID());
			CHECK(world.Space == RenderTextSpace::World);
			CHECK(world.Alignment == RenderTextAlignment::Right);
			CHECK(world.Billboard);
			CHECK(Test::ApproxEqual(glm::vec3(world.World[3]), glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f));
			// Extraction adds no debug primitives and leaves the debug view Lit.
			CHECK(snapshot->DebugDraw.IsEmpty());
			CHECK(snapshot->DebugView == RenderDebugView::Lit);
		}

		TEST_CASE("RenderExtraction: world texts in a play scene render at the interpolated pose")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity label = scene.CreateEntity("Label");
			TextComponent& moving = label.AddComponent<TextComponent>();
			moving.Text = "Moving";
			moving.Space = TextSpace::World;
			TransformSystem::Update(scene);
			SnapshotPreviousPose(label);
			SetTranslation(label, glm::vec3(4.0f, 0.0f, 0.0f));
			TransformSystem::Update(scene);
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .Alpha = 0.25f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Texts.size() == 1);
			CHECK(Test::ApproxEqual(glm::vec3(snapshot->Texts[0].World[3]), glm::vec3(1.0f, 0.0f, 0.0f), 1e-6f));
		}
	}

}
