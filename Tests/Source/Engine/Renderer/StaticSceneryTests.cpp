#include "TestsPCH.h"
#include "Engine/Renderer/RenderStaticSet.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/PlanarReflections.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/ProbeCapture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/StaticComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Shared/InstanceRecord.h"
#include "Support/BufferCreationFails.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// The renderer's static path (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3; SceneRendererStatic.cpp). Most cases
// render a snapshot twice: as it is, its mesh items dynamic (dense pick ids in snapshot order), and as its static twin, the
// same items moved into a static set built as the scene's RenderStaticCache builds one (MakeStaticSet: the cell of each item's
// world origin, cells in key order, items in snapshot order). The twins draw the same pixels; the static one draws each key
// once per view and face. The fixture's 96 x 64 view looks down -Z from the origin.

namespace Engine {

	namespace {

		constexpr uint32_t Width = 96;
		constexpr uint32_t Height = 64;
		constexpr AssetHandle QuadMeshHandle{ 8910 }; // the fixture's 2 x 2 quad facing +Z
		constexpr AssetHandle GreyMaterialHandle{ 0x5943000000000001ull };
		constexpr AssetHandle RedMaterialHandle{ 0x5943000000000002ull };
		constexpr AssetHandle GreenMaterialHandle{ 0x5943000000000003ull };
		constexpr AssetHandle MaskMaterialHandle{ 0x5943000000000004ull };
		constexpr AssetHandle GlassMaterialHandle{ 0x5943000000000005ull };
		constexpr AssetHandle MirrorMaterialHandle{ 0x5943000000000006ull };
		constexpr AssetHandle RedEmitterHandle{ 0x5943000000000007ull };
		constexpr AssetHandle GreenEmitterHandle{ 0x5943000000000008ull };
		constexpr AssetHandle MorphedMeshHandle{ 0x5943000000000009ull };
		constexpr AssetHandle PlainMeshHandle{ 0x594300000000000aull };
		constexpr AssetHandle ReloadedMeshHandle{ 0x594300000000000bull };

		AssetRef<Asset> MakeMaterial(const glm::vec4& baseColor, float metallic, float roughness, const glm::vec3& emissive, bool doubleSided,
			AlphaMode mode = AlphaMode::Opaque)
		{
			MaterialData material;
			material.BaseColor = baseColor;
			material.Metallic = metallic;
			material.Roughness = roughness;
			material.Emissive = emissive;
			material.DoubleSided = doubleSided;
			material.AlphaMode = mode;
			return CreateRef<MaterialData>(std::move(material));
		}

		// A quad of `half` metres a side's half facing +Z, with a morph target that pushes its top edge 0.5 m along +Z at the
		// default weight `defaultWeight` (none at all without `morphed`).
		Ref<MeshData> MakeStrip(bool morphed, float defaultWeight, float half = 1.0f)
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			for (const glm::vec2 corner : { glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(-1.0f, 1.0f) })
			{
				const glm::vec3 position(corner * half, 0.0f);
				mesh->Vertices.push_back({ .Position = position, .Normal = { 0.0f, 0.0f, 1.0f }, .Tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .TexCoord = (corner + 1.0f) * 0.5f });
				mesh->Bounds.Extend(position);
			}
			mesh->Indices = { 0, 1, 2, 0, 2, 3 };
			mesh->Submeshes.push_back({ .IndexOffset = 0, .IndexCount = 6, .MaterialSlot = 0, .Bounds = mesh->Bounds });
			mesh->Slots.push_back({ .Name = "Default" });
			if (morphed)
			{
				MeshMorphTarget push{ .Name = "Push", .DefaultWeight = defaultWeight };
				for (size_t index = 0; index < mesh->Vertices.size(); ++index)
					push.Deltas.push_back({ .Position = { 0.0f, 0.0f, index >= 2 ? 0.5f : 0.0f } });
				mesh->MorphTargets.push_back(push);
			}
			return mesh;
		}

