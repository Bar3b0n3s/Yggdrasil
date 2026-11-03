#include "TestsPCH.h"

#include "Engine/Renderer/LightClusters.h"

#include "Engine/Core/Random.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

// The clustered light loop (M23, C23.1, R-CLC; Docs/Decisions/0034-m23-contract.md decision 5, stream C).

namespace Engine {

	namespace {

		// A 60-degree perspective camera at the origin looking down -Z, clipping at 0.1 and 100 m.
		CameraData MakeCamera(uint32_t width, uint32_t height)
		{
			CameraData camera;
			camera.NearClip = 0.1f;
			camera.FarClip = 100.0f;
			camera.ViewportWidth = width;
			camera.ViewportHeight = height;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 100.0f, width, height);
			return camera;
		}

	}

	TEST_SUITE("Renderer")
	{
		// The grid covers the scene resolution with 64-pixel tiles (partial at the right and bottom edges) and the clip range
		// with 24 exponential slices; a pixel's cluster follows its tile and its depth's slice, clamped to the grid.
		TEST_CASE("LightClusters: the grid covers the view with 64-pixel tiles and 24 exponential slices")
		{
			const LightClusterGrid grid = ComputeLightClusterGrid(MakeCamera(1920, 1080), 1920, 1080);
			CHECK(grid.TilesX == 30);
			CHECK(grid.TilesY == 17);
			CHECK(grid.Slices == LightClusterDepthSlices);
			CHECK(grid.Near == 0.1f);
			CHECK(grid.Far == 100.0f);
			CHECK(GetLightClusterIndex(grid, 0, 0, 0.1f) == 0);
			CHECK(GetLightClusterIndex(grid, 63, 63, 0.1f) == 0);
			CHECK(GetLightClusterIndex(grid, 64, 0, 0.1f) == 1);
			CHECK(GetLightClusterIndex(grid, 0, 64, 0.1f) == 30);
			// Slice 12 of 24 starts at the geometric middle of the range, sqrt(0.1 * 100) = 3.16 m.
			CHECK(GetLightClusterIndex(grid, 0, 0, 3.0f) == 11 * 30 * 17);
			CHECK(GetLightClusterIndex(grid, 0, 0, 3.3f) == 12 * 30 * 17);
			// Beyond the far plane and before the near plane the slice clamps.
			CHECK(GetLightClusterIndex(grid, 1919, 1079, 1000.0f) == 24 * 30 * 17 - 1);
			CHECK(GetLightClusterIndex(grid, 1919, 1079, 0.01f) == 16 * 30 + 29);

			const LightClusterGrid partial = ComputeLightClusterGrid(MakeCamera(65, 1), 65, 1);
			CHECK(partial.TilesX == 2);
			CHECK(partial.TilesY == 1);
		}

		// A clip range that cannot be sliced (not finite and positive with Near < Far) gives one slice that holds every depth.
		TEST_CASE("LightClusters: a camera without a positive finite clip range puts every depth in slice 0")
		{
			CameraData camera = MakeCamera(128, 64);
			for (const std::pair<float, float>& clip : { std::pair{ 0.0f, 100.0f }, std::pair{ -1.0f, 100.0f }, std::pair{ 10.0f, 10.0f },
					 std::pair{ 0.1f, std::numeric_limits<float>::infinity() }, std::pair{ std::numeric_limits<float>::quiet_NaN(), 100.0f } })
			{
				camera.NearClip = clip.first;
				camera.FarClip = clip.second;
				CAPTURE(camera.NearClip);
				CAPTURE(camera.FarClip);
				const LightClusterGrid grid = ComputeLightClusterGrid(camera, 128, 64);
				CHECK(grid.TilesX == 2);
				CHECK(grid.TilesY == 1);
				CHECK(grid.SliceScale == 0.0f);
				CHECK(grid == ComputeLightClusterGrid(camera, 128, 64));
				for (const float depth : { 0.0f, 0.05f, 3.0f, 1.0e6f, std::numeric_limits<float>::quiet_NaN() })
					CHECK(GetLightClusterIndex(grid, 127, 63, depth) == 1);
			}
		}

		TEST_CASE("LightClusters: the LightCount view's palette follows its bands")
		{
			CHECK(GetLightCountDebugColor(0) == glm::vec3(0.0f));
			CHECK(GetLightCountDebugColor(2) == glm::vec3(0.0f, 0.0f, 1.0f));
			CHECK(GetLightCountDebugColor(3) == glm::vec3(0.0f, 1.0f, 1.0f));
			CHECK(GetLightCountDebugColor(8) == glm::vec3(0.0f, 1.0f, 0.0f));
			CHECK(GetLightCountDebugColor(16) == glm::vec3(1.0f, 1.0f, 0.0f));
			CHECK(GetLightCountDebugColor(32) == glm::vec3(1.0f, 0.5f, 0.0f));
			CHECK(GetLightCountDebugColor(33) == glm::vec3(1.0f, 0.0f, 0.0f));
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		// M23 acceptance (R-CLC, stream C): 256 seeded point and spot lights over a field of quads render byte-identically
		// through the clusters and through the brute-force loop over every light (RenderQualitySettings::ClusteredLights
		// false), and the clustered render records the LightClusters pass.
		TEST_CASE("SceneRenderer: clustered lighting equals the brute-force light loop exactly on 256 lights")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 256, 192);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			for (int row = 0; row < 4; ++row)
			{
				for (int column = 0; column < 6; ++column)
				{
					const glm::vec3 position(static_cast<float>(column) * 2.0f - 5.0f, static_cast<float>(row) * 2.0f - 3.0f, -6.0f - static_cast<float>(row));
					snapshot.Meshes.push_back({ .World = glm::translate(glm::mat4(1.0f), position), .Mesh = UUID(8910), .Materials = { UUID(8911) } });
				}
			}
			Random random(0x434C555354455253ULL);
			for (uint32_t index = 0; index < MaxVisibleLights; ++index)
			{
				const bool spot = index % 4 == 0;
				const glm::vec3 position(random.NextFloat() * 14.0f - 7.0f, random.NextFloat() * 10.0f - 5.0f, -2.0f - random.NextFloat() * 10.0f);
				snapshot.Lights.push_back({ .Type = spot ? RenderLightType::Spot : RenderLightType::Point,
					.Color = glm::vec3(random.NextFloat(), random.NextFloat(), random.NextFloat()),
					.Intensity = 0.5f + random.NextFloat() * 4.0f,
					.Position = position,
					.Direction = glm::vec3(0.0f, 0.0f, -1.0f),
					.Range = 0.5f + random.NextFloat() * 3.0f,
					.Entity = UUID(1000 + index) });
			}

			const Image clustered = setup.Render(snapshot);
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const bool recordedClusters = std::ranges::any_of(stats.Passes, [](const RenderPassStats& pass)
			{
				return pass.Name == std::string_view("LightClusters");
			});
			snapshot.Quality.ClusteredLights = false;
			++snapshot.FrameIndex;
			const Image bruteForce = setup.Render(snapshot);
			CHECK(recordedClusters);
			CHECK(clustered.Pixels == bruteForce.Pixels);
		}

		// The pass runs only where its masks are read: the Lit view's forward passes with a point or spot light in the list,
		// or the LightCount view. With directional lights alone, in a material debug view or the wireframe, without a camera
		// or with ClusteredLights off, the forward loop visits every light of the list and the pass is not recorded.
		TEST_CASE("SceneRenderer: the LightClusters pass runs only where the clusters are read")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { { .Type = RenderLightType::Directional, .Entity = UUID(30) },
				{ .Type = RenderLightType::Point, .Position = glm::vec3(0.0f, 0.0f, -2.0f), .Range = 3.0f, .Entity = UUID(31) } };
			const auto recordsClusters = [&setup, &snapshot]()
			{
				++snapshot.FrameIndex;
				setup.Submit(snapshot);
				return Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "LightClusters") != nullptr;
			};
			CHECK(recordsClusters());
			snapshot.DebugView = RenderDebugView::LightCount;
			CHECK(recordsClusters());
			snapshot.DebugView = RenderDebugView::Albedo;
			CHECK_FALSE(recordsClusters());
			snapshot.DebugView = RenderDebugView::Lit;
			snapshot.Flags = RenderViewFlags::Wireframe;
			CHECK_FALSE(recordsClusters());
			snapshot.Flags = RenderViewFlags::None;
			snapshot.Quality.ClusteredLights = false;
			CHECK_FALSE(recordsClusters());
			snapshot.Quality.ClusteredLights = true;
			snapshot.HasCamera = false;
			CHECK_FALSE(recordsClusters());
			snapshot.HasCamera = true;
			snapshot.Lights.pop_back();
			CHECK_FALSE(recordsClusters());
		}

		// The orthographic grid (the tiles' extents do not grow with depth): 64 seeded point and spot lights over a field of
		// quads at several depths render byte-identically through the clusters and through the brute-force loop.
		TEST_CASE("SceneRenderer: clustered lighting equals the brute-force light loop with an orthographic camera")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 256, 192);
			RenderSnapshot snapshot = setup.Snapshot(RenderProjection::Orthographic);
			snapshot.Camera.OrthographicSize = 4.0f;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 4.0f, 0.1f, 20.0f, 256, 192);
			snapshot.Meshes.clear();
			for (int row = 0; row < 4; ++row)
			{
				for (int column = 0; column < 6; ++column)
				{
					const glm::vec3 position(static_cast<float>(column) * 2.0f - 5.0f, static_cast<float>(row) * 2.0f - 3.0f, -4.0f - static_cast<float>(column));
					snapshot.Meshes.push_back({ .World = glm::translate(glm::mat4(1.0f), position), .Mesh = UUID(8910), .Materials = { UUID(8911) } });
				}
			}
			Random random(0x4F525448434C5553ULL);
			for (uint32_t index = 0; index < 64; ++index)
			{
				const bool spot = index % 3 == 0;
				const glm::vec3 position(random.NextFloat() * 12.0f - 6.0f, random.NextFloat() * 8.0f - 4.0f, -2.0f - random.NextFloat() * 10.0f);
				snapshot.Lights.push_back({ .Type = spot ? RenderLightType::Spot : RenderLightType::Point,
					.Color = glm::vec3(random.NextFloat(), random.NextFloat(), random.NextFloat()),
					.Intensity = 0.5f + random.NextFloat() * 4.0f,
					.Position = position,
					.Direction = glm::vec3(0.0f, 0.0f, -1.0f),
					.Range = 0.5f + random.NextFloat() * 2.0f,
					.Entity = UUID(2000 + index) });
			}
			const Image clustered = setup.Render(snapshot);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "LightClusters") != nullptr);
			snapshot.Quality.ClusteredLights = false;
			++snapshot.FrameIndex;
			const Image bruteForce = setup.Render(snapshot);
			CHECK(clustered.Pixels == bruteForce.Pixels);
		}
	}

}
