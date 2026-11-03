#include "TestsPCH.h"

#include "Engine/Renderer/RenderPrepare.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

// The CPU rules of §8.3 pass 1 (Architecture §15.2 "Renderer (CPU): ... light culling; transparent sort ties";
// Docs/Decisions/0013-m8-decisions.md decision 7).

namespace Engine {

	namespace {

		// A perspective camera at the origin looking down -Z with a 90 degree field of view, 1:1, culling beyond 100 m.
		CameraData MakeCamera()
		{
			CameraData camera;
			camera.View = glm::mat4(1.0f);
			camera.Position = glm::vec3(0.0f);
			camera.VerticalFov = 90.0f;
			camera.FarClip = 100.0f;
			camera.ViewportWidth = 64;
			camera.ViewportHeight = 64;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 90.0f, 10.0f, 0.1f, 100.0f, 64, 64);
			return camera;
		}

		LightData MakePointLight(const glm::vec3& position, float intensity, uint64_t entity)
		{
			return LightData{ .Type = RenderLightType::Point, .Intensity = intensity, .Position = position, .Range = 2.0f, .Entity = UUID(entity) };
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderPrepare: bounds behind the camera, beside the frustum or beyond FarClip are outside the view")
		{
			const CameraData camera = MakeCamera();
			const Aabb unit{ .Min = glm::vec3(-0.5f), .Max = glm::vec3(0.5f) };
			CHECK_FALSE(IsOutsideView(unit, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f)), camera));
			CHECK(IsOutsideView(unit, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f)), camera));
			CHECK(IsOutsideView(unit, glm::translate(glm::mat4(1.0f), glm::vec3(20.0f, 0.0f, -5.0f)), camera));
			CHECK(IsOutsideView(unit, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -150.0f)), camera));
			CHECK_FALSE(IsOutsideView(Aabb{}, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f)), camera));
		}

		TEST_CASE("RenderPrepare: directional lights are always visible and point lights outside the view are culled")
		{
			const CameraData camera = MakeCamera();
			const std::vector<LightData> lights = {
				MakePointLight(glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 1), // behind the camera, out of range
				LightData{ .Type = RenderLightType::Directional, .Intensity = 3.0f, .Entity = UUID(2) },
				MakePointLight(glm::vec3(0.0f, 0.0f, -5.0f), 10.0f, 3), // in view
				MakePointLight(glm::vec3(0.0f, 0.0f, 1.0f), 10.0f, 4),  // behind, but its range reaches the view
			};
			const LightCullResult result = CullLights(lights, camera);
			REQUIRE(result.Visible.size() == 3);
			CHECK(result.Visible[0] == 1); // directional lights come first
			CHECK(std::ranges::find(result.Visible, 0U) == result.Visible.end());
			CHECK(result.Culled == 1);
			CHECK(result.Dropped == 0);
		}

		TEST_CASE("RenderPrepare: a spot light is culled by the bounding sphere of its cone")
		{
			const CameraData camera = MakeCamera();
			// Behind the camera with a range that reaches the view: as a point light it would be visible, but its narrow cone
			// points away from the view and its sphere lies behind the near plane; turned towards the view it is visible.
			LightData away{ .Type = RenderLightType::Spot,
				.Intensity = 1.0f,
				.Position = glm::vec3(0.0f, 0.0f, 1.0f),
				.Direction = glm::vec3(0.0f, 0.0f, 1.0f),
				.Range = 2.0f,
				.InnerConeAngle = 10.0f,
				.OuterConeAngle = 20.0f,
				.Entity = UUID(1) };
			LightData towards = away;
			towards.Direction = glm::vec3(0.0f, 0.0f, -1.0f);
			towards.Entity = UUID(2);
			LightData wide = away;
			wide.OuterConeAngle = 80.0f; // the base circle's sphere, which reaches past the camera
			wide.Entity = UUID(3);
			const std::vector<LightData> lights = { away, towards, wide };
			const LightCullResult result = CullLights(lights, camera);
			CHECK(result.Visible == std::vector<uint32_t>{ 1, 2 });
			CHECK(result.Culled == 1);
		}

		TEST_CASE("RenderPrepare: more than 256 visible lights keeps the most important and counts the dropped ones")
		{
			const CameraData camera = MakeCamera();
			std::vector<LightData> lights;
			for (uint32_t index = 0; index < MaxVisibleLights + 10; ++index)
				lights.push_back(MakePointLight(glm::vec3(0.0f, 0.0f, -5.0f), 1.0f + static_cast<float>(index), index + 1));
			const LightCullResult result = CullLights(lights, camera);
			CHECK(result.Visible.size() == MaxVisibleLights);
			CHECK(result.Dropped == 10);
			// The ten weakest (the first ten) are the ones dropped.
			for (uint32_t index = 0; index < 10; ++index)
				CHECK(std::ranges::find(result.Visible, index) == result.Visible.end());
			// Deterministic: the same input gives the same list.
			CHECK(CullLights(lights, camera).Visible == result.Visible);
		}

		TEST_CASE("RenderPrepare: lights with zero or non-finite radiance are culled")
		{
			const CameraData camera = MakeCamera();
			std::vector<LightData> lights = { MakePointLight(glm::vec3(0.0f, 0.0f, -5.0f), 0.0f, 1),
				MakePointLight(glm::vec3(0.0f, 0.0f, -5.0f), std::numeric_limits<float>::infinity(), 2),
				LightData{ .Type = RenderLightType::Directional, .Color = glm::vec3(0.0f), .Entity = UUID(3) } };
			const LightCullResult result = CullLights(lights, camera);
			CHECK(result.Visible.empty());
			CHECK(result.Culled == 3);
		}

		TEST_CASE("RenderPrepare: transparent draws sort back to front with ties by entity UUID")
		{
			std::vector<TransparentSortKey> keys = {
				{ .ViewDepth = 2.0f, .Entity = UUID(5), .Submesh = 0, .DrawIndex = 0 },
				{ .ViewDepth = 9.0f, .Entity = UUID(7), .Submesh = 0, .DrawIndex = 1 },
				{ .ViewDepth = 2.0f, .Entity = UUID(3), .Submesh = 1, .DrawIndex = 2 },
				{ .ViewDepth = 2.0f, .Entity = UUID(3), .Submesh = 0, .DrawIndex = 3 },
				{ .ViewDepth = std::numeric_limits<float>::quiet_NaN(), .Entity = UUID(9), .Submesh = 0, .DrawIndex = 4 },
			};
			SortTransparentDraws(keys);
			std::vector<uint32_t> order;
			for (const TransparentSortKey& key : keys)
				order.push_back(key.DrawIndex);
			CHECK(order == std::vector<uint32_t>{ 4, 1, 3, 2, 0 });
		}

		TEST_CASE("RenderPrepare: opaque draws sort by pipeline, material and mesh, ties in snapshot order")
		{
			std::vector<OpaqueSortKey> keys = {
				{ .Pipeline = 1, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .DrawIndex = 0 },
				{ .Pipeline = 0, .Material = AssetHandle(20), .Mesh = AssetHandle(1), .DrawIndex = 1 },
				{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(2), .DrawIndex = 2 },
				{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .DrawIndex = 3 },
				{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .DrawIndex = 4 },
			};
			SortOpaqueDraws(keys);
			std::vector<uint32_t> order;
			for (const OpaqueSortKey& key : keys)
				order.push_back(key.DrawIndex);
			CHECK(order == std::vector<uint32_t>{ 3, 4, 2, 1, 0 });
		}

		// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): the submesh joins the opaque key before the
		// snapshot order, so the draws of two-submesh items of one mesh and material come out submesh by submesh, each in
		// snapshot order, where they would interleave item by item without it and no two equal draws would be adjacent.
		TEST_CASE("RenderPrepare: opaque draws of one mesh and material sort by submesh before snapshot order")
		{
			std::vector<OpaqueSortKey> keys;
			for (uint32_t item = 0; item < 3; ++item)
			{
				for (uint32_t submesh = 0; submesh < 2; ++submesh)
					keys.push_back({ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .Submesh = submesh, .DrawIndex = item * 2 + submesh });
			}
			keys.push_back({ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(2), .Submesh = 0, .DrawIndex = 6 });
			SortOpaqueDraws(keys);
			std::vector<uint32_t> order;
			for (const OpaqueSortKey& key : keys)
				order.push_back(key.DrawIndex);
			CHECK(order == std::vector<uint32_t>{ 0, 2, 4, 1, 3, 5, 6 });
		}

		// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2, stream A): an instanced draw is a maximal run of
		// adjacent draws with equal keys, so the runs, in order, cover the pass's draws once and in their order.
		TEST_CASE("RenderPrepare: GroupMeshInstances batches adjacent draws with equal keys, in order")
		{
			const MeshInstanceKey cube{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .Submesh = 0 };
			const MeshInstanceKey cubeTop{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .Submesh = 1 };
			const MeshInstanceKey sphere{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(2), .Submesh = 0 };
			const MeshInstanceKey mirrored{ .Pipeline = 1, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .Submesh = 0 };
			// Two deformed draws of the cube (C23.3): each reads vertices of its own, so neither batches with the other or with
			// the cube's undeformed draw before them.
			const MeshInstanceKey deformedA{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .Submesh = 0, .VertexStream = 1 };
			const MeshInstanceKey deformedB{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .Submesh = 0, .VertexStream = 2 };
			const std::vector<MeshInstanceKey> keys = { cube, cube, cubeTop, sphere, sphere, sphere, cube, mirrored, cube, deformedA, deformedB };
			const std::vector<MeshInstanceBatch> batches = GroupMeshInstances(keys);
			const std::vector<MeshInstanceBatch> expected = { { .First = 0, .Count = 2 }, { .First = 2, .Count = 1 }, { .First = 3, .Count = 3 },
				{ .First = 6, .Count = 1 }, { .First = 7, .Count = 1 }, { .First = 8, .Count = 1 }, { .First = 9, .Count = 1 }, { .First = 10, .Count = 1 } };
			CHECK(batches == expected);
			CHECK(GroupMeshInstances({}).empty());
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		// M23 acceptance (R-INST, stream A): the same snapshot of 24 equal quads of two submeshes each, mirrored ones and two
		// materials renders byte-identically with instancing and with one draw per submesh (RenderQualitySettings::Instancing
		// false). The instanced render records one forward draw per pipeline variant, material and submesh: 2 x 2 x 2 = 8 of
		// the 48 submesh draws. Without the submesh in the opaque order the two submeshes of each quad would alternate, no two
		// equal draws would be adjacent, and nothing would batch.
		TEST_CASE("SceneRenderer: an instanced render is byte-identical to the per-draw render")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(UUID(8912), CreateRef<MaterialData>());
			// The integration fixture's quad as two submeshes of one triangle each, both of material slot 0.
			auto quad = CreateRef<MeshData>();
			quad->Vertices = {
				{ .Position = { -1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 1 } },
				{ .Position = { 1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 1 } },
				{ .Position = { 1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 0 } },
				{ .Position = { -1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } },
			};
			quad->Indices = { 0, 1, 2, 0, 2, 3 };
			quad->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
			quad->Submeshes = { { .IndexOffset = 0, .IndexCount = 3, .MaterialSlot = 0, .Bounds = quad->Bounds },
				{ .IndexOffset = 3, .IndexCount = 3, .MaterialSlot = 0, .Bounds = quad->Bounds } };
			quad->Slots = { {} };
			setup.Assets().Publish(UUID(8913), quad);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			for (int index = 0; index < 24; ++index)
			{
				const glm::vec3 position(static_cast<float>(index % 6) - 2.5f, static_cast<float>(index / 6) - 1.5f, -8.0f);
				glm::mat4 world = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(0.4f));
				if (index % 5 == 0)
					world = world * glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
				snapshot.Meshes.push_back({ .World = world, .Mesh = UUID(8913), .Materials = { UUID(index % 3 == 0 ? 8912 : 8911) } });
			}
			snapshot.Lights.push_back({ .Type = RenderLightType::Directional, .Direction = glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f)) });

			const auto forwardDraws = [&setup]()
			{
				for (const RenderPassStats& pass : setup.Renderer().GetRenderStats().Passes)
				{
					if (pass.Name == std::string_view("ForwardOpaque"))
						return pass.DrawCalls;
				}
				return 0U;
			};
			const Image instanced = setup.Render(snapshot);
			const uint32_t instancedDraws = forwardDraws();
			snapshot.Quality.Instancing = false;
			++snapshot.FrameIndex;
			const Image perDraw = setup.Render(snapshot);
			const uint32_t perDrawDraws = forwardDraws();
			CHECK(instanced.Pixels == perDraw.Pixels);
			CHECK(perDrawDraws == 48);
			CHECK(instancedDraws == 8);
		}

		// M23 (R-INST): the picking prepass reads each instance's pick id from its record, so two items of one mesh and
		// material drawn as one instanced draw write their own ids, as the per-draw render does.
		TEST_CASE("SceneRenderer: an instanced picking prepass writes each instance's pick id")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Flags = RenderViewFlags::Picking;
			MeshDrawItem left = snapshot.Meshes[0];
			left.World = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, -3.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.4f));
			MeshDrawItem right = left;
			right.World[3].x = 1.0f;
			right.Entity = UUID(40);
			right.PickId = 2;
			snapshot.Meshes = { left, right };
			const auto readIds = [&gpu, &setup](const RenderSnapshot& rendered)
			{
				setup.Submit(rendered);
				Readback readback(gpu.GetDevice());
				auto ids = readback.ReadTexture(*setup.Renderer().GetEntityIdTexture());
				REQUIRE_MESSAGE(ids.has_value(), ids.error().ToString());
				return std::move(*ids);
			};
			const auto idAt = [](const Image& image, uint32_t x, uint32_t y)
			{
				uint32_t id = 0;
				std::memcpy(&id, image.GetRow(y).data() + static_cast<size_t>(x) * sizeof(id), sizeof(id));
				return id;
			};
			const Image instanced = readIds(snapshot);
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* depth = Test::SceneRendererIntegrationFixture::FindPass(stats, "DepthNormal");
			REQUIRE(depth != nullptr);
			CHECK(depth->DrawCalls == 1);
			CHECK(depth->Triangles == 4);
			// The quads' centres, at x = -1 and 1 three metres away with a 60 degree field of view: view pixels 13 and 50.
			CHECK(idAt(instanced, 13, 32) == 1);
			CHECK(idAt(instanced, 50, 32) == 2);
			CHECK(idAt(instanced, 32, 32) == 0);
			snapshot.Quality.Instancing = false;
			++snapshot.FrameIndex;
			const Image perDraw = readIds(snapshot);
			CHECK(instanced.Pixels == perDraw.Pixels);
		}
	}

}
