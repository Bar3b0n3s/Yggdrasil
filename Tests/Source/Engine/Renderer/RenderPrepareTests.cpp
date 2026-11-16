#include "TestsPCH.h"

#include "Engine/Renderer/RenderPrepare.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Shared/DrawConstants.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
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

		// M23's milestone review (Docs/Decisions/0034-m23-contract.md decisions 2 and 8): the record M26's motion vectors read
		// (plan §4.1). A moved item's PreviousWorld is the previous frame's world matrix it gives; an item without one (no
		// motion, every item until M26) takes its World; the mirroring flag follows the World's determinant, and the flags
		// given (the camera draws' DrawFlagReceiveShadows) join it.
		TEST_CASE("RenderPrepare: an instance record carries its item's previous world matrix, or its world matrix without one")
		{
			MeshDrawItem moved{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)), .PickId = 7 };
			moved.PreviousWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 2.0f, 3.0f));
			const InstanceRecord movedRecord = MakeMeshInstanceRecord(moved, DrawFlagReceiveShadows);
			CHECK(movedRecord.World == moved.World);
			CHECK(movedRecord.PreviousWorld == *moved.PreviousWorld);
			CHECK(movedRecord.EntityId == 7);
			CHECK(movedRecord.Flags == DrawFlagReceiveShadows);
			const MeshDrawItem mirrored{ .World = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)) };
			const InstanceRecord mirroredRecord = MakeMeshInstanceRecord(mirrored);
			CHECK(mirroredRecord.World == mirrored.World);
			CHECK(mirroredRecord.PreviousWorld == mirrored.World);
			CHECK(mirroredRecord.EntityId == 0);
			CHECK(mirroredRecord.Flags == DrawFlagMirrored);
		}

		// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 4): non-neutral parameters ride in the record with
		// DrawFlagMaterialParameters, which tells the fragment shaders to read them; neutral ones leave the record as it was
		// before M31, flag clear, so an item without the component draws exactly as before.
		TEST_CASE("RenderPrepare: an instance record carries its item's material parameters and their flag only when they are not neutral")
		{
			MeshDrawItem item{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)), .PickId = 3 };
			const InstanceRecord neutral = MakeMeshInstanceRecord(item, DrawFlagReceiveShadows);
			CHECK(neutral.Flags == DrawFlagReceiveShadows);
			CHECK(neutral.Tint == glm::vec3(1.0f));
			CHECK(neutral.Emissive == glm::vec3(0.0f));
			CHECK(neutral.UVOffset == glm::vec2(0.0f));
			CHECK(neutral.Opacity == 1.0f);
			CHECK(neutral.DissolveAmount == 0.0f);
			CHECK(neutral.DissolveEdge == glm::vec3(0.0f));
			CHECK(neutral.DissolveEdgeWidth == 0.0f);
			CHECK(neutral.DissolveNoiseScale == 1.0f);

			item.Parameters = { .Tint = glm::vec3(1.0f, 0.5f, 0.25f),
				.Emissive = glm::vec3(2.0f, 0.0f, 0.0f),
				.UVOffset = glm::vec2(0.25f, 0.75f),
				.DissolveAmount = 0.5f,
				.DissolveEdge = glm::vec3(0.0f, 0.0f, 3.0f),
				.DissolveEdgeWidth = 0.1f,
				.DissolveNoiseScale = 4.0f,
				.Opacity = 0.75f };
			const InstanceRecord record = MakeMeshInstanceRecord(item, DrawFlagReceiveShadows);
			CHECK(record.Flags == (DrawFlagReceiveShadows | DrawFlagMaterialParameters));
			CHECK(record.World == item.World);
			CHECK(record.EntityId == 3);
			CHECK(record.Tint == item.Parameters.Tint);
			CHECK(record.Emissive == item.Parameters.Emissive);
			CHECK(record.UVOffset == item.Parameters.UVOffset);
			CHECK(record.Opacity == 0.75f);
			CHECK(record.DissolveAmount == 0.5f);
			CHECK(record.DissolveEdge == item.Parameters.DissolveEdge);
			CHECK(record.DissolveEdgeWidth == 0.1f);
			CHECK(record.DissolveNoiseScale == 4.0f);

			// Any one member off its neutral value sets the flag.
			item.Parameters = {};
			item.Parameters.Opacity = 0.99f;
			CHECK((MakeMeshInstanceRecord(item).Flags & DrawFlagMaterialParameters) != 0);
			item.Parameters = {};
			item.Parameters.UVOffset.y = 0.5f;
			CHECK((MakeMeshInstanceRecord(item).Flags & DrawFlagMaterialParameters) != 0);
			item.Parameters = {};
			CHECK(item.Parameters.IsNeutral());
			CHECK((MakeMeshInstanceRecord(item).Flags & DrawFlagMaterialParameters) == 0);
		}

		// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 5): an Opaque material draws with the Mask variants
		// while its item dissolves or fades, so its fragment shaders can clip; its shadow casters only while it dissolves,
		// since a fade keeps the shadow. Mask and Blend materials keep their modes.
		TEST_CASE("RenderPrepare: an Opaque material takes the Mask variants while its item dissolves or fades, its shadow only while it dissolves")
		{
			const MeshMaterialParameters neutral;
			MeshMaterialParameters tinted;
			tinted.Tint = glm::vec3(0.5f);
			MeshMaterialParameters dissolving;
			dissolving.DissolveAmount = 0.25f;
			MeshMaterialParameters fading;
			fading.Opacity = 0.5f;
			for (const bool shadow : { false, true })
			{
				CAPTURE(shadow);
				CHECK(SelectMeshAlphaMode(AlphaMode::Opaque, neutral, shadow) == AlphaMode::Opaque);
				CHECK(SelectMeshAlphaMode(AlphaMode::Opaque, tinted, shadow) == AlphaMode::Opaque);
				CHECK(SelectMeshAlphaMode(AlphaMode::Opaque, dissolving, shadow) == AlphaMode::Mask);
				for (const MeshMaterialParameters& parameters : { neutral, tinted, dissolving, fading })
				{
					CHECK(SelectMeshAlphaMode(AlphaMode::Mask, parameters, shadow) == AlphaMode::Mask);
					CHECK(SelectMeshAlphaMode(AlphaMode::Blend, parameters, shadow) == AlphaMode::Blend);
				}
			}
			CHECK(SelectMeshAlphaMode(AlphaMode::Opaque, fading, false) == AlphaMode::Mask);
			CHECK(SelectMeshAlphaMode(AlphaMode::Opaque, fading, true) == AlphaMode::Opaque);
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

		// M23's milestone review (Docs/Decisions/0034-m23-contract.md decisions 2 and 8; plan §4.1: the instance record's
		// PreviousWorld, which M26's motion vectors read, is M23's frozen input): every pass that draws meshes writes its records
		// by MakeMeshInstanceRecord, the camera's draws (the Prepare stage) included, so a moved item's shadow-caster and
		// selection-mask records carry the previous frame's world matrix it gives, and a new item's (no PreviousWorld) its
		// current World, as its camera record does.
		TEST_CASE("RenderPrepare: the camera, shadow and selection records carry a moved item's previous world matrix and a new item's world matrix")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				RenderSnapshot snapshot = setup.Snapshot();
				MeshDrawItem moved = snapshot.Meshes.front();
				moved.PreviousWorld = glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f, 0.0f, -3.0f));
				MeshDrawItem added = snapshot.Meshes.front();
				added.World = glm::translate(glm::mat4(1.0f), glm::vec3(1.5f, 0.0f, -4.0f));
				added.Entity = UUID(901);
				added.PickId = 2;
				snapshot.Meshes = { moved, added };
				snapshot.SelectedEntities = { moved.Entity, added.Entity };
				// Each record of one pass's list is the moved item's or the new item's, by its World and pick id.
				const auto checkRecords = [&moved, &added](std::span<const InstanceRecord> records)
				{
					REQUIRE(records.size() == 2);
					uint32_t movedSeen = 0;
					uint32_t addedSeen = 0;
					for (const InstanceRecord& record : records)
					{
						if (record.EntityId == moved.PickId)
						{
							++movedSeen;
							CHECK(record.World == moved.World);
							CHECK(record.PreviousWorld == *moved.PreviousWorld);
						}
						else
						{
							++addedSeen;
							CHECK(record.EntityId == added.PickId);
							CHECK(record.World == added.World);
							CHECK(record.PreviousWorld == added.World);
						}
					}
					CHECK(movedSeen == 1);
					CHECK(addedSeen == 1);
				};
				const std::vector<InstanceRecord> camera = { MakeMeshInstanceRecord(moved, DrawFlagReceiveShadows), MakeMeshInstanceRecord(added) };
				checkRecords(camera);
				std::vector<InstanceRecord> shadow;
				const Result<ShadowCasterSet> casters = ShadowPass::PrepareCasters(setup.Cache(), snapshot.Meshes, true, shadow);
				REQUIRE_MESSAGE(casters.has_value(), casters.error().ToString());
				CHECK(casters->Draws.size() == 2);
				checkRecords(shadow);
				std::vector<InstanceRecord> selection;
				const SelectionDrawSet mask = SelectionPass::PrepareDraws(setup.Cache(), snapshot, true, selection);
				CHECK(mask.Draws.size() == 2);
				checkRecords(selection);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M31's acceptance (Docs/Decisions/0046-m31-material-parameters.md decision 4; World Tree's brief 5): the parameters
		// ride in the instance records, so 200 instances of one mesh and material with 200 different tints draw in as few
		// draws as 200 untinted ones: one instanced draw in each opaque pass.
		TEST_CASE("SceneRenderer: 200 tinted instances of one mesh and material draw in as few draws as untinted ones")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				RenderSnapshot snapshot = setup.Snapshot();
				const MeshDrawItem quad = snapshot.Meshes.front();
				snapshot.Meshes.clear();
				for (int index = 0; index < 200; ++index)
				{
					MeshDrawItem item = quad;
					item.World = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(index % 20) * 0.25f - 2.4f, static_cast<float>(index / 20) * 0.25f - 1.2f, -8.0f))
						* glm::scale(glm::mat4(1.0f), glm::vec3(0.1f));
					item.Entity = UUID(static_cast<uint64_t>(5000 + index));
					item.PickId = 0;
					snapshot.Meshes.push_back(std::move(item));
				}
				const auto draws = [&setup](std::string_view pass)
				{
					const RenderStats stats = setup.Renderer().GetRenderStats();
					const RenderPassStats* found = Test::SceneRendererIntegrationFixture::FindPass(stats, pass);
					return found != nullptr ? found->DrawCalls : 0U;
				};
				const Image untinted = setup.Render(snapshot);
				const uint32_t untintedForward = draws("ForwardOpaque");
				const uint32_t untintedDepth = draws("DepthNormal");
				for (size_t index = 0; index < snapshot.Meshes.size(); ++index)
				{
					const float step = static_cast<float>(index) / 199.0f;
					snapshot.Meshes[index].Parameters.Tint = glm::vec3(1.0f - step, step, 0.5f);
				}
				++snapshot.FrameIndex;
				const Image tinted = setup.Render(snapshot);
				CHECK(untintedForward == 1);
				CHECK(untintedDepth == 1);
				CHECK(draws("ForwardOpaque") == untintedForward);
				CHECK(draws("DepthNormal") == untintedDepth);
				CHECK(tinted.Pixels != untinted.Pixels);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M31 (decisions 2 and 5): the tint multiplies the base colour, the emission adds to the material's, and the offset
		// moves every map's texture coordinates: by half a checker period it swaps the squares, by a whole period it gives the
		// same image again.
		TEST_CASE("SceneRenderer: a tint multiplies an item's base colour, its emission adds and its offset moves its textures")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				RenderSnapshot snapshot = setup.Snapshot();
				const auto centre = [](const Image& image, size_t channel)
				{
					return Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, channel);
				};
				const Image plain = setup.Render(snapshot);
				REQUIRE(centre(plain, 1) > 8);
				snapshot.Meshes[0].Parameters.Tint = glm::vec3(1.0f, 0.0f, 0.0f);
				++snapshot.FrameIndex;
				const Image red = setup.Render(snapshot);
				CHECK(centre(red, 0) == centre(plain, 0));
				CHECK(centre(red, 1) < centre(plain, 1) / 2);
				CHECK(centre(red, 2) < centre(plain, 2) / 2);
				snapshot.Meshes[0].Parameters = {};
				snapshot.Meshes[0].Parameters.Emissive = glm::vec3(0.0f, 1.0f, 0.0f);
				++snapshot.FrameIndex;
				const Image glowing = setup.Render(snapshot);
				CHECK(centre(glowing, 1) > centre(plain, 1) + 64);
				CHECK(centre(glowing, 0) == centre(plain, 0));

				// The engine's checker texture (8 squares a side) as the base colour.
				auto checker = CreateRef<MaterialData>();
				checker->BaseColorMap = TypedAssetHandle<AssetType::Texture>(BuiltinAssetHandles::CheckerTexture);
				setup.Assets().Publish(UUID(8914), checker);
				snapshot.Meshes[0].Parameters = {};
				snapshot.Meshes[0].Materials = { UUID(8914) };
				const auto renderOffset = [&setup, &snapshot](float offset)
				{
					snapshot.Meshes[0].Parameters.UVOffset = glm::vec2(offset, 0.0f);
					++snapshot.FrameIndex;
					return setup.Render(snapshot);
				};
				const Image squares = renderOffset(0.0f);
				const Image swapped = renderOffset(0.125f);
				const Image period = renderOffset(0.25f);
				size_t changed = 0;
				int periodDelta = 0;
				for (size_t index = 0; index < squares.Pixels.size(); ++index)
				{
					const int before = std::to_integer<int>(squares.Pixels[index]);
					changed += std::abs(before - std::to_integer<int>(swapped.Pixels[index])) > 16 ? 1U : 0U;
					periodDelta = std::max(periodDelta, std::abs(before - std::to_integer<int>(period.Pixels[index])));
				}
				CAPTURE(changed);
				CAPTURE(periodDelta);
				CHECK(changed > squares.Pixels.size() / 8);
				CHECK(periodDelta <= 3);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M31 (decision 5): a dissolve clips the fragments whose noise lies below its amount (none at 0, about half at one
		// half, all at 1) through the Mask variants, which an Opaque material takes while it dissolves, and glows along the
		// edge; a fade dithers the share 1 - Opacity of the pixels away. The opaque passes keep one draw each.
		TEST_CASE("SceneRenderer: a dissolve and a fade clip an Opaque item's pixels through the Mask variants")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				RenderSnapshot snapshot = setup.Snapshot();
				// The quad 1.5 m away fills the middle of the view: the 24 x 24 pixels around the centre are all its own.
				snapshot.Meshes[0].World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.5f));
				const auto shownShare = [](const Image& image)
				{
					size_t shown = 0;
					for (uint32_t y = 20; y < 44; ++y)
						for (uint32_t x = 20; x < 44; ++x)
							shown += Test::SceneRendererIntegrationFixture::Channel(image, x, y, 1) > 2 ? 1U : 0U;
					return static_cast<double>(shown) / (24.0 * 24.0);
				};
				const auto render = [&setup, &snapshot](const MeshMaterialParameters& parameters)
				{
					snapshot.Meshes[0].Parameters = parameters;
					++snapshot.FrameIndex;
					return setup.Render(snapshot);
				};
				CHECK(shownShare(render({})) == 1.0);
				CHECK(shownShare(render({ .DissolveAmount = 1.0f })) == 0.0);
				// The noise repeated 8 times over the quad: the 24 x 24 pixels see about two of its tiles.
				const double half = shownShare(render({ .DissolveAmount = 0.5f, .DissolveNoiseScale = 8.0f }));
				CAPTURE(half);
				CHECK(half > 0.25);
				CHECK(half < 0.75);
				const RenderStats stats = setup.Renderer().GetRenderStats();
				const RenderPassStats* depth = Test::SceneRendererIntegrationFixture::FindPass(stats, "DepthNormal");
				REQUIRE(depth != nullptr);
				CHECK(depth->DrawCalls == 1);

				// The edge glows blue where the noise lies just above the amount.
				const Image edged = render({ .DissolveAmount = 0.4f, .DissolveEdge = glm::vec3(0.0f, 0.0f, 4.0f), .DissolveEdgeWidth = 0.2f, .DissolveNoiseScale = 8.0f });
				size_t blue = 0;
				for (uint32_t y = 20; y < 44; ++y)
					for (uint32_t x = 20; x < 44; ++x)
						blue += Test::SceneRendererIntegrationFixture::Channel(edged, x, y, 2) > Test::SceneRendererIntegrationFixture::Channel(edged, x, y, 1) + 64 ? 1U : 0U;
				CHECK(blue > 24);

				CHECK(shownShare(render({ .Opacity = 0.0f })) == 0.0);
				const double faded = shownShare(render({ .Opacity = 0.5f }));
				CAPTURE(faded);
				CHECK(faded > 0.4);
				CHECK(faded < 0.6);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M31 (decision 5): a dissolve removes its share of the item's shadow too (its casters take the Mask variant), while a
		// fade keeps the whole shadow: a faded wall still shades what it hides, as the brief's walls in front of the player do.
		TEST_CASE("SceneRenderer: a dissolved caster casts no shadow while a faded one keeps its whole shadow")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				RenderSnapshot snapshot = setup.Snapshot();
				// A wall 6 m away and a caster 4 m away left of the centre; the sun shines down -Z and to the right, so the caster's
				// shadow falls on the wall right of the centre, beside the caster, not behind it.
				const MeshDrawItem quad = snapshot.Meshes.front();
				MeshDrawItem wall = quad;
				wall.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(4.0f));
				// A dark caster, so the camera tells it from the lit wall behind it.
				auto dark = CreateRef<MaterialData>();
				dark->BaseColor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f);
				setup.Assets().Publish(UUID(8917), dark);
				MeshDrawItem caster = quad;
				caster.World = glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f, 0.0f, -4.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
				caster.Materials = { UUID(8917) };
				caster.Entity = UUID(40);
				caster.PickId = 2;
				snapshot.Meshes = { wall, caster };
				snapshot.Lights.push_back({ .Type = RenderLightType::Directional,
					.Intensity = 3.0f,
					.Direction = glm::normalize(glm::vec3(0.5f, 0.0f, -1.0f)),
					.CastShadows = true,
					.ShadowDistance = 20.0f });
				const auto render = [&setup, &snapshot](const MeshMaterialParameters& parameters)
				{
					snapshot.Meshes[1].Parameters = parameters;
					++snapshot.FrameIndex;
					return setup.Render(snapshot);
				};
				const auto green = [](const Image& image, uint32_t x)
				{
					return Test::SceneRendererIntegrationFixture::Channel(image, x, 32, 1);
				};
				// Pixel 37 sees the wall in the caster's shadow and pixel 25 the caster itself; the wall alone shows both lit.
				const Image shadowed = render({});
				const MeshDrawItem kept = snapshot.Meshes[1];
				snapshot.Meshes = { wall };
				++snapshot.FrameIndex;
				const Image bare = setup.Render(snapshot);
				snapshot.Meshes = { wall, kept };
				REQUIRE(green(shadowed, 37) + 24 < green(bare, 37));
				REQUIRE(std::abs(green(shadowed, 25) - green(bare, 25)) > 8);
				// Dissolved: neither the caster nor its shadow is left. Faded: the caster is gone and its whole shadow stays.
				const Image dissolved = render({ .DissolveAmount = 1.0f });
				CHECK(std::abs(green(dissolved, 37) - green(bare, 37)) <= 2);
				CHECK(std::abs(green(dissolved, 25) - green(bare, 25)) <= 2);
				const Image faded = render({ .Opacity = 0.0f });
				CHECK(green(faded, 37) == green(shadowed, 37));
				CHECK(std::abs(green(faded, 25) - green(bare, 25)) <= 2);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M31 (decision 5): a Blend material stays in the transparent pass: its Opacity scales its alpha (a true fade) and its
		// dissolved fragments blend nothing.
		TEST_CASE("SceneRenderer: a Blend material's opacity scales its alpha and its dissolve leaves nothing behind")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				auto glass = CreateRef<MaterialData>();
				glass->AlphaMode = AlphaMode::Blend;
				glass->Emissive = glm::vec3(1.0f);
				setup.Assets().Publish(UUID(8915), glass);
				RenderSnapshot snapshot = setup.Snapshot();
				snapshot.Meshes[0].Materials = { UUID(8915) };
				const auto render = [&setup, &snapshot](const MeshMaterialParameters& parameters)
				{
					snapshot.Meshes[0].Parameters = parameters;
					++snapshot.FrameIndex;
					return setup.Render(snapshot);
				};
				const auto centre = [](const Image& image)
				{
					return Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, 1);
				};
				const int solid = centre(render({}));
				const int half = centre(render({ .Opacity = 0.5f }));
				CAPTURE(solid);
				CAPTURE(half);
				CHECK(half > 16);
				CHECK(half < solid - 16);
				CHECK(centre(render({ .DissolveAmount = 1.0f })) == 0);
				const RenderStats stats = setup.Renderer().GetRenderStats();
				const RenderPassStats* transparent = Test::SceneRendererIntegrationFixture::FindPass(stats, "ForwardTransparent");
				REQUIRE(transparent != nullptr);
				CHECK(transparent->DrawCalls == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M31 (World Tree's brief 5, "they work on skinned meshes too"): a deformed item's draws carry its record's parameters
		// as any item's do.
		TEST_CASE("SceneRenderer: a skinned item takes its tint and its dissolve")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				// The fixture's quad skinned to one joint, which the palette moves 0.5 m right.
				auto skinned = CreateRef<MeshData>();
				skinned->Vertices = {
					{ .Position = { -1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 1 } },
					{ .Position = { 1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 1 } },
					{ .Position = { 1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 0 } },
					{ .Position = { -1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } },
				};
				skinned->Indices = { 0, 1, 2, 0, 2, 3 };
				skinned->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
				skinned->Submeshes = { { .IndexCount = 6, .Bounds = skinned->Bounds } };
				skinned->Slots = { {} };
				skinned->Skin = { MeshSkinVertex{}, MeshSkinVertex{}, MeshSkinVertex{}, MeshSkinVertex{} };
				skinned->SkinJointCount = 1;
				setup.Assets().Publish(UUID(8916), skinned);
				RenderSnapshot snapshot = setup.Snapshot();
				snapshot.Meshes[0].Mesh = UUID(8916);
				snapshot.Meshes[0].JointPalette = { glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f)) };
				const auto render = [&setup, &snapshot](const MeshMaterialParameters& parameters)
				{
					snapshot.Meshes[0].Parameters = parameters;
					++snapshot.FrameIndex;
					return setup.Render(snapshot);
				};
				const Image plain = render({});
				const Image red = render({ .Tint = glm::vec3(1.0f, 0.0f, 0.0f) });
				const Image gone = render({ .DissolveAmount = 1.0f });
				// Pixel 40 lies inside the moved quad.
				const auto at = [](const Image& image, size_t channel)
				{
					return Test::SceneRendererIntegrationFixture::Channel(image, 40, 32, channel);
				};
				REQUIRE(at(plain, 1) > 8);
				CHECK(at(red, 0) == at(plain, 0));
				CHECK(at(red, 1) < at(plain, 1) / 2);
				CHECK(at(gone, 1) == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