		void PublishAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			Test::InMemoryAssetManager& assets = fixture.Assets();
			assets.Publish(GreyMaterialHandle, MakeMaterial(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), 0.0f, 0.8f, glm::vec3(0.0f), false));
			assets.Publish(RedMaterialHandle, MakeMaterial(glm::vec4(0.9f, 0.2f, 0.1f, 1.0f), 0.0f, 0.5f, glm::vec3(0.0f), false));
			assets.Publish(GreenMaterialHandle, MakeMaterial(glm::vec4(0.1f, 0.8f, 0.2f, 1.0f), 0.5f, 0.3f, glm::vec3(0.0f), false));
			assets.Publish(MaskMaterialHandle, MakeMaterial(glm::vec4(0.3f, 0.3f, 0.9f, 1.0f), 0.0f, 0.6f, glm::vec3(0.0f), true, AlphaMode::Mask));
			assets.Publish(GlassMaterialHandle, MakeMaterial(glm::vec4(0.2f, 0.4f, 0.9f, 0.5f), 0.0f, 0.2f, glm::vec3(0.1f), true, AlphaMode::Blend));
			assets.Publish(MirrorMaterialHandle, MakeMaterial(glm::vec4(1.0f), 1.0f, 0.0f, glm::vec3(0.0f), false));
			assets.Publish(RedEmitterHandle, MakeMaterial(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 1.0f, glm::vec3(2.0f, 0.0f, 0.0f), true));
			assets.Publish(GreenEmitterHandle, MakeMaterial(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 1.0f, glm::vec3(0.0f, 2.0f, 0.0f), true));
			assets.Publish(MorphedMeshHandle, MakeStrip(true, 0.6f));
			assets.Publish(PlainMeshHandle, MakeStrip(false, 0.0f));
			assets.Publish(ReloadedMeshHandle, MakeStrip(false, 0.0f, 0.5f));
		}

		glm::mat4 Place(const glm::vec3& position, const glm::vec3& scale = glm::vec3(1.0f), float turn = 0.0f)
		{
			return glm::translate(glm::mat4(1.0f), position) * glm::rotate(glm::mat4(1.0f), glm::radians(turn), glm::vec3(0.0f, 1.0f, 0.0f))
				* glm::scale(glm::mat4(1.0f), scale);
		}

		MeshDrawItem MakeItem(const glm::mat4& world, AssetHandle mesh, AssetHandle material, uint64_t entity, bool castShadows = true)
		{
			return MeshDrawItem{ .World = world, .Mesh = mesh, .Materials = { material }, .CastShadows = castShadows, .Entity = UUID(entity) };
		}

		// The cell RenderStaticCache gives an item at `world` (its translation's floor over StaticCellSize, in double).
		std::pair<int32_t, int32_t> GetCellKey(const glm::mat4& world)
		{
			const auto key = [](float coordinate)
			{
				return static_cast<int32_t>(std::floor(static_cast<double>(coordinate) / static_cast<double>(StaticCellSize)));
			};
			return { key(world[3].x), key(world[3].z) };
		}

		// `items` as a static set: each in the cell of its world origin, cells in key order (x, then z), items in their order,
		// each cell's pick table its items' entities; the cell and set versions from `version`, which moves.
		Ref<const RenderStaticSet> MakeStaticSet(std::span<const MeshDrawItem> items, uint64_t& version)
		{
			std::map<std::pair<int32_t, int32_t>, Ref<RenderStaticCell>> cells;
			for (const MeshDrawItem& item : items)
			{
				Ref<RenderStaticCell>& cell = cells[GetCellKey(item.World)];
				if (cell == nullptr)
				{
					cell = CreateRef<RenderStaticCell>();
					const std::pair<int32_t, int32_t> key = GetCellKey(item.World);
					cell->Key = glm::ivec2(key.first, key.second);
					cell->Version = ++version;
				}
				MeshDrawItem copy = item;
				copy.PickId = 0;
				copy.PreviousWorld.reset();
				cell->Items.push_back(copy);
				cell->PickTable.push_back(item.Entity);
				cell->Dissolving = cell->Dissolving || item.Parameters.DissolveAmount > 0.0f;
			}
			Ref<RenderStaticSet> set = CreateRef<RenderStaticSet>();
			for (const auto& entry : cells)
				set->Cells.push_back(entry.second);
			set->Version = ++version;
			return set;
		}

		// `snapshot` with dense pick ids for its mesh items in snapshot order.
		RenderSnapshot WithPickIds(RenderSnapshot snapshot)
		{
			snapshot.PickTable.clear();
			for (size_t index = 0; index < snapshot.Meshes.size(); ++index)
			{
				snapshot.Meshes[index].PickId = static_cast<uint32_t>(index + 1);
				snapshot.PickTable.push_back(snapshot.Meshes[index].Entity);
			}
			return snapshot;
		}

		// The static twin of `snapshot`: its mesh items but those of `dynamic` moved into a static set, the others keeping their
		// pick ids in order.
		RenderSnapshot MakeStaticTwin(RenderSnapshot snapshot, std::span<const UUID> dynamic = {})
		{
			std::vector<MeshDrawItem> moved;
			std::vector<MeshDrawItem> kept;
			for (const MeshDrawItem& item : snapshot.Meshes)
			{
				if (std::ranges::find(dynamic, item.Entity) != dynamic.end())
					kept.push_back(item);
				else
					moved.push_back(item);
			}
			uint64_t version = 0;
			snapshot.StaticSet = MakeStaticSet(moved, version);
			snapshot.Meshes.assign(kept.begin(), kept.end());
			return WithPickIds(std::move(snapshot));
		}

		// The fixture's snapshot without its quad, at `Width` x `Height`, with a dim ambient light.
		RenderSnapshot MakeEmptySnapshot(Test::SceneRendererIntegrationFixture& fixture)
		{
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Meshes.clear();
			snapshot.PickTable.clear();
			snapshot.Camera.ClearColor = glm::vec3(0.05f, 0.07f, 0.1f);
			snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = false, .FallbackColor = glm::vec3(0.08f) };
			return snapshot;
		}

		LightData MakeSun(const glm::vec3& direction, uint32_t cascades)
		{
			LightData sun;
			sun.Direction = glm::normalize(direction);
			sun.Intensity = 3.0f;
			sun.CastShadows = true;
			sun.ShadowDistance = 20.0f;
			sun.CascadeCount = cascades;
			sun.Entity = UUID(200);
			return sun;
		}

		LightData MakeSpot(const glm::vec3& position, uint64_t entity)
		{
			LightData spot;
			spot.Type = RenderLightType::Spot;
			spot.Position = position;
			spot.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
			spot.Range = 12.0f;
			spot.Intensity = 40.0f;
			spot.CastShadows = true;
			spot.Entity = UUID(entity);
			return spot;
		}

		LightData MakePoint(const glm::vec3& position, float range, uint64_t entity)
		{
			LightData point;
			point.Type = RenderLightType::Point;
			point.Position = position;
			point.Range = range;
			point.Intensity = 20.0f;
			point.CastShadows = true;
			point.Entity = UUID(entity);
			return point;
		}

		// The scenery of the byte-identity case: a floor, opaque cubes and spheres of two materials, a mirrored (negative scale)
		// cube, a Mask material, a tinted and a dissolving item, two glass cubes and a glass sphere (Blend), one far item in
		// another cell the view culls, under a shadowed sun (two cascades), a shadowed spot and a shadowed point light.
		RenderSnapshot MakeScenery(Test::SceneRendererIntegrationFixture& fixture)
		{
			RenderSnapshot snapshot = MakeEmptySnapshot(fixture);
			auto& meshes = snapshot.Meshes;
			meshes.push_back(MakeItem(Place(glm::vec3(0.0f, -1.6f, -8.0f), glm::vec3(24.0f, 0.2f, 24.0f)), BuiltinAssetHandles::CubeMesh, GreyMaterialHandle, 1, false));
			meshes.push_back(MakeItem(Place(glm::vec3(-2.2f, -0.9f, -6.0f), glm::vec3(1.0f), 20.0f), BuiltinAssetHandles::CubeMesh, RedMaterialHandle, 2));
			meshes.push_back(MakeItem(Place(glm::vec3(0.2f, -0.9f, -7.5f), glm::vec3(1.0f), 35.0f), BuiltinAssetHandles::CubeMesh, RedMaterialHandle, 3));
			meshes.push_back(MakeItem(Place(glm::vec3(2.4f, -0.9f, -9.0f), glm::vec3(1.0f), 50.0f), BuiltinAssetHandles::CubeMesh, GreenMaterialHandle, 4));
			meshes.push_back(MakeItem(Place(glm::vec3(-1.0f, -0.5f, -10.0f), glm::vec3(1.4f)), BuiltinAssetHandles::SphereMesh, GreenMaterialHandle, 5));
			meshes.push_back(MakeItem(Place(glm::vec3(1.6f, 0.4f, -5.0f), glm::vec3(-0.8f, 0.8f, 0.8f), 15.0f), BuiltinAssetHandles::CubeMesh, RedMaterialHandle, 6));
			meshes.push_back(MakeItem(Place(glm::vec3(-3.0f, 0.3f, -8.5f), glm::vec3(0.9f)), BuiltinAssetHandles::CubeMesh, MaskMaterialHandle, 7));
			MeshDrawItem tinted = MakeItem(Place(glm::vec3(0.9f, -0.6f, -11.5f), glm::vec3(1.2f)), BuiltinAssetHandles::SphereMesh, RedMaterialHandle, 8);
			tinted.Parameters.Tint = glm::vec3(0.4f, 0.9f, 1.0f);
			tinted.Parameters.Emissive = glm::vec3(0.1f, 0.0f, 0.2f);
			meshes.push_back(tinted);
			MeshDrawItem dissolving = MakeItem(Place(glm::vec3(3.5f, 0.1f, -12.0f), glm::vec3(1.3f)), BuiltinAssetHandles::CubeMesh, GreyMaterialHandle, 9);
			dissolving.Parameters.DissolveAmount = 0.35f;
			dissolving.Parameters.DissolveEdge = glm::vec3(1.0f, 0.4f, 0.0f);
			dissolving.Parameters.DissolveEdgeWidth = 0.05f;
			meshes.push_back(dissolving);
			meshes.push_back(MakeItem(Place(glm::vec3(-0.6f, 0.2f, -4.2f), glm::vec3(0.7f), 30.0f), BuiltinAssetHandles::CubeMesh, GlassMaterialHandle, 10, false));
			meshes.push_back(MakeItem(Place(glm::vec3(0.4f, 0.5f, -3.6f), glm::vec3(0.6f), 10.0f), BuiltinAssetHandles::CubeMesh, GlassMaterialHandle, 11, false));
			meshes.push_back(MakeItem(Place(glm::vec3(-1.8f, 0.9f, -5.5f), glm::vec3(0.8f)), BuiltinAssetHandles::SphereMesh, GlassMaterialHandle, 12, false));
			meshes.push_back(MakeItem(Place(glm::vec3(-70.0f, 0.0f, -8.0f)), BuiltinAssetHandles::CubeMesh, RedMaterialHandle, 13));
			snapshot.Lights = { MakeSun(glm::vec3(0.3f, -1.0f, -0.4f), 2), MakeSpot(glm::vec3(0.0f, 3.0f, -6.0f), 201), MakePoint(glm::vec3(-2.0f, 1.2f, -7.0f), 6.0f, 202) };
			return WithPickIds(std::move(snapshot));
		}

		// Renders `snapshot` with the next frame index and returns Render's status.
		Status RenderStatus(GraphicsDevice& device, Test::SceneRendererIntegrationFixture& fixture, RenderSnapshot& snapshot)
		{
			snapshot.FrameIndex += 1;
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			Status rendered = fixture.Renderer().Render(**list, snapshot);
			(*list)->close();
			fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
			return rendered;
		}

		// Renders `snapshot`, which must render without an error, and reads the final image back.
		Image RenderImage(GraphicsDevice& device, Test::SceneRendererIntegrationFixture& fixture, RenderSnapshot snapshot)
		{
			const Status rendered = RenderStatus(device, fixture, snapshot);
			REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			return fixture.Read();
		}

		uint32_t GetPassDraws(const RenderStats& stats, std::string_view name)
		{
			const RenderPassStats* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, name);
			return pass != nullptr ? pass->DrawCalls : 0;
		}

		// The number of pixels where `a` and `b` differ.
		size_t CountDifferences(const Image& a, const Image& b)
		{
			REQUIRE(a.Pixels.size() == b.Pixels.size());
			size_t differences = 0;
			for (size_t index = 0; index + 4 <= a.Pixels.size(); index += 4)
			{
				if (!std::equal(a.Pixels.begin() + static_cast<std::ptrdiff_t>(index), a.Pixels.begin() + static_cast<std::ptrdiff_t>(index + 4),
						b.Pixels.begin() + static_cast<std::ptrdiff_t>(index)))
					++differences;
			}
			return differences;
		}

		// A camera at `position` looking along `forward` (unit), +Y up unless the view looks along it.
		void PlaceCamera(RenderSnapshot& snapshot, const glm::vec3& position, const glm::vec3& forward)
		{
			const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			snapshot.Camera.View = glm::lookAt(position, position + forward, up);
			snapshot.Camera.Position = position;
		}

		std::vector<std::byte> ToBytes(const ProbeCubeCapture& capture)
		{
			std::vector<std::byte> bytes;
			for (const std::vector<glm::vec4>& face : capture.Faces)
			{
				const std::span<const std::byte> texels = std::as_bytes(std::span(face));
				bytes.insert(bytes.end(), texels.begin(), texels.end());
			}
			return bytes;
		}

		// A scene for the extraction-driven cases: a primary camera at the origin looking down -Z, a sun (with `lit`; else an
		// Environment of intensity 0, so nothing lights the scene), and a root (Static with `isStatic`) over cubes at
		// `positions`.
		struct StaticScene
		{
			Test::SceneTestFixture Fixture{};
			Entity Root{};
			std::vector<Entity> Cubes{};
		};

		void BuildStaticScene(StaticScene& built, bool isStatic, std::span<const glm::vec3> positions, bool lit = true)
		{
			Scene& scene = built.Fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .FarClip = 40.0f, .Primary = true, .Clear = ClearMode::Color, .ClearColor = glm::vec3(0.05f, 0.07f, 0.1f) });
			if (!lit)
			{
				// No ambient light either (RENDER_NO_LIGHTING's case).
				Entity environment = scene.CreateEntity("Environment");
				environment.AddComponent<EnvironmentComponent>().Intensity = 0.0f;
			}
			else
			{
				Entity sun = scene.CreateEntity("Sun");
				sun.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Intensity = 3.0f, .ShadowDistance = 30.0f, .CascadeCount = 2 });
				sun.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Rotation = glm::quat(glm::radians(glm::vec3(-50.0f, 20.0f, 0.0f)));
				});
			}
			built.Root = scene.CreateEntity("Scenery");
			if (isStatic)
				built.Root.AddComponent<StaticComponent>();
			for (size_t index = 0; index < positions.size(); ++index)
			{
				Entity cube = scene.CreateEntity(std::format("Cube{}", index), built.Root);
				cube.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
				const glm::vec3 position = positions[index];
				cube.Patch<TransformComponent>([position, index](TransformComponent& transform)
				{
					transform.Translation = position;
					transform.Rotation = glm::quat(glm::radians(glm::vec3(0.0f, 17.0f * static_cast<float>(index), 0.0f)));
				});
				built.Cubes.push_back(cube);
			}
			TransformSystem::Update(scene);
		}

		// The snapshot of `scene`'s primary camera at the fixture's size with the fixture's post settings.
		RenderSnapshot ExtractScene(const Scene& scene, RenderViewFlags flags = RenderViewFlags::None)
		{
			Result<RenderSnapshot> extracted = ExtractRenderSnapshot(scene, { .Width = Width, .Height = Height, .Flags = flags });
			REQUIRE_MESSAGE(extracted.has_value(), extracted.error().ToString());
			RenderSnapshot snapshot = std::move(*extracted);
			snapshot.Post.BloomEnabled = false;
			snapshot.Post.AntiAliasing = RenderAntiAliasing::None;
			snapshot.Post.SsaoEnabled = false;
			snapshot.Post.SsrEnabled = false;
			snapshot.Post.Tonemap = RenderTonemapper::Linear;
			snapshot.Quality.ShadowMapSize = 512;
			return snapshot;
		}

	}

	TEST_SUITE("Renderer")
	{
		// Every mesh view's draws of the static twin are the dynamic twin's: the camera's prepass and forward passes (opaque, Mask,
		// mirrored, tinted, dissolving and Blend items), the three kinds of shadow maps and the image they shade, byte for byte.
		// The counts follow the dynamic path's (visible meshes, culled submeshes, submesh draws), and the static set's stats are
		// the set's.
		TEST_CASE("StaticScenery: a scene renders the same bytes with its scenery static and without" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				PublishAssets(fixture);
				const RenderSnapshot scenery = MakeScenery(fixture);
				const Image dynamic = RenderImage(device, fixture, scenery);
				const RenderStats dynamicStats = fixture.Renderer().GetRenderStats();
				const SceneRenderStats dynamicDraws = fixture.Renderer().GetLastStats();
				const RenderSnapshot twin = MakeStaticTwin(scenery);
				REQUIRE(twin.Meshes.empty());
				REQUIRE(twin.StaticSet->Cells.size() == 3);
				const Image staticImage = RenderImage(device, fixture, twin);
				const RenderStats staticStats = fixture.Renderer().GetRenderStats();
				const SceneRenderStats staticDraws = fixture.Renderer().GetLastStats();

				CHECK(CountDifferences(staticImage, dynamic) == 0);
				CHECK(staticStats.VisibleMeshes == dynamicStats.VisibleMeshes);
				CHECK(staticStats.VisibleMeshes == 12);
				CHECK(staticStats.CulledSubmeshes == dynamicStats.CulledSubmeshes);
				CHECK(staticDraws.MeshDraws == dynamicDraws.MeshDraws);
				CHECK(staticDraws.TransparentDraws == dynamicDraws.TransparentDraws);
				CHECK(staticStats.ShadowedSpotLights == 1);
				CHECK(staticStats.ShadowedPointLights == 1);
				CHECK(staticStats.StaticItems == 13);
				CHECK(staticStats.StaticCells == 3);
				CHECK(staticStats.StaticCellsVisible == 2);
				CHECK(staticStats.DynamicItems == 0);
				CHECK(dynamicStats.StaticItems == 0);
				CHECK(dynamicStats.StaticCells == 0);
				CHECK(dynamicStats.DynamicItems == 13);
				// One draw per key per view: never more than the dynamic path's batches.
				CHECK(GetPassDraws(staticStats, "ForwardOpaque") <= GetPassDraws(dynamicStats, "ForwardOpaque"));
				CHECK(GetPassDraws(staticStats, "DirectionalShadows") <= GetPassDraws(dynamicStats, "DirectionalShadows"));

				// Half static, half dynamic: the same image again, the static draws before the dynamic draws of their keys.
				const std::array<UUID, 5> dynamicEntities = { UUID(2), UUID(4), UUID(8), UUID(10), UUID(12) };
				const Image mixed = RenderImage(device, fixture, MakeStaticTwin(scenery, dynamicEntities));
				CHECK(CountDifferences(mixed, dynamic) == 0);
				CHECK(fixture.Renderer().GetRenderStats().VisibleMeshes == 12);
			}
			device.RunGarbageCollection();
		}

		// One cube at the middle of each of 64 cells (8 x 8 of 32 m) and one more in the first cell's far corner: a camera that
		// sees one cell tests that cell's items alone and culls the other 63 whole, each counting its submeshes as culled as the
		// dynamic path counts them; another camera, another cell; looking up, none.
		TEST_CASE("StaticScenery: a view culls whole cells, so its static work follows what it sees" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				PublishAssets(fixture);
				RenderSnapshot grid = MakeEmptySnapshot(fixture);
				grid.Camera.FarClip = 40.0f;
				grid.Lights = { MakeSun(glm::vec3(0.2f, -1.0f, -0.3f), 1) };
				for (int32_t x = 0; x < 8; ++x)
				{
					for (int32_t z = 0; z < 8; ++z)
					{
						const glm::vec3 centre(16.0f + 32.0f * static_cast<float>(x), 0.0f, 16.0f + 32.0f * static_cast<float>(z));
						grid.Meshes.push_back(MakeItem(Place(centre), BuiltinAssetHandles::CubeMesh, (x + z) % 2 == 0 ? RedMaterialHandle : GreenMaterialHandle,
							1000 + static_cast<uint64_t>(x * 8 + z)));
					}
				}
				grid.Meshes.push_back(MakeItem(Place(glm::vec3(31.0f, 0.0f, 31.0f)), BuiltinAssetHandles::CubeMesh, GreyMaterialHandle, 999));
				grid = WithPickIds(std::move(grid));

				struct View
				{
					glm::vec3 Position;
					glm::vec3 Forward;
					uint32_t VisibleCells;
				};
				const std::array<View, 3> views = { View{ glm::vec3(16.0f, 0.0f, 40.0f), glm::vec3(0.0f, 0.0f, -1.0f), 1 },
					View{ glm::vec3(80.0f, 0.0f, 104.0f), glm::vec3(0.0f, 0.0f, -1.0f), 1 }, View{ glm::vec3(80.0f, 0.0f, 104.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0 } };
				for (const View& view : views)
				{
					CAPTURE(view.Position);
					CAPTURE(view.Forward);
					RenderSnapshot dynamicView = grid;
					PlaceCamera(dynamicView, view.Position, view.Forward);
					const Image dynamic = RenderImage(device, fixture, dynamicView);
					const RenderStats dynamicStats = fixture.Renderer().GetRenderStats();
					const Image staticImage = RenderImage(device, fixture, MakeStaticTwin(dynamicView));
					const RenderStats staticStats = fixture.Renderer().GetRenderStats();
					CHECK(CountDifferences(staticImage, dynamic) == 0);
					CHECK(staticStats.StaticCells == 64);
					CHECK(staticStats.StaticItems == 65);
					CHECK(staticStats.StaticCellsVisible == view.VisibleCells);
					CHECK(staticStats.VisibleMeshes == view.VisibleCells);
					CHECK(staticStats.VisibleMeshes == dynamicStats.VisibleMeshes);
					CHECK(staticStats.CulledSubmeshes == dynamicStats.CulledSubmeshes);
					CHECK(staticStats.CulledSubmeshes == 65 - view.VisibleCells);
				}
			}
			device.RunGarbageCollection();
		}

		// Cubes and spheres (two caster keys) under a spot light and a one-cascade sun, each interleaved in snapshot order with
		// casters far outside every shadow face, and a cube below and a sphere above a point light: each face draws one draw per
		// key of the static casters it sees (two in the cascade, two in the spot tile, one in each of the point light's two
		// faces that see one), where the dynamic path draws a run per adjacent visible caster; the shadow maps, and so the image,
		// are the same.
		TEST_CASE("StaticScenery: each shadow face draws one draw per key of its visible static casters" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				PublishAssets(fixture);
				RenderSnapshot casters = MakeEmptySnapshot(fixture);
				casters.Meshes.push_back(MakeItem(Place(glm::vec3(0.0f, -1.6f, -8.0f), glm::vec3(30.0f, 0.2f, 30.0f)), BuiltinAssetHandles::CubeMesh, GreyMaterialHandle, 1, false));
				uint64_t entity = 10;
				for (int32_t row = 0; row < 3; ++row)
				{
					for (int32_t column = 0; column < 3; ++column)
					{
						const glm::vec3 position(-1.6f + 1.6f * static_cast<float>(column), -0.9f, -7.6f + 1.6f * static_cast<float>(row));
						const AssetHandle mesh = (row + column) % 2 == 0 ? BuiltinAssetHandles::CubeMesh : BuiltinAssetHandles::SphereMesh;
						casters.Meshes.push_back(MakeItem(Place(position, glm::vec3(0.8f)), mesh, RedMaterialHandle, ++entity));
						// A caster no face sees, after each visible one.
						const uint64_t hidden = ++entity;
						casters.Meshes.push_back(MakeItem(Place(glm::vec3(-300.0f - 4.0f * static_cast<float>(hidden), 0.0f, -8.0f)), mesh, GreenMaterialHandle, hidden));
					}
				}
				casters.Meshes.push_back(MakeItem(Place(glm::vec3(12.0f, -0.4f, -12.0f), glm::vec3(0.8f)), BuiltinAssetHandles::CubeMesh, RedMaterialHandle, 100));
				casters.Meshes.push_back(MakeItem(Place(glm::vec3(12.0f, 4.4f, -12.0f), glm::vec3(0.8f)), BuiltinAssetHandles::SphereMesh, GreenMaterialHandle, 101));
				casters.Lights = { MakeSun(glm::vec3(0.1f, -1.0f, -0.2f), 1), MakeSpot(glm::vec3(0.0f, 6.0f, -6.0f), 301), MakePoint(glm::vec3(12.0f, 2.0f, -12.0f), 4.0f, 302) };
				casters = WithPickIds(std::move(casters));

				const Image dynamic = RenderImage(device, fixture, casters);
				const RenderStats dynamicStats = fixture.Renderer().GetRenderStats();
				const Image staticImage = RenderImage(device, fixture, MakeStaticTwin(casters));
				const RenderStats staticStats = fixture.Renderer().GetRenderStats();
				CHECK(CountDifferences(staticImage, dynamic) == 0);
				REQUIRE(staticStats.ShadowedSpotLights == 1);
				REQUIRE(staticStats.ShadowedPointLights == 1);
				CHECK(GetPassDraws(staticStats, "DirectionalShadows") == 2);
				CHECK(GetPassDraws(staticStats, "SpotShadows") == 2);
				CHECK(GetPassDraws(staticStats, "PointShadows") == 2);
				CHECK(staticStats.ShadowDraws == 6);
				// The dynamic casters' batches break at each caster a face culls.
				CHECK(GetPassDraws(dynamicStats, "DirectionalShadows") > 2);
				CHECK(GetPassDraws(dynamicStats, "SpotShadows") > 2);
			}
			device.RunGarbageCollection();
		}

		// A scene whose scenery root is Static, and its twin without Static: a cube moved across a cell edge (a write through
		// Entity::Patch) leaves its old cell and joins another, both rebuilt as new cell objects while the untouched cell stays
		// the same object, and the static render follows it, as the dynamic twin's does.
		TEST_CASE("StaticScenery: a moved static entity's cell rebuilds and its draw follows" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				const std::array<glm::vec3, 3> positions = { glm::vec3(-1.5f, 0.0f, -6.0f), glm::vec3(1.5f, 0.0f, -6.0f), glm::vec3(0.5f, 0.0f, -33.0f) };
				StaticScene withStatic;
				BuildStaticScene(withStatic, true, positions);
				StaticScene withoutStatic;
				BuildStaticScene(withoutStatic, false, positions);
				const RenderSnapshot before = ExtractScene(withStatic.Fixture.GetScene());
				REQUIRE(before.StaticSet != nullptr);
				REQUIRE(before.StaticSet->Cells.size() == 3);
				CHECK(before.Meshes.empty());
				const uint64_t rebuildsBefore = withStatic.Fixture.GetScene().GetStaticCellRebuilds();
				const Image beforeImage = RenderImage(device, fixture, before);
				CHECK(CountDifferences(beforeImage, RenderImage(device, fixture, ExtractScene(withoutStatic.Fixture.GetScene()))) == 0);

				// The far cube (cell (0, -2)) moves into the second cube's cell (0, -1).
				for (StaticScene* built : { &withStatic, &withoutStatic })
				{
					built->Cubes[2].Patch<TransformComponent>([](TransformComponent& transform)
					{
						transform.Translation = glm::vec3(0.0f, 1.2f, -8.0f);
					});
					TransformSystem::Update(built->Fixture.GetScene());
				}
				const RenderSnapshot after = ExtractScene(withStatic.Fixture.GetScene());
				REQUIRE(after.StaticSet != nullptr);
				REQUIRE(after.StaticSet->Cells.size() == 2);
				// Cell (-1, -1) (the first cube's) is the same object; cell (0, -1) is rebuilt with the moved cube, and cell (0, -2) is
				// gone: two cells rebuilt or removed.
				const auto findCell = [](const RenderStaticSet& set, const glm::ivec2& key) -> Ref<const RenderStaticCell>
				{
					for (const Ref<const RenderStaticCell>& cell : set.Cells)
					{
						if (cell->Key == key)
							return cell;
					}
					return nullptr;
				};
				CHECK(findCell(*after.StaticSet, glm::ivec2(-1, -1)) == findCell(*before.StaticSet, glm::ivec2(-1, -1)));
				REQUIRE(findCell(*after.StaticSet, glm::ivec2(0, -1)) != nullptr);
				CHECK(findCell(*after.StaticSet, glm::ivec2(0, -1)) != findCell(*before.StaticSet, glm::ivec2(0, -1)));
				CHECK(findCell(*after.StaticSet, glm::ivec2(0, -1))->Items.size() == 2);
				CHECK(findCell(*after.StaticSet, glm::ivec2(0, -2)) == nullptr);
				CHECK(withStatic.Fixture.GetScene().GetStaticCellRebuilds() - rebuildsBefore == 2);
				const Image afterImage = RenderImage(device, fixture, after);
				CHECK(CountDifferences(afterImage, beforeImage) > 0);
				CHECK(CountDifferences(afterImage, RenderImage(device, fixture, ExtractScene(withoutStatic.Fixture.GetScene()))) == 0);
				CHECK(fixture.Renderer().GetRenderStats().VisibleMeshes == 3);
			}
			device.RunGarbageCollection();
		}

		// The static markers behind a mirror (a dynamic glass with a PlanarReflection) show in its reflection exactly as dynamic
		// ones do; a viewport capture and a probe capture of the static twin give the dynamic twin's pixels; a pick of a static
		// item's pixel names its entity through its static pick id; and a selected static item's outline is the dynamic one's.
		TEST_CASE("StaticScenery: mirror views, captures, picking and selection draw static items" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				PublishAssets(fixture);
				// The mirror: M42's fixture scene, the markers behind the camera at z = +2 facing the glass at z = -3.
				RenderSnapshot mirror = MakeEmptySnapshot(fixture);
				mirror.Environment.FallbackColor = glm::vec3(0.0f);
				mirror.Post.SsrEnabled = false;
				const glm::mat4 glass = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f));
				mirror.Meshes.push_back(MakeItem(glass, QuadMeshHandle, MirrorMaterialHandle, 900, false));
				mirror.PlanarReflections.push_back(PlanarReflectionItem{ .World = glass, .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Entity = UUID(900) });
				mirror.Meshes.push_back(MakeItem(Place(glm::vec3(-0.8f, 0.0f, 2.0f), glm::vec3(0.4f)), QuadMeshHandle, RedEmitterHandle, 1));
				mirror.Meshes.push_back(MakeItem(Place(glm::vec3(0.8f, 0.0f, 2.0f), glm::vec3(0.4f)), QuadMeshHandle, GreenEmitterHandle, 2));
				mirror = WithPickIds(std::move(mirror));
				const std::array<UUID, 1> glassEntity = { UUID(900) };
				const Image dynamicMirror = RenderImage(device, fixture, mirror);
				const Image staticMirror = RenderImage(device, fixture, MakeStaticTwin(mirror, glassEntity));
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				CHECK(CountDifferences(staticMirror, dynamicMirror) == 0);
				// The reflection shows the red marker on the glass's left.
				CHECK(Test::SceneRendererIntegrationFixture::Channel(staticMirror, 42, 31, 0) > 200);

				// The captures.
				const RenderSnapshot scenery = MakeScenery(fixture);
				const RenderSnapshot twin = MakeStaticTwin(scenery);
				auto viewport = ViewportCapture::CreateForScenes(device, fixture.Pipelines(), fixture.Cache(), fixture.Assets());
				REQUIRE_MESSAGE(viewport.has_value(), viewport.error().ToString());
				const Result<Image> capturedDynamic = (*viewport)->Capture({ .Width = 48, .Height = 32 }, scenery);
				REQUIRE_MESSAGE(capturedDynamic.has_value(), capturedDynamic.error().ToString());
				const Result<Image> capturedStatic = (*viewport)->Capture({ .Width = 48, .Height = 32 }, twin);
				REQUIRE_MESSAGE(capturedStatic.has_value(), capturedStatic.error().ToString());
				CHECK(CountDifferences(*capturedStatic, *capturedDynamic) == 0);
				auto probe = ProbeCapture::Create(device, fixture.Pipelines(), fixture.Cache(), fixture.Assets());
				REQUIRE_MESSAGE(probe.has_value(), probe.error().ToString());
				const Result<ProbeCubeCapture> probeDynamic = (*probe)->Capture(scenery, glm::vec3(0.0f, 0.5f, -6.5f), 16);
				REQUIRE_MESSAGE(probeDynamic.has_value(), probeDynamic.error().ToString());
				const Result<ProbeCubeCapture> probeStatic = (*probe)->Capture(twin, glm::vec3(0.0f, 0.5f, -6.5f), 16);
				REQUIRE_MESSAGE(probeStatic.has_value(), probeStatic.error().ToString());
				CHECK(ToBytes(*probeStatic) == ToBytes(*probeDynamic));

				// Picking: the red cube left of the centre (entity 2) through its static record's id, the green one (4) too.
				RenderSnapshot picking = twin;
				picking.Flags = RenderViewFlags::Picking;
				const Status picked = RenderStatus(device, fixture, picking);
				REQUIRE_MESSAGE(picked.has_value(), picked.error().ToString());
				const auto pick = [&fixture, &device, &picking](uint32_t x, uint32_t y) -> PickResult
				{
					auto ticket = fixture.Renderer().RequestPick({ .X = x, .Y = y, .FrameIndex = picking.FrameIndex, .SceneRevision = picking.SceneRevision, .ViewGeneration = fixture.Renderer().GetViewGeneration() });
					REQUIRE_MESSAGE(ticket.has_value(), ticket.error().ToString());
					device.WaitForIdle();
					const auto ready = fixture.Renderer().PollPick(*ticket, picking.FrameIndex + 2);
					REQUIRE(ready.has_value());
					REQUIRE(ready->has_value());
					return **ready;
				};
				const PickResult left = pick(28, 38);
				CHECK(left.Entity == UUID(2));
				CHECK(left.PickId >= StaticPickIdBase);
				const PickResult floor = pick(48, 62);
				CHECK(floor.Entity == UUID(1));
				CHECK(floor.PickId >= StaticPickIdBase);

				// Selection: the static cube's outline and mask draw as the dynamic one's.
				RenderSnapshot selected = scenery;
				selected.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
				selected.SelectedEntities = { UUID(3), UUID(10) };
				const Image dynamicSelection = RenderImage(device, fixture, selected);
				const RenderStats dynamicSelectionStats = fixture.Renderer().GetRenderStats();
				RenderSnapshot staticSelected = MakeStaticTwin(selected);
				const Image staticSelection = RenderImage(device, fixture, staticSelected);
				const RenderStats staticSelectionStats = fixture.Renderer().GetRenderStats();
				CHECK(CountDifferences(staticSelection, dynamicSelection) == 0);
				CHECK(GetPassDraws(staticSelectionStats, "SelectionMask") == GetPassDraws(dynamicSelectionStats, "SelectionMask"));
				CHECK(GetPassDraws(staticSelectionStats, "SelectionMask") > 0);
				CHECK(CountDifferences(staticSelection, RenderImage(device, fixture, twin)) > 0);
			}
			device.RunGarbageCollection();
		}

		// A Static scenery root over cubes in one cell and no light or emission: RENDER_NO_LIGHTING's check sees the static meshes,
		// and the wireframe overlay draws their edges exactly as it draws the same scene's without Static.
		TEST_CASE("StaticScenery: the wireframe overlay and RENDER_NO_LIGHTING see static meshes" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				// All three in cell (0, -1), so the overlay draws the static edges in the dynamic ones' order.
				const std::array<glm::vec3, 3> positions = { glm::vec3(0.5f, 0.0f, -6.0f), glm::vec3(2.0f, 0.5f, -8.0f), glm::vec3(3.0f, -0.5f, -5.0f) };
				StaticScene withStatic;
				BuildStaticScene(withStatic, true, positions, false);
				StaticScene withoutStatic;
				BuildStaticScene(withoutStatic, false, positions, false);
				const Result<RenderSceneValidation> validation = EvaluateRenderSceneValidation(withStatic.Fixture.GetScene(), fixture.Assets());
				REQUIRE(validation.has_value());
				CHECK(validation->NoLighting);
				REQUIRE(withStatic.Fixture.GetScene().GetStaticCache() != nullptr);

				const RenderViewFlags flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Wireframe;
				const RenderSnapshot wireframe = ExtractScene(withStatic.Fixture.GetScene(), flags);
				REQUIRE(wireframe.StaticSet != nullptr);
				REQUIRE(wireframe.StaticSet->Cells.size() == 1);
				CHECK(wireframe.Meshes.empty());
				const Image staticImage = RenderImage(device, fixture, wireframe);
				const RenderStats staticStats = fixture.Renderer().GetRenderStats();
				const Image dynamicImage = RenderImage(device, fixture, ExtractScene(withoutStatic.Fixture.GetScene(), flags));
				CHECK(CountDifferences(staticImage, dynamicImage) == 0);
				// The static cubes' edges are drawn (the Wireframe row), and they are what the image shows besides the clear colour.
				const RenderPassStats* edges = Test::SceneRendererIntegrationFixture::FindPass(staticStats, "Wireframe");
				REQUIRE(edges != nullptr);
				CHECK(edges->DrawCalls > 0);
				const Image cleared = RenderImage(device, fixture, [&withStatic]()
				{
					RenderSnapshot nothing = ExtractScene(withStatic.Fixture.GetScene(), RenderViewFlags::EditorOverlays | RenderViewFlags::Wireframe);
					nothing.StaticSet = nullptr;
					return nothing;
				}());
				CHECK(CountDifferences(staticImage, cleared) > 0);
			}
			device.RunGarbageCollection();
		}

		// The static record buffer is the GpuResourceCache's and the indices and prepared cells the renderer's: a device loss
		// releases both, and the recovered renderer draws the static scene byte for byte as before.
		TEST_CASE("StaticScenery: a static scene renders the same bytes after a device loss" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
			PublishAssets(fixture);
			RenderSnapshot snapshot = MakeStaticTwin(MakeScenery(fixture));
			GraphicsDevice& device = gpu.GetDevice();
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &fixture, &snapshot]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					snapshot.FrameIndex += 1;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = fixture.Renderer().Render(*list, snapshot);
					list->close();
					fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					if (fixture.Renderer().GetRenderStats().VisibleMeshes != 12)
						return MakeError(ErrorCode::InvalidState, "the static items were not drawn");
					nvrhi::ITexture* image = fixture.Renderer().GetFinalTexture();
					if (image == nullptr)
						return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
					Readback readback(device);
					ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
					pixels = std::move(read.Pixels);
				}
				return pixels;
			};
			// The view's targets reach their steady state in the pool first, as the other renderer loss cases do.
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(fixture.Cache().GetStats().StaticCellCount == 3);
		}

		// The static record buffer runs out of memory (its creation fails, then again after the trim): the render draws no
		// static item, the dynamic ones as before, and returns OutOfMemory; the next render writes the records and draws them.
		TEST_CASE("StaticScenery: static records that run out of memory are not drawn for the render, which returns the error, and the next render "
				  "draws them"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				PublishAssets(fixture);
				const RenderSnapshot scenery = MakeScenery(fixture);
				const std::array<UUID, 2> dynamicEntities = { UUID(1), UUID(5) };
				RenderSnapshot twin = MakeStaticTwin(scenery, dynamicEntities);
				RenderSnapshot withoutStatic = twin;
				withoutStatic.StaticSet = nullptr;
				const Image dynamicOnly = RenderImage(device, fixture, withoutStatic);
				const Image whole = RenderImage(device, fixture, scenery);
				Status rendered;
				{
					// The static record buffer is the only storage buffer of its size the render creates.
					const Test::BufferCreationFails fails([](const VkBufferCreateInfo& info)
					{
						return info.size == uint64_t{ GpuResourceCache::StaticRecordMinimumCapacity } * sizeof(InstanceRecord)
							&& (info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0;
					});
					rendered = RenderStatus(device, fixture, twin);
					CHECK(fails.GetFailedCount() == 2);
				}
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::OutOfMemory);
				CHECK(fixture.Cache().GetStaticRecordBuffer() == nullptr);
				CHECK(CountDifferences(fixture.Read(), dynamicOnly) == 0);
				CHECK(fixture.Renderer().GetRenderStats().StaticCellsVisible == 0);

				const Status recovered = RenderStatus(device, fixture, twin);
				REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
				CHECK(fixture.Cache().GetStaticRecordBuffer() != nullptr);
				CHECK(CountDifferences(fixture.Read(), whole) == 0);
			}
			device.RunGarbageCollection();
		}

		// The view's InstanceIndices buffer (t22) runs out of memory: at its first creation the render fails before recording, as
		// the Instances buffer's does; when it cannot grow, the render draws no static item, keeps its earlier buffer bound and
		// returns the error, and the next render grows it and draws them. The scenes are grids of static cubes with no light,
		// so the camera's draws alone fill the indices, one an item.
		TEST_CASE("StaticScenery: an InstanceIndices buffer that runs out of memory fails the render it is created for and drops the static draws "
				  "of the render it cannot grow for"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				PublishAssets(fixture);
				const auto makeGrid = [&fixture](int32_t rows)
				{
					RenderSnapshot grid = MakeEmptySnapshot(fixture);
					for (int32_t row = 0; row < rows; ++row)
					{
						for (int32_t column = 0; column < 10; ++column)
						{
							const glm::vec3 position(-4.5f + static_cast<float>(column), -3.5f + 0.8f * static_cast<float>(row), -10.0f);
							grid.Meshes.push_back(MakeItem(Place(position, glm::vec3(0.3f)), BuiltinAssetHandles::CubeMesh, RedMaterialHandle,
								static_cast<uint64_t>(1 + row * 10 + column)));
						}
					}
					return WithPickIds(std::move(grid));
				};
				const auto failing = [](uint64_t entries)
				{
					return [entries](const VkBufferCreateInfo& info)
					{
						return info.size == entries * sizeof(InstanceIndex) && (info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0;
					};
				};

				// The first render of 80 static cubes creates the buffer for its 80 indices.
				const RenderSnapshot small = makeGrid(8);
				RenderSnapshot smallTwin = MakeStaticTwin(small);
				{
					const Test::BufferCreationFails fails(failing(80));
					const Status rendered = RenderStatus(device, fixture, smallTwin);
					REQUIRE_FALSE(rendered.has_value());
					CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
					CHECK(IsGpuOutOfMemory(rendered.error(), device));
					CHECK(fails.GetFailedCount() == 1);
				}
				const Image smallStatic = RenderImage(device, fixture, smallTwin);
				CHECK(fixture.Renderer().GetRenderStats().VisibleMeshes == 80);
				CHECK(CountDifferences(smallStatic, RenderImage(device, fixture, small)) == 0);

				// 100 static cubes need 100 indices: the buffer of 80 cannot grow to 160, so the render shows none of them.
				const Image empty = RenderImage(device, fixture, MakeEmptySnapshot(fixture));
				const RenderSnapshot large = makeGrid(10);
				const Image largeDynamic = RenderImage(device, fixture, large);
				RenderSnapshot largeTwin = MakeStaticTwin(large);
				{
					const Test::BufferCreationFails fails(failing(160));
					const Status rendered = RenderStatus(device, fixture, largeTwin);
					REQUIRE_FALSE(rendered.has_value());
					CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
					CHECK(IsGpuOutOfMemory(rendered.error(), device));
					CHECK(fails.GetFailedCount() == 1);
				}
				CHECK(CountDifferences(fixture.Read(), empty) == 0);
				const RenderStats dropped = fixture.Renderer().GetRenderStats();
				CHECK(dropped.StaticItems == 100);
				CHECK(dropped.StaticCellsVisible == 0);
				CHECK(dropped.VisibleMeshes == 0);
				const Image largeStatic = RenderImage(device, fixture, largeTwin);
				CHECK(fixture.Renderer().GetRenderStats().VisibleMeshes == 100);
				CHECK(CountDifferences(largeStatic, largeDynamic) == 0);
			}
			device.RunGarbageCollection();
		}

		// A renderer's static keys hold the GPU objects of the meshes their draws read only while a prepared cell uses them: a set
		// that drops a cell, and a snapshot without a static set, leave the renderer holding nothing of those meshes, so an
		// unloaded mesh's buffers go with the cache's trim (as the dynamic path's do).
		TEST_CASE("StaticScenery: a renderer lets go of a static mesh's GPU objects once no cell it draws uses the mesh" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				PublishAssets(fixture);
				// Cell (0, -1) holds the plain strip and cell (1, -1) the smaller one, both casting under the sun.
				RenderSnapshot snapshot = MakeEmptySnapshot(fixture);
				snapshot.Meshes.push_back(MakeItem(Place(glm::vec3(0.5f, 0.0f, -4.0f)), PlainMeshHandle, RedMaterialHandle, 1));
				snapshot.Meshes.push_back(MakeItem(Place(glm::vec3(40.0f, 0.0f, -4.0f)), ReloadedMeshHandle, GreenMaterialHandle, 2));
				snapshot.Lights = { MakeSun(glm::vec3(0.2f, -1.0f, -0.6f), 1) };
				RenderSnapshot twin = MakeStaticTwin(WithPickIds(std::move(snapshot)));
				REQUIRE(twin.StaticSet->Cells.size() == 2);
				const Status first = RenderStatus(device, fixture, twin);
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				// A mesh's own buffers (its attribute stream may be the cache's shared default one).
				const auto buffersOf = [&fixture](AssetHandle mesh)
				{
					const GpuMesh& mirror = fixture.Cache().GetMesh(mesh);
					return std::array<nvrhi::BufferHandle, 2>{ mirror.VertexBuffer, mirror.IndexBuffer };
				};
				const std::array<nvrhi::BufferHandle, 2> plain = buffersOf(PlainMeshHandle);
				const std::array<nvrhi::BufferHandle, 2> smaller = buffersOf(ReloadedMeshHandle);
				// Once the device retired the renders and the cache's trim dropped the unloaded mesh's mirror, this case's handles
				// are the buffers' last holders.
				const auto released = [&device, &fixture](AssetHandle mesh, const std::array<nvrhi::BufferHandle, 2>& buffers)
				{
					fixture.Assets().Remove(mesh);
					fixture.Cache().TrimUnused();
					device.WaitForIdle();
					device.RunGarbageCollection();
					for (const nvrhi::BufferHandle& buffer : buffers)
					{
						REQUIRE(buffer != nullptr);
						CHECK_FALSE(device.GetResourceTracker().HasOtherReferences(*buffer, 1));
					}
				};

				// The set without the plain strip's cell.
				Ref<RenderStaticSet> rest = CreateRef<RenderStaticSet>(*twin.StaticSet);
				rest->Cells = { twin.StaticSet->Cells[1] };
				RenderSnapshot withoutPlain = twin;
				withoutPlain.StaticSet = rest;
				const Status second = RenderStatus(device, fixture, withoutPlain);
				REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
				CHECK(fixture.Renderer().GetRenderStats().StaticItems == 1);
				released(PlainMeshHandle, plain);

				// A snapshot without a static set.
				RenderSnapshot nothing = withoutPlain;
				nothing.StaticSet = nullptr;
				const Status third = RenderStatus(device, fixture, nothing);
				REQUIRE_MESSAGE(third.has_value(), third.error().ToString());
				released(ReloadedMeshHandle, smaller);
			}
			device.RunGarbageCollection();
		}

		// What a renderer prepared from a cell is keyed by the cell object and its meshes' and materials' mirrors: a mesh reloaded
		// under an unchanged cell (the same set object) draws its new version, as the dynamic twin does.
		TEST_CASE("StaticScenery: an asset reload under an unchanged cell redraws the new mesh" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeEmptySnapshot(fixture);
				snapshot.Meshes.push_back(MakeItem(Place(glm::vec3(0.0f, 0.0f, -4.0f)), ReloadedMeshHandle, RedEmitterHandle, 1));
				snapshot.Meshes.push_back(MakeItem(Place(glm::vec3(2.0f, 0.0f, -5.0f)), BuiltinAssetHandles::CubeMesh, GreenEmitterHandle, 2));
				snapshot = WithPickIds(std::move(snapshot));
				const RenderSnapshot twin = MakeStaticTwin(snapshot);
				const Image before = RenderImage(device, fixture, twin);
				CHECK(CountDifferences(before, RenderImage(device, fixture, snapshot)) == 0);

				fixture.Assets().Publish(ReloadedMeshHandle, MakeStrip(false, 0.0f, 1.0f));
				const Image after = RenderImage(device, fixture, twin);
				CHECK(CountDifferences(after, before) > 0);
				CHECK(CountDifferences(after, RenderImage(device, fixture, snapshot)) == 0);
				// The material too: an emitter turned green under the same cell.
				fixture.Assets().Publish(RedEmitterHandle, MakeMaterial(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 1.0f, glm::vec3(0.0f, 0.0f, 2.0f), true));
				const Image recoloured = RenderImage(device, fixture, twin);
				CHECK(CountDifferences(recoloured, after) > 0);
				CHECK(CountDifferences(recoloured, RenderImage(device, fixture, snapshot)) == 0);
			}
			device.RunGarbageCollection();
		}

		// Decision 1's limit (the W43.2 record): a static item whose mesh morphs by its default weights draws undeformed, as the
		// same mesh without morph targets draws, and the renderer warns once for its entity, also across renders and a device
		// loss.
		TEST_CASE("StaticScenery: a static item whose mesh morphs by its default weights draws undeformed, with one warning for its entity"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::ExpectLog warned(LogLevel::Warn, "morphs by its default morph weights");
			RenderSnapshot morphed;
			Image deformed;
			Image undeformed;
			{
				// The dynamic path deforms it, and the same mesh without morph targets draws undeformed: rendered by a renderer and a
				// cache of their own, so the device loss below compares a renderer and a cache that drew only the static twin (the
				// fixture collects no stale mirror, and a recovery creates again only what the renders after it use).
				Test::SceneRendererIntegrationFixture dynamicFixture(gpu, Width, Height);
				PublishAssets(dynamicFixture);
				RenderSnapshot plain = MakeEmptySnapshot(dynamicFixture);
				plain.Meshes.push_back(MakeItem(Place(glm::vec3(0.0f, 0.0f, -4.0f), glm::vec3(1.0f), 25.0f), PlainMeshHandle, RedMaterialHandle, 7));
				plain.Lights = { MakeSun(glm::vec3(0.2f, -1.0f, -0.6f), 1) };
				plain = WithPickIds(std::move(plain));
				morphed = plain;
				morphed.Meshes[0].Mesh = MorphedMeshHandle;
				deformed = RenderImage(device, dynamicFixture, morphed);
				undeformed = RenderImage(device, dynamicFixture, plain);
			}
			CHECK(CountDifferences(deformed, undeformed) > 0);
			CHECK(warned.GetMatchCount() == 0);
			Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
			PublishAssets(fixture);

			RenderSnapshot twin = MakeStaticTwin(morphed);
			CHECK(CountDifferences(RenderImage(device, fixture, twin), undeformed) == 0);
			CHECK(CountDifferences(RenderImage(device, fixture, twin), undeformed) == 0);
			CHECK(warned.GetMatchCount() == 1);

			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			// One render per frame in flight, as the other renderer loss cases, so each timing slot holds its objects.
			test.Use = [&device, &fixture, &twin]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					twin.FrameIndex += 1;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = fixture.Renderer().Render(*list, twin);
					list->close();
					fixture.Renderer().OnSubmitted(twin.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					nvrhi::ITexture* image = fixture.Renderer().GetFinalTexture();
					if (image == nullptr)
						return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
					Readback readback(device);
					ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
					pixels = std::move(read.Pixels);
				}
				return pixels;
			};
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(warned.GetMatchCount() == 1);
		}

		// The cache's static ranges: a set's cells take ranges once, a later acquisition of the same cells keeps them, a cell no
		// acquisition used since the last collection frees its range for the next new cell, a grown buffer keeps every range at
		// its place, and a trim drops the buffer and every range.
		TEST_CASE("GpuResourceCache: static cells keep their record ranges until a collection after a frame that did not use them"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::InMemoryAssetManager assets;
				GpuResourceCache cache(device, assets);
				uint64_t version = 0;
				std::vector<MeshDrawItem> items;
				for (uint64_t index = 0; index < 6; ++index)
					items.push_back(MakeItem(Place(glm::vec3(40.0f * static_cast<float>(index), 0.0f, 0.0f)), BuiltinAssetHandles::CubeMesh, AssetHandle(), index + 1));
				const Ref<const RenderStaticSet> first = MakeStaticSet(items, version);
				REQUIRE(first->Cells.size() == 6);
				const Result<std::vector<uint32_t>> firsts = cache.AcquireStaticRecords(*first);
				REQUIRE_MESSAGE(firsts.has_value(), firsts.error().ToString());
				CHECK(*firsts == std::vector<uint32_t>{ 0, 1, 2, 3, 4, 5 });
				CHECK(cache.GetStats().StaticCellCount == 6);
				CHECK(cache.GetStats().StaticRecordCapacity == GpuResourceCache::StaticRecordMinimumCapacity);
				CHECK(*cache.AcquireStaticRecords(*first) == *firsts);

				// A set that keeps cells 0, 2, 4 and 5 and replaces cell 1 by a three-item cell: the kept ranges stay; after a
				// collection that follows an acquisition without cell 1 and 3, their places are free and the new cell takes the
				// first place that holds three records, after the mark.
				Ref<RenderStaticCell> replaced = CreateRef<RenderStaticCell>(*first->Cells[1]);
				replaced->Version = ++version;
				replaced->Items.push_back(replaced->Items.front());
				replaced->Items.push_back(replaced->Items.front());
				replaced->PickTable = { UUID(20), UUID(21), UUID(22) };
				RenderStaticSet second;
				second.Cells = { first->Cells[0], first->Cells[2], first->Cells[4], first->Cells[5] };
				// A collection keeps what the frame before it used (every cell) and clears the marks; the next frame acquires
				// four cells, and the collection after it frees the other two.
				cache.CollectStale();
				CHECK(cache.GetStats().StaticCellCount == 6);
				REQUIRE(cache.AcquireStaticRecords(second).has_value());
				cache.CollectStale();
				CHECK(cache.GetStats().StaticCellCount == 4);
				second.Cells.push_back(replaced);
				const Result<std::vector<uint32_t>> again = cache.AcquireStaticRecords(second);
				REQUIRE(again.has_value());
				CHECK(*again == std::vector<uint32_t>{ 0, 2, 4, 5, 6 });
				// A one-item cell takes the freed place of cell 1.
				Ref<RenderStaticCell> single = CreateRef<RenderStaticCell>(*first->Cells[3]);
				single->Version = ++version;
				second.Cells.push_back(single);
				const Result<std::vector<uint32_t>> third = cache.AcquireStaticRecords(second);
				REQUIRE(third.has_value());
				CHECK(third->back() == 1);

				// A set beyond the capacity grows the buffer with every kept range at its place.
				std::vector<MeshDrawItem> many(GpuResourceCache::StaticRecordMinimumCapacity, items.front());
				Ref<RenderStaticCell> large = CreateRef<RenderStaticCell>();
				large->Version = ++version;
				large->Items = many;
				large->PickTable.assign(many.size(), UUID(30));
				second.Cells.push_back(large);
				const Result<std::vector<uint32_t>> grown = cache.AcquireStaticRecords(second);
				REQUIRE(grown.has_value());
				CHECK(std::vector<uint32_t>(grown->begin(), grown->end() - 1) == *third);
				CHECK(cache.GetStats().StaticRecordCapacity == 2 * GpuResourceCache::StaticRecordMinimumCapacity);
				CHECK(cache.GetStats().StaticRecordCount == 4 + 3 + 1 + GpuResourceCache::StaticRecordMinimumCapacity);

				cache.TrimUnused();
				CHECK(cache.GetStaticRecordBuffer() == nullptr);
				CHECK(cache.GetStats().StaticCellCount == 0);
				const Result<std::vector<uint32_t>> rebuilt = cache.AcquireStaticRecords(*first);
				REQUIRE(rebuilt.has_value());
				CHECK(*rebuilt == *firsts);
			}
			device.RunGarbageCollection();
		}
	}

}
