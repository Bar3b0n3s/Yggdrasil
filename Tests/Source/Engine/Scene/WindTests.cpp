#include "TestsPCH.h"

#include "Engine/Scene/Wind.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RenderDistanceComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/WindComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

// The scene's wind (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3F): SampleWind in double
// precision with IEEE-exact operations, floor and sqrt, so its bits are committed and the Debug and Release unit runs both
// check them (decision 6), and FindSceneWind. Render extraction's case of M46's data (a MeshRenderer's RenderDistance and
// the scene's Wind) is here too: RenderExtractionTests.cpp reached MSVC's section limit at phase 8's landing (C1128 in
// Debug), so each later milestone's extraction case lives in its own unit's file.

namespace Engine {

	namespace {

		// A gale from the south-west with every field away from its default.
		WindComponent MakeGale()
		{
			WindComponent wind;
			wind.Direction = glm::vec3(3.0f, 7.0f, 4.0f);
			wind.Strength = 18.0f;
			wind.GustStrength = 6.5f;
			wind.GustFrequency = 0.35f;
			wind.Turbulence = 0.6f;
			return wind;
		}

		std::string Bits(const glm::dvec3& value)
		{
			return std::format("{:#018x} {:#018x} {:#018x}", std::bit_cast<uint64_t>(value.x), std::bit_cast<uint64_t>(value.y),
				std::bit_cast<uint64_t>(value.z));
		}

