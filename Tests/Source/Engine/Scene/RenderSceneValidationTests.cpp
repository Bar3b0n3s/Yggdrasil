#include "TestsPCH.h"

#include "Engine/Scene/RenderAnnotations.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <limits>

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderSceneValidation: default ambient and emissive-only scenes avoid false warnings")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Intensity = 0;
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			auto emission = CreateRef<MaterialData>();
			emission->Emissive = { 1, 0, 0 };
			emission->EmissiveStrength = 2;
			assets.Publish(AssetHandle(2000), emission);
			mesh.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Materials.resize(1);
				r.Materials[0].SetHandle(AssetHandle(2000));
			});
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			Entity other = scene.CreateEntity("Unlit");
			other.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			other.SetActive(false);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
		}
		TEST_CASE("RenderSceneValidation: missing maps use fallback and usable maps use SH illumination")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Environment.SetHandle(AssetHandle(2001));
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			environment.Patch<EnvironmentComponent>([](EnvironmentComponent& e)
			{
				e.FallbackColor = glm::vec3(0);
			});
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			auto map = CreateRef<EnvironmentData>();
			map->IrradianceSH9[0] = glm::vec3(1);
			assets.Publish(AssetHandle(2001), map);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			map = CreateRef<EnvironmentData>();
			assets.Publish(AssetHandle(2001), map);
			environment.Patch<EnvironmentComponent>([](EnvironmentComponent& e)
			{
				e.FallbackColor = glm::vec3(1);
			});
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
		}
		TEST_CASE("RenderSceneValidation: only enabled contributing shadowed spots consume the global budget")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Intensity = 0;
			for (int i = 0; i < 9; ++i)
				scene.CreateEntity("Spot").AddComponent<SpotLightComponent>().CastShadows = true;
			Entity invalid = scene.CreateEntity("Invalid");
			invalid.AddComponent<SpotLightComponent>().CastShadows = true;
			invalid.Patch<SpotLightComponent>([](SpotLightComponent& s)
			{
				s.Intensity = std::numeric_limits<float>::quiet_NaN();
			});
			Entity disabled = scene.CreateEntity("Disabled");
			disabled.AddComponent<SpotLightComponent>().CastShadows = true;
			disabled.SetActive(false);
			Entity dark = scene.CreateEntity("Dark");
			dark.AddComponent<SpotLightComponent>().CastShadows = true;
			dark.Patch<SpotLightComponent>([](SpotLightComponent& s)
			{
				s.Range = 0;
			});
			const auto result = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE(result);
			CHECK(result->ShadowedSpotLights == 9);
			CHECK_FALSE(result->NoLighting);
		}
		TEST_CASE("RenderSceneValidation: nonfinite environment and emission return classification errors")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Intensity = std::numeric_limits<float>::infinity();
			const auto invalid = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE_FALSE(invalid);
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
			environment.Patch<EnvironmentComponent>([](EnvironmentComponent& e)
			{
				e.Intensity = 0;
			});
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			mesh.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Materials.resize(1);
				r.Materials[0].SetHandle(AssetHandle(2000));
			});
			auto emission = CreateRef<MaterialData>();
			emission->EmissiveStrength = std::numeric_limits<float>::quiet_NaN();
			assets.Publish(AssetHandle(2000), emission);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets));
		}
	}

}
