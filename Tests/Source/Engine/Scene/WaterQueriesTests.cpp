#include "TestsPCH.h"

#include "Engine/Scene/WaterQueries.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PlanarReflectionComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <optional>
#include <string_view>

// Which water holds a point (M47, Docs/Decisions/0065-m47-water.md decision 3): the effectively enabled Rectangle whose
// volume holds it with the highest surface, ties by canonical order, else the scene's one Ocean; a Rectangle's volume masks
// the Ocean, so a cave lake below the sea is dry above its surface; a water's height follows its entity's Translation.
// Render extraction resolves each water through ResolveWater: M47's two extraction cases moved here, unchanged and under their
// names, from RenderExtractionTests.cpp at phase 8's landing, which keeps that file under MSVC's object section limit (the
// lead's decision), with the helpers they use.

namespace Engine {

	namespace {

		// A MeshRenderer drawing `mesh` with its default materials.
		MeshRendererComponent MakeMeshRenderer(AssetHandle mesh)
		{
			MeshRendererComponent renderer;
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
			return renderer;
		}

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

		// An entity `name` with a Water of `shape` at `translation`, turned `yawDegrees` about +Y and scaled by `scale`.
		Entity AddWater(Scene& scene, std::string_view name, WaterShape shape, const glm::vec3& translation, float yawDegrees = 0.0f,
			const glm::vec3& scale = glm::vec3(1.0f))
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([&translation, yawDegrees, &scale](TransformComponent& transform)
			{
				transform.Translation = translation;
				transform.Rotation = glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
				transform.Scale = scale;
			});
			WaterComponent water;
			water.Shape = shape;
			entity.AddComponent<WaterComponent>(water);
			return entity;
		}

		// Sets the Rectangle's size, depth and headroom.
		void ShapeRectangle(Entity entity, const glm::vec2& size, float depth, float headroom)
		{
			entity.Patch<WaterComponent>([&size, depth, headroom](WaterComponent& water)
			{
				water.Size = size;
				water.Depth = depth;
				water.Headroom = headroom;
			});
		}

