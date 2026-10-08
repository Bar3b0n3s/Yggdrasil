#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/AssetTestFixture.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

// The scene renderer of the walking skeleton (Architecture §8.2, §8.3; Roadmap M7). Skipped skeletons of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 7): stream B implements the renderer and removes the skips.

namespace Engine {

	namespace {

		// A perspective camera at (0, 0, 5) looking down -Z at the unit cube, whose +Z face fills the centre of the view, lit
		// by a directional light of `intensity` shining along -Z straight onto that face (N·L = 1 there).
		RenderSnapshot MakeCubeSnapshot(uint32_t width, uint32_t height, float intensity)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.Position = glm::vec3(0.0f, 0.0f, 5.0f);
			snapshot.Camera.View = glm::translate(glm::mat4(1.0f), -snapshot.Camera.Position);
			snapshot.Camera.ViewportWidth = width;
			snapshot.Camera.ViewportHeight = height;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 1000.0f, width, height);
			snapshot.Camera.ClearColor = glm::vec3(0.0f);
			snapshot.Meshes.push_back(MeshDrawItem{ .Mesh = BuiltinAssetHandles::CubeMesh });
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Intensity = intensity, .Direction = glm::vec3(0.0f, 0.0f, -1.0f) });
			snapshot.Post.ExposureEV = 0.0f;
			return snapshot;
		}

		// Renders `snapshot` with `renderer` and reads LdrColor back.
		Image RenderToImage(GraphicsDevice& device, SceneRenderer& renderer, const RenderSnapshot& snapshot)
		{
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			const Status rendered = renderer.Render(**commandList, snapshot);
			REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);

			Readback readback(device);
			Result<Image> image = readback.ReadTexture(*renderer.GetFinalTexture());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			return std::move(*image);
		}

		// The red channel of the RGBA8 pixel at (x, y).
		int GetRed(const Image& image, uint32_t x, uint32_t y)
		{
			return std::to_integer<int>(image.GetRow(y)[static_cast<size_t>(x) * 4]);
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRenderer: renders a lit cube from a synthetic snapshot" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets.GetManager(),
					{ .Width = 64, .Height = 64 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());

				const Image lit = RenderToImage(gpu.GetDevice(), **renderer, MakeCubeSnapshot(64, 64, 1.0f));
				CHECK((*renderer)->GetLastStats().MeshDraws == 1);
				CHECK((*renderer)->GetLastStats().Lights == 1);
				// The same view lit by the ambient term alone (a light of intensity 0).
				const Image ambient = RenderToImage(gpu.GetDevice(), **renderer, MakeCubeSnapshot(64, 64, 0.0f));

				// The corner shows the black clear colour; the face the light shines on is brighter than the ambient term alone,
				// which still lights it (RenderEnvironment::FallbackColor).
				CHECK(GetRed(lit, 0, 0) == 0);
				CHECK(GetRed(ambient, 32, 32) > 0);
				CHECK(GetRed(lit, 32, 32) > GetRed(ambient, 32, 32));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: a snapshot without a camera clears and draws nothing" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets.GetManager(),
					{ .Width = 16, .Height = 16 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				static_cast<void>(RenderToImage(gpu.GetDevice(), **renderer, RenderSnapshot{}));
				CHECK((*renderer)->GetLastStats().MeshDraws == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRendererPipelines: renderers of different sizes share one pipeline set" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				// §8.5: the pipeline count is the pass list's, whatever the number of views.
				CHECK((*pipelines)->GetPipelineCount() == SceneRendererPipelines::GetLayoutDescriptions().size());
				CHECK((*pipelines)->GetPipelineCount() > 0);

				Result<Scope<SceneRenderer>> small = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets.GetManager(),
					{ .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(small.has_value(), small.error().ToString());
				Result<Scope<SceneRenderer>> large = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets.GetManager(),
					{ .Width = 96, .Height = 48 });
				REQUIRE_MESSAGE(large.has_value(), large.error().ToString());
				const Image smallImage = RenderToImage(gpu.GetDevice(), **small, MakeCubeSnapshot(32, 32, 1.0f));
				const Image largeImage = RenderToImage(gpu.GetDevice(), **large, MakeCubeSnapshot(96, 48, 1.0f));
				CHECK(smallImage.Width == 32);
				CHECK(largeImage.Width == 96);
				CHECK(largeImage.Height == 48);
				CHECK(GetRed(smallImage, 16, 16) == GetRed(largeImage, 48, 24));
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
