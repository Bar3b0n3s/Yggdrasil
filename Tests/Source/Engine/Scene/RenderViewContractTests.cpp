#include "TestsPCH.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <vector>

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderExtraction: directional shadow fields and project quality settings reach the snapshot")
		{
			Test::SceneTestFixture fixture;
			Entity sun = fixture.GetScene().CreateEntity("Sun");
			DirectionalLightComponent light;
			light.ShadowDistance = 65.0f;
			light.CascadeCount = 3;
			light.CascadeSplitLambda = 0.25f;
			light.DepthBias = 0.75f;
			light.NormalBias = 1.25f;
			sun.AddComponent<DirectionalLightComponent>(light);
			RenderExtractionRequest request;
			request.Quality = { .ShadowMapSize = 1024, .SsaoHalfResolution = true };
			const auto snapshot = ExtractRenderSnapshot(fixture.GetScene(), request);
			REQUIRE(snapshot.has_value());
			REQUIRE(snapshot->Lights.size() == 1);
			CHECK(snapshot->Lights[0].ShadowDistance == light.ShadowDistance);
			CHECK(snapshot->Lights[0].CascadeCount == light.CascadeCount);
			CHECK(snapshot->Lights[0].CascadeSplitLambda == light.CascadeSplitLambda);
			CHECK(snapshot->Lights[0].DepthBias == light.DepthBias);
			CHECK(snapshot->Lights[0].NormalBias == light.NormalBias);
			CHECK(snapshot->Quality.ShadowMapSize == 1024);
			CHECK(snapshot->Quality.SsaoHalfResolution);
		}

		TEST_CASE("RenderExtraction: PickTable uses canonical UUIDs before culling")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			for (uint64_t value : { 5u, 2u, 4u, 1u, 3u })
			{
				Entity entity = scene.CreateEntityWithID(UUID(value), "Mesh");
				entity.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
				if (value == 5)
					entity.GetComponent<TransformComponent>().Translation = { 9000.0f, 0.0f, 0.0f };
				if (value == 2)
					entity.SetActive(false);
				if (value == 3)
					entity.GetComponent<MeshRendererComponent>().Visible = false;
				if (value == 4)
					scene.DestroyEntity(entity);
			}
			TransformSystem::Update(scene);
			const auto snapshot = ExtractRenderSnapshot(scene, {});
			REQUIRE(snapshot.has_value());
			// Architecture §5.1: roots retain stored order; the offscreen root precedes the visible root.
			CHECK(snapshot->PickTable == std::vector<UUID>{ UUID(5), UUID(1) });
			REQUIRE(snapshot->Meshes.size() == 2);
			for (size_t index = 0; index < snapshot->Meshes.size(); ++index)
			{
				CHECK(snapshot->Meshes[index].PickId == index + 1);
				CHECK(snapshot->Meshes[index].Entity == snapshot->PickTable[index]);
			}
		}

		TEST_CASE("RenderExtraction: selected UUIDs and overlay flags are copied by value")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(fixture.GetScene().CreateEntityWithID(UUID(1), "First"));
			static_cast<void>(fixture.GetScene().CreateEntityWithID(UUID(2), "Second"));
			RenderExtractionRequest request;
			request.Flags = RenderViewFlags::Picking | RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
			request.SelectedEntities = { UUID(2), UUID(1), UUID(2), UUID(999) };
			request.Annotations = { .Labels = RenderAnnotationLabels::Explicit, .LabelEntities = { UUID(2), UUID(999), UUID(2) }, .Bounds = true, .Axes = true };
			const auto snapshot = ExtractRenderSnapshot(fixture.GetScene(), request);
			REQUIRE(snapshot.has_value());
			CHECK(snapshot->Flags == request.Flags);
			request.SelectedEntities.clear();
			request.Annotations.LabelEntities.clear();
			CHECK(snapshot->SelectedEntities == std::vector<UUID>{ UUID(1), UUID(2) });
			CHECK(snapshot->Annotations.LabelEntities == std::vector<UUID>{ UUID(2) });
			CHECK(snapshot->Annotations.Bounds);
			CHECK(snapshot->Annotations.Axes);
		}

		TEST_CASE("RenderExtraction: invalid view flags annotations and shadow sizes are rejected")
		{
			Test::SceneTestFixture fixture;
			for (uint32_t size : { 0u, 255u, 257u, 16384u })
			{
				RenderExtractionRequest request;
				request.Quality.ShadowMapSize = size;
				const auto result = ExtractRenderSnapshot(fixture.GetScene(), request);
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
			}
			RenderExtractionRequest request;
			request.Flags = static_cast<RenderViewFlags>(1u << 31);
			CHECK_FALSE(ExtractRenderSnapshot(fixture.GetScene(), request).has_value());
			request.Flags = RenderViewFlags::None;
			request.Annotations.Labels = static_cast<RenderAnnotationLabels>(255);
			CHECK_FALSE(ExtractRenderSnapshot(fixture.GetScene(), request).has_value());
			request.Annotations.Labels = RenderAnnotationLabels::All;
			request.Annotations.LabelEntities = { UUID(1) };
			CHECK_FALSE(ExtractRenderSnapshot(fixture.GetScene(), request).has_value());
		}
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderValidation: lighting warnings respect fallback environment and emissive materials")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().FallbackColor = glm::vec3(0.0f);
			const auto empty = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE(empty.has_value());
			CHECK_FALSE(empty->NoLighting);
			Entity mesh = scene.CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			const auto dark = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE(dark.has_value());
			CHECK(dark->NoLighting);
			auto material = CreateRef<MaterialData>();
			material->Emissive = glm::vec3(2.0f, 0.0f, 0.0f);
			material->EmissiveStrength = 1.0f;
			assets.Publish(AssetHandle(123), material);
			mesh.Patch<MeshRendererComponent>([](MeshRendererComponent& component)
			{
				component.Materials.emplace_back(AssetHandle(123));
			});
			const auto emissive = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE(emissive.has_value());
			CHECK_FALSE(emissive->NoLighting);
			Entity other = scene.CreateEntity("OtherMesh");
			other.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			const auto unlitOther = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE(unlitOther.has_value());
			CHECK(unlitOther->NoLighting);
			environment.Patch<EnvironmentComponent>([](EnvironmentComponent& component)
			{
				component.FallbackColor = glm::vec3(0.1f);
			});
			const auto ambient = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE(ambient.has_value());
			CHECK_FALSE(ambient->NoLighting);
		}
	}

}