		// The id of the water that holds `point` at time 0, or the null UUID.
		UUID WaterAt(const Scene& scene, const glm::dvec3& point)
		{
			const std::optional<SceneWaterSample> sample = SampleSceneWater(scene, point, 0.0);
			return sample.has_value() ? sample->Water : UUID();
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("WaterQueries: the topmost Rectangle holds a point, ties by canonical order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity sea = AddWater(scene, "Sea", WaterShape::Ocean, glm::vec3(0.0f, 0.0f, 0.0f));
			// Two overlapping pools at heights 2 and 3 (volumes from 1 m below to 4 m above), and a third pool level with the
			// second, later in canonical order.
			const Entity low = AddWater(scene, "Low", WaterShape::Rectangle, glm::vec3(0.0f, 2.0f, 0.0f));
			const Entity high = AddWater(scene, "High", WaterShape::Rectangle, glm::vec3(4.0f, 3.0f, 0.0f));
			const Entity level = AddWater(scene, "Level", WaterShape::Rectangle, glm::vec3(8.0f, 3.0f, 0.0f));
			for (const Entity pool : { low, high, level })
				ShapeRectangle(pool, glm::vec2(10.0f), 1.0f, 4.0f);

			// Only the low pool's footprint: the low pool, at a point above its surface too (dry, within its headroom).
			CHECK(WaterAt(scene, glm::dvec3(-3.0, 1.5, 0.0)) == low.GetUUID());
			CHECK(WaterAt(scene, glm::dvec3(-3.0, 5.0, 0.0)) == low.GetUUID());
			// Where the low and high pools overlap, the high one's surface is higher.
			CHECK(WaterAt(scene, glm::dvec3(2.0, 2.5, 0.0)) == high.GetUUID());
			const std::optional<SceneWaterSample> overlap = SampleSceneWater(scene, glm::dvec3(2.0, 2.5, 0.0), 0.0);
			REQUIRE(overlap.has_value());
			CHECK(overlap->Height == 3.0);
			CHECK(overlap->Depth == 0.5);
			// Where the high and level pools overlap at equal heights, the earlier one in canonical order.
			CHECK(WaterAt(scene, glm::dvec3(6.0, 2.5, 0.0)) == high.GetUUID());
			// Under the low pool's floor (more than 1 m below its base) and beyond its headroom: the sea.
			CHECK(WaterAt(scene, glm::dvec3(-3.0, 0.5, 0.0)) == sea.GetUUID());
			CHECK(WaterAt(scene, glm::dvec3(-3.0, 6.5, 0.0)) == sea.GetUUID());
			// Outside every footprint: the sea; a disabled pool holds nothing.
			CHECK(WaterAt(scene, glm::dvec3(40.0, 2.5, 0.0)) == sea.GetUUID());
			high.SetActive(false);
			CHECK(WaterAt(scene, glm::dvec3(2.0, 2.5, 0.0)) == low.GetUUID());
			CHECK(WaterAt(scene, glm::dvec3(6.0, 2.5, 0.0)) == level.GetUUID());
			// No Ocean: nothing outside the pools.
			sea.SetActive(false);
			CHECK_FALSE(SampleSceneWater(scene, glm::dvec3(40.0, 2.5, 0.0), 0.0).has_value());
		}

		TEST_CASE("WaterQueries: a Rectangle's volume masks the Ocean, so a cave below the sea is dry")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity sea = AddWater(scene, "Sea", WaterShape::Ocean, glm::vec3(0.0f));
			// S05's grotto: a lake 30 m below the sea's level in a 20 m chamber, its Headroom up to the chamber's ceiling.
			const Entity lake = AddWater(scene, "Grotto", WaterShape::Rectangle, glm::vec3(100.0f, -30.0f, 50.0f), 35.0f);
			ShapeRectangle(lake, glm::vec2(20.0f, 20.0f), 4.0f, 12.0f);

			// In the cave's air above the lake, 25 m under the sea's level: the lake's, dry (depth negative), not the sea.
			const glm::dvec3 air(100.0, -25.0, 50.0);
			const std::optional<SceneWaterSample> cave = SampleSceneWater(scene, air, 0.0);
			REQUIRE(cave.has_value());
			CHECK(cave->Water == lake.GetUUID());
			CHECK(cave->Depth == doctest::Approx(-5.0));
			const SceneWaters waters = CollectSceneWaters(scene);
			REQUIRE(waters.Ocean.has_value());
			CHECK_FALSE(IsUnderwater(waters.Rectangles.front(), air, 0.0));
			CHECK(IsUnderwater(*waters.Ocean, air, 0.0)); // the sea's own surface, masking aside
			// In the lake: under its surface.
			const std::optional<SceneWaterSample> swim = SampleSceneWater(scene, glm::dvec3(104.0, -31.0, 47.0), 0.0);
			REQUIRE(swim.has_value());
			CHECK(swim->Water == lake.GetUUID());
			CHECK(swim->Depth == doctest::Approx(1.0));
			// Above the ceiling (the rock over the cave) and below the lake's floor: the sea again.
			CHECK(WaterAt(scene, glm::dvec3(100.0, -17.0, 50.0)) == sea.GetUUID());
			CHECK(WaterAt(scene, glm::dvec3(100.0, -35.0, 50.0)) == sea.GetUUID());
			// The footprint turns with the yaw: a point 12 m along the lake's local +X lies beyond its 10 m half extent.
			const glm::quat yaw = glm::angleAxis(glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::dvec3 outside = glm::dvec3(100.0, -25.0, 50.0) + glm::dvec3(yaw * glm::vec3(12.0f, 0.0f, 0.0f));
			const glm::dvec3 inside = glm::dvec3(100.0, -25.0, 50.0) + glm::dvec3(yaw * glm::vec3(9.0f, 0.0f, 9.0f));
			CHECK(WaterAt(scene, outside) == sea.GetUUID());
			CHECK(WaterAt(scene, inside) == lake.GetUUID());
			// A scale on X doubles the footprint along local X: the point 12 m out is now inside.
			lake.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f, 1.0f, 1.0f);
			});
			CHECK(WaterAt(scene, outside) == lake.GetUUID());
			const ResolvedWater resolved = ResolveWater(lake);
			CHECK_FALSE(SampleWater(resolved, glm::dvec3(100.0, -25.0, 80.0), 0.0).has_value());
			CHECK(SampleWater(resolved, glm::dvec3(100.0, -25.0, 55.0), 0.0).has_value());
		}

		TEST_CASE("WaterQueries: a water's height follows its entity's Translation")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity sea = AddWater(scene, "Sea", WaterShape::Ocean, glm::vec3(0.0f, 1.25f, 0.0f));
			sea.Patch<WaterComponent>([](WaterComponent& water)
			{
				water.Waves = { WaterWave{ .Direction = 20.0f, .Wavelength = 30.0f, .Amplitude = 0.4f, .Steepness = 0.6f } };
			});
			const glm::dvec3 point(7.5, 0.0, -3.0);
			const std::optional<SceneWaterSample> before = SampleSceneWater(scene, point, 4.0);
			REQUIRE(before.has_value());
			// The tide: the base rises by 0.75 m, and with it every point of the surface.
			sea.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 2.0f;
			});
			const std::optional<SceneWaterSample> after = SampleSceneWater(scene, point, 4.0);
			REQUIRE(after.has_value());
			CHECK(after->Height - before->Height == doctest::Approx(0.75).epsilon(1e-9));
			CHECK(glm::length(after->Normal - before->Normal) < 1.0e-12);
			// Under a parent, the world height counts: the parent raised by 3 m raises the water by 3 m.
			Entity coast = scene.CreateEntity("Coast");
			REQUIRE(scene.SetParent(sea, coast, std::nullopt, false));
			coast.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 3.0f;
			});
			const std::optional<SceneWaterSample> raised = SampleSceneWater(scene, point, 4.0);
			REQUIRE(raised.has_value());
			CHECK(raised->Height - after->Height == doctest::Approx(3.0).epsilon(1e-9));
			// A calm water is its base exactly.
			sea.Patch<WaterComponent>([](WaterComponent& water)
			{
				water.WaveScale = 0.0f;
			});
			const std::optional<SceneWaterSample> calm = SampleSceneWater(scene, point, 4.0);
			REQUIRE(calm.has_value());
			CHECK(calm->Height == 5.0);
		}

		TEST_CASE("WaterQueries: only the first effectively enabled Ocean is the scene's")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity first = AddWater(scene, "First", WaterShape::Ocean, glm::vec3(0.0f, 1.0f, 0.0f));
			const Entity second = AddWater(scene, "Second", WaterShape::Ocean, glm::vec3(0.0f, 5.0f, 0.0f));
			CHECK(WaterAt(scene, glm::dvec3(0.0)) == first.GetUUID());
			const SceneWaters waters = CollectSceneWaters(scene);
			CHECK(waters.Rectangles.empty());
			REQUIRE(waters.Ocean.has_value());
			CHECK(waters.Ocean->Entity == first.GetUUID());
			first.SetActive(false);
			CHECK(WaterAt(scene, glm::dvec3(0.0)) == second.GetUUID());
		}

		// M47 (Docs/Decisions/0065-m47-water.md decision 3): every effectively enabled water but a second Ocean, in canonical
		// order, at its rendered pose, its waves resolved as the queries resolve them, their phases at the request's simulation
		// time (an edit view's is 0), the detail drift wrapped, and its PickId after the terrains'.
		TEST_CASE("RenderExtraction: a water is extracted at its rendered pose with its waves at the request's time, and at time 0 in an edit scene")
		{
			const auto addWater = [](Scene& scene, std::string_view name, WaterShape shape, const glm::vec3& translation)
			{
				Entity entity = scene.CreateEntity(name);
				SetTranslation(entity, translation);
				WaterComponent water;
				water.Shape = shape;
				water.Waves = { WaterWave{ .Direction = 30.0f, .Wavelength = 12.0f, .Amplitude = 0.5f, .Steepness = 0.7f, .Speed = 0.0f, .Phase = 45.0f },
					WaterWave{ .Direction = -60.0f, .Wavelength = 4.0f, .Amplitude = 0.2f, .Steepness = 1.0f, .Speed = 3.0f, .Phase = 0.0f } };
				water.WaveScale = 1.5f;
				water.DetailDrift = glm::vec2(2.0f, -1.0f);
				water.DetailScale = 4.0f;
				water.ShallowColor = glm::vec3(0.2f, 0.6f, 0.5f);
				water.UnderwaterVisibility = 22.0f;
				entity.AddComponent<WaterComponent>(water);
				return entity;
			};
			Test::SceneTestFixture edit;
			Scene& scene = edit.GetScene();
			CreateCamera(scene, "Camera", true);
			const Entity sea = addWater(scene, "Sea", WaterShape::Ocean, glm::vec3(0.0f, -1.0f, 0.0f));
			const Entity pool = addWater(scene, "Pool", WaterShape::Rectangle, glm::vec3(5.0f, 3.0f, -2.0f));
			// A second Ocean, which is left out.
			static_cast<void>(addWater(scene, "Second", WaterShape::Ocean, glm::vec3(0.0f, 2.0f, 0.0f)));
			const Entity off = addWater(scene, "Off", WaterShape::Rectangle, glm::vec3(0.0f));
			off.SetActive(false);
			Entity ground = scene.CreateEntity("Ground");
			ground.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::PlaneMesh));
			// A terrain after the waters in canonical order: its PickId still comes before theirs (extraction loads no asset).
			Entity island = scene.CreateEntity("Island");
			island.AddComponent<TerrainComponent>(TerrainComponent{ .Heightfield = TypedAssetHandle<AssetType::Heightfield>(AssetHandle(0x7e47000000000001ull)) });
			TransformSystem::Update(scene);

			for (const double time : { 0.0, 7.5 })
			{
				CAPTURE(time);
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .SimulationTime = time });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				// The sea and the pool; the second Ocean and the disabled pool are left out.
				REQUIRE(snapshot->Waters.size() == 2);
				const WaterItem& item = snapshot->Waters[0];
				CHECK(item.Entity == sea.GetUUID());
				CHECK(item.Shape == RenderWaterShape::Ocean);
				CHECK(snapshot->Waters[1].Entity == pool.GetUUID());
				CHECK(snapshot->Waters[1].Shape == RenderWaterShape::Rectangle);
				CHECK(item.World == TransformSystem::ComputeWorldMatrix(sea));
				// The waves as the queries resolve them, their phases at the request's time.
				const ResolvedWater resolved = ResolveWater(sea);
				const std::array<double, MaxWaterSurfaceWaves> phases = ComputeWaterWavePhases(resolved.Surface, time);
				REQUIRE(item.WaveCount == 2);
				for (uint32_t index = 0; index < item.WaveCount; ++index)
				{
					const WaterSurfaceWave& wave = resolved.Surface.Waves[index];
					CHECK(item.Waves[index].Direction == glm::vec2(wave.Direction));
					CHECK(item.Waves[index].WaveNumber == static_cast<float>(wave.WaveNumber));
					CHECK(item.Waves[index].Amplitude == static_cast<float>(wave.Amplitude));
					CHECK(item.Waves[index].Steepness == static_cast<float>(wave.Steepness));
					CHECK(item.Waves[index].Phase == static_cast<float>(phases[index]));
				}
				CHECK(item.Waves[0].Amplitude == doctest::Approx(0.75f));
				// The detail drift at the time, wrapped by DetailScale: 2 m/s and -1 m/s over 7.5 s are 15 and -7.5 m, so 3 and 0.5.
				CHECK(item.DetailOffset.x == doctest::Approx(time == 0.0 ? 0.0 : 3.0));
				CHECK(item.DetailOffset.y == doctest::Approx(time == 0.0 ? 0.0 : 0.5));
				CHECK(item.ShallowColor == glm::vec3(0.2f, 0.6f, 0.5f));
				CHECK(item.UnderwaterVisibility == 22.0f);
				CHECK_FALSE(item.PreviousWorld.has_value());
				CHECK_FALSE(item.PreviousPhases.has_value());
				// PickIds after the meshes' (the ground's) and the terrains' (the island's), in the waters' order.
				REQUIRE(snapshot->PickTable.size() == 4);
				REQUIRE(snapshot->Terrains.size() == 1);
				CHECK(snapshot->Terrains[0].PickId == 2);
				CHECK(item.PickId == 3);
				CHECK(snapshot->PickTable[2] == sea.GetUUID());
				CHECK(snapshot->Waters[1].PickId == 4);
			}
			// An edit view (SimulationTime 0, the default) shows the phases at time 0.
			const Result<RenderSnapshot> editView = ExtractRenderSnapshot(scene, { .Camera = RenderCameraSource::Explicit, .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(editView.has_value(), editView.error().ToString());
			REQUIRE(editView->Waters.size() == 2);
			CHECK(editView->Waters[0].Waves[0].Phase == static_cast<float>(ComputeWaterWavePhases(ResolveWater(sea).Surface, 0.0)[0]));

			// In a runtime scene the item takes the interpolated pose: the tide written in the fixed step moves smoothly.
			Test::SceneTestFixture runtime(1, true);
			Entity tide = addWater(runtime.GetScene(), "Tide", WaterShape::Ocean, glm::vec3(0.0f));
			TransformSystem::Update(runtime.GetScene());
			SnapshotPreviousPose(tide);
			SetTranslation(tide, glm::vec3(0.0f, 2.0f, 0.0f));
			TransformSystem::Update(runtime.GetScene());
			const Result<RenderSnapshot> between = ExtractRenderSnapshot(runtime.GetScene(), { .Width = 8, .Height = 8, .Alpha = 0.5f });
			REQUIRE_MESSAGE(between.has_value(), between.error().ToString());
			REQUIRE(between->Waters.size() == 1);
			CHECK(between->Waters[0].World[3].y == doctest::Approx(1.0f));
		}

		// M47's planar opt-in (Docs/Decisions/0065-m47-water.md decision 3, W47.3): a PlanarReflection on a drawn water gives
		// an item of the water's base plane, level at its rendered position (Normal +Y, Offset 0; the component's Normal and
		// Offset are not read, its ResolutionScale is), whatever the entity draws; a second Ocean, which is not drawn, gives none.
		TEST_CASE("RenderExtraction: a planar reflection on a water is the water's base plane")
		{
			Test::SceneTestFixture edit;
			Scene& scene = edit.GetScene();
			CreateCamera(scene, "Camera", true);
			Entity lake = scene.CreateEntity("Lake");
			lake.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(3.0f, -2.0f, 5.0f);
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(10.0f, 40.0f, 0.0f));
			});
			WaterComponent water;
			water.Shape = WaterShape::Rectangle;
			lake.AddComponent<WaterComponent>(water);
			lake.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::QuadMesh));
			lake.AddComponent<PlanarReflectionComponent>(PlanarReflectionComponent{ .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Offset = 0.5f, .ResolutionScale = 0.25f });
			Entity sea = scene.CreateEntity("Sea");
			sea.AddComponent<WaterComponent>();
			Entity secondSea = scene.CreateEntity("SecondSea");
			secondSea.AddComponent<WaterComponent>();
			secondSea.AddComponent<PlanarReflectionComponent>();
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->PlanarReflections.size() == 1);
			const PlanarReflectionItem& item = snapshot->PlanarReflections[0];
			CHECK(item.Entity == lake.GetUUID());
			CHECK(item.Normal == glm::vec3(0.0f, 1.0f, 0.0f));
			CHECK(item.Offset == 0.0f);
			CHECK(item.ResolutionScale == 0.25f);
			glm::mat4 level(1.0f);
			level[3] = glm::vec4(3.0f, -2.0f, 5.0f, 1.0f);
			CHECK(item.World == level);
		}
	}

}
