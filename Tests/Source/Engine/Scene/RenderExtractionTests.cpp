#include "TestsPCH.h"

#include "Engine/Scene/RenderExtraction.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"

// Render extraction (Architecture §5.2, §8.2). Skipped skeletons of the M7 contract (Docs/Decisions/0012-m7-decisions.md
// decision 6): stream B implements extraction and removes the skips.

namespace Engine {

	namespace {

		Entity CreateCamera(Scene& scene, std::string_view name, bool primary)
		{
			Entity camera = scene.CreateEntity(name);
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = primary });
			return camera;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderExtraction: the primary camera is the first effectively enabled one in canonical order" * doctest::skip(true))
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

		TEST_CASE("RenderExtraction: a game view without a primary camera has no camera and is no error" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK_FALSE(snapshot->HasCamera);
		}

		TEST_CASE("RenderExtraction: visible meshes and lights of enabled entities are extracted in canonical order" * doctest::skip(true))
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

		TEST_CASE("RenderExtraction: entity and explicit cameras render through their own pose, and invalid ones are errors"
			* doctest::skip(true))
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

		TEST_CASE("RenderExtraction: runtime scenes interpolate between the previous and the current pose" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity moving = scene.CreateEntity("Moving");
			Entity child = scene.CreateEntity("Child", moving);
			TransformSystem::Update(scene);
			moving.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = moving.GetComponent<WorldTransformComponent>().Matrix });
			child.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = child.GetComponent<WorldTransformComponent>().Matrix });
			moving.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(4.0f, 0.0f, 0.0f);
			});
			TransformSystem::Update(scene);

			CHECK(ComputeRenderedWorldMatrix(moving, 0.25f)[3].x == doctest::Approx(1.0f));
			CHECK(ComputeRenderedWorldMatrix(child, 0.25f)[3].x == doctest::Approx(1.0f));

			// A reset tag on the parent stops interpolation for its subtree.
			moving.AddComponent<InterpolationResetTag>();
			CHECK(ComputeRenderedWorldMatrix(moving, 0.25f)[3].x == doctest::Approx(4.0f));
			CHECK(ComputeRenderedWorldMatrix(child, 0.25f)[3].x == doctest::Approx(4.0f));
		}

		TEST_CASE("RenderExtraction: the rendered pose is TransformSystem's render position and rotation" * doctest::skip(true))
		{
			// One rule for the image and for scripts' Transform.RenderPosition/RenderRotation (RenderExtraction.h), the
			// fallback for a previous pose that does not decompose included.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity turning = scene.CreateEntity("Turning");
			Entity shrunk = scene.CreateEntity("Shrunk");
			TransformSystem::Update(scene);
			turning.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = turning.GetComponent<WorldTransformComponent>().Matrix });
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
	}

}