		// A MeshRenderer drawing `mesh` with its default materials.
		MeshRendererComponent MakeMeshRenderer(AssetHandle mesh)
		{
			MeshRendererComponent renderer;
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
			return renderer;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Wind: Sample gives the committed velocity at fixed points and times")
		{
			const WindComponent gale = MakeGale();
			// The direction is the horizontal part normalized: (3, 0, 4) / 5.
			CHECK(GetWindDirection(gale) == glm::dvec3(0.6, 0.0, 0.8));
			CHECK(GetWindGustWavelength(gale) == static_cast<double>(18.0f) / static_cast<double>(0.35f));
			// The gust wave: 0 at whole numbers, 1 halfway, the cubic's 0.15625 a quarter of the way at 0.25.
			CHECK(WindGustWave(0.0) == 0.0);
			CHECK(WindGustWave(3.5) == 1.0);
			CHECK(WindGustWave(-0.75) == 0.5);
			CHECK(WindGustWave(0.125) == 0.15625);

			struct Sample
			{
				glm::dvec3 Position{ 0.0 };
				double Time = 0.0;
				std::string_view Expected;
			};
			const std::array<Sample, 3> samples = { {
				{ glm::dvec3(0.0), 0.0, "0x4025999999999999 0x0000000000000000 0x402ccccccccccccd" },
				{ glm::dvec3(12.5, 3.0, -40.25), 7.25, "0x4031293010885a54 0x0000000000000000 0x40319b7fd01ffa12" },
				{ glm::dvec3(-310.0, 0.0, 95.75), 1234.5, "0x40270a29f9a6645c 0x0000000000000000 0x4030ec13b0988f43" },
			} };
			for (const Sample& sample : samples)
			{
				const glm::dvec3 velocity = SampleWind(gale, sample.Position, sample.Time);
				CAPTURE(sample.Time);
				CHECK(Bits(velocity) == sample.Expected);
				// No vertical part, and the speed stays within the steady speed plus the gusts and their turn.
				CHECK(velocity.y == 0.0);
				CHECK(glm::length(velocity) <= 18.0 + 6.5 * 1.7);
			}

			// A calm wind is still, and a wind without gusts blows at its Strength along its direction everywhere.
			WindComponent calm = gale;
			calm.Strength = 0.0f;
			calm.GustStrength = 0.0f;
			CHECK(SampleWind(calm, glm::dvec3(5.0, 1.0, -3.0), 42.0) == glm::dvec3(0.0));
			WindComponent steady = gale;
			steady.GustStrength = 0.0f;
			CHECK(SampleWind(steady, glm::dvec3(-8.0, 0.0, 300.0), 9.5) == glm::dvec3(0.6, 0.0, 0.8) * static_cast<double>(18.0f));
			// Gusts travel with the wind: a point one wavelength downwind sees the same gust at the same time.
			const double wavelength = GetWindGustWavelength(gale);
			const glm::dvec3 upwind = SampleWind(gale, glm::dvec3(0.0), 3.0);
			const glm::dvec3 downwind = SampleWind(gale, glm::dvec3(0.6, 0.0, 0.8) * wavelength * 4.0, 3.0);
			CHECK(glm::length(upwind - downwind) < 1e-9);
			// A Direction without a horizontal part, which the component refuses, blows along +X.
			WindComponent upward = gale;
			upward.Direction = glm::vec3(0.0f, 1.0f, 0.0f);
			CHECK(GetWindDirection(upward) == glm::dvec3(1.0, 0.0, 0.0));
		}

		TEST_CASE("Wind: FindSceneWind returns the scene's effectively enabled Wind")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			CHECK(FindSceneWind(scene) == nullptr);
			const Entity first = scene.CreateEntity("First");
			first.AddComponent<WindComponent>(MakeGale());
			REQUIRE(FindSceneWind(scene) != nullptr);
			CHECK(FindSceneWind(scene)->Strength == 18.0f);
			// A Wind under a disabled hierarchy does not blow.
			first.SetActive(false);
			REQUIRE(first.HasComponent<HierarchyDisabledTag>());
			CHECK(FindSceneWind(scene) == nullptr);

			// W46.1's review: with several Winds (a hand-edited file, SCENE_DUPLICATE_UNIQUE_COMPONENT), the effectively enabled
			// one on the lowest entity UUID blows, whatever order the entities were created or re-created in.
			const auto addWind = [&scene](uint64_t id, std::string_view name, float strength)
			{
				Entity entity = scene.CreateEntityWithID(UUID(id), std::string(name));
				WindComponent wind = MakeGale();
				wind.Strength = strength;
				entity.AddComponent<WindComponent>(wind);
				return entity;
			};
			Entity late = addWind(0x20, "Late", 4.0f);
			Entity early = addWind(0x10, "Early", 2.0f);
			REQUIRE(FindSceneWind(scene) != nullptr);
			CHECK(FindSceneWind(scene)->Strength == 2.0f);
			early.SetActive(false);
			REQUIRE(FindSceneWind(scene) != nullptr);
			CHECK(FindSceneWind(scene)->Strength == 4.0f);
			early.SetActive(true);
			scene.DestroyEntity(late);
			late = addWind(0x20, "Late", 4.0f);
			REQUIRE(FindSceneWind(scene) != nullptr);
			CHECK(FindSceneWind(scene)->Strength == 2.0f);
			// The Winds' pool order puts 0x20 before 0x10 now (destroyed and re-added after it); the choice does not follow it.
			early.RemoveComponent<WindComponent>();
			WindComponent again = MakeGale();
			again.Strength = 2.0f;
			early.AddComponent<WindComponent>(again);
			REQUIRE(FindSceneWind(scene) != nullptr);
			CHECK(FindSceneWind(scene)->Strength == 2.0f);
		}

		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3C and 3G): a MeshRenderer's RenderDistance
		// rides on its item (the defaults without one), and the effectively enabled Wind reaches the snapshot with the request's
		// SimulationTime as its Time (none without one).
		TEST_CASE("RenderExtraction: a mesh's RenderDistance and the scene's Wind reach the snapshot at the request's time")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity plain = scene.CreateEntity("Plain");
			plain.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::CubeMesh));
			Entity distant = scene.CreateEntity("Distant");
			distant.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::CubeMesh));
			distant.AddComponent<RenderDistanceComponent>(RenderDistanceComponent{ .CullDistance = 250.0f, .LodBias = 2.5f, .ShadowLodOffset = 3, .FadeWidth = 0.2f });
			TransformSystem::Update(scene);
			Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .SimulationTime = 12.5 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 2);
			CHECK(snapshot->Meshes[0].Distance == MeshDrawDistance{});
			CHECK(snapshot->Meshes[1].Distance == MeshDrawDistance{ .CullDistance = 250.0f, .LodBias = 2.5f, .ShadowLodOffset = 3, .FadeWidth = 0.2f });
			CHECK_FALSE(snapshot->Wind.Enabled);
			CHECK(snapshot->Wind == RenderWind{});

			Entity weather = scene.CreateEntity("Weather");
			weather.AddComponent<WindComponent>(WindComponent{ .Direction = glm::vec3(0.0f, 0.0f, -3.0f), .Strength = 18.0f, .GustStrength = 6.0f, .GustFrequency = 0.4f, .Turbulence = 0.7f });
			TransformSystem::Update(scene);
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .SimulationTime = 12.5 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Wind == RenderWind{ .Enabled = true, .Direction = glm::vec3(0.0f, 0.0f, -3.0f), .Strength = 18.0f, .GustStrength = 6.0f, .GustFrequency = 0.4f, .Turbulence = 0.7f, .Time = 12.5 });
			// An edit-time request (time 0) shows the wind without its sway's progress; a disabled Wind does not blow.
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE(snapshot.has_value());
			CHECK(snapshot->Wind.Enabled);
			CHECK(snapshot->Wind.Time == 0.0);
			weather.SetActive(false);
			TransformSystem::Update(scene);
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .SimulationTime = 12.5 });
			REQUIRE(snapshot.has_value());
			CHECK_FALSE(snapshot->Wind.Enabled);
		}
	}

}
