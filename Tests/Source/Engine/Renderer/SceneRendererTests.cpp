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
#include <cstdlib>
#include <limits>
#include <utility>

// The scene renderer of the walking skeleton (Architecture §8.2, §8.3; Roadmap M7; Docs/Decisions/0012-m7-decisions.md
// decision 7), driven by synthetic snapshots.

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

		// Records the render of `snapshot` with `renderer`, executes it and returns what Render reported.
		Status Render(GraphicsDevice& device, SceneRenderer& renderer, const RenderSnapshot& snapshot)
		{
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			const Status rendered = renderer.Render(**commandList, snapshot);
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);
			return rendered;
		}

		Image ReadFinalImage(GraphicsDevice& device, SceneRenderer& renderer)
		{
			Readback readback(device);
			Result<Image> image = readback.ReadTexture(*renderer.GetFinalTexture());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			return std::move(*image);
		}

		// Renders `snapshot` with `renderer` and reads LdrColor back.
		Image RenderToImage(GraphicsDevice& device, SceneRenderer& renderer, const RenderSnapshot& snapshot)
		{
			const Status rendered = Render(device, renderer, snapshot);
			REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			return ReadFinalImage(device, renderer);
		}

		// The channel `channel` (0 red, 1 green, 2 blue) of the RGBA8 pixel at (x, y).
		int GetChannel(const Image& image, uint32_t x, uint32_t y, size_t channel)
		{
			return std::to_integer<int>(image.GetRow(y)[static_cast<size_t>(x) * 4 + channel]);
		}

		int GetRed(const Image& image, uint32_t x, uint32_t y)
		{
			return GetChannel(image, x, y, 0);
		}

		// The pipelines, a cache and a renderer of `width` x `height` over the fixture's device and asset manager.
		class RendererSetup
		{
		public:
			RendererSetup(Test::HeadlessGpuFixture& gpu, Test::AssetTestFixture& assets, uint32_t width, uint32_t height)
				: m_Cache(gpu.GetDevice(), assets.GetManager())
			{
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				m_Pipelines = std::move(*pipelines);
				Result<Scope<SceneRenderer>> renderer =
					SceneRenderer::Create(gpu.GetDevice(), *m_Pipelines, m_Cache, assets.GetManager(), { .Width = width, .Height = height });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				m_Renderer = std::move(*renderer);
			}

			[[nodiscard]] SceneRenderer& GetRenderer() { return *m_Renderer; }
		private:
			// Destroyed in reverse: the renderer, the pipelines, then the cache (§8.14 item 4).
			GpuResourceCache m_Cache;
			Scope<SceneRendererPipelines> m_Pipelines;
			Scope<SceneRenderer> m_Renderer;
		};

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRenderer: renders a lit cube from a synthetic snapshot" * doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("SceneRenderer: a snapshot without a camera clears and draws nothing" * doctest::test_suite(Test::GpuSuite))
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
				// Meshes without a camera are not drawn: the image is the default clear colour, sRGB-encoded.
				RenderSnapshot snapshot;
				snapshot.Meshes.push_back(MeshDrawItem{ .Mesh = BuiltinAssetHandles::CubeMesh });
				const Image image = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
				CHECK((*renderer)->GetLastStats().MeshDraws == 0);
				// CameraData's default ClearColor (0.05, 0.05, 0.06) through the sRGB OETF: 63, 63, 69.
				for (uint32_t y : { 0U, 8U, 15U })
				{
					for (uint32_t x : { 0U, 8U, 15U })
					{
						CAPTURE(x);
						CAPTURE(y);
						CHECK(std::abs(GetChannel(image, x, y, 0) - 63) <= 1);
						CHECK(std::abs(GetChannel(image, x, y, 1) - 63) <= 1);
						CHECK(std::abs(GetChannel(image, x, y, 2) - 69) <= 1);
						CHECK(GetChannel(image, x, y, 3) == 255);
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRendererPipelines: renderers of different sizes share one pipeline set" * doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("SceneRenderer: Resize rebuilds the targets and a render is deterministic" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				RendererSetup setup(gpu, assets, 24, 24);
				SceneRenderer& renderer = setup.GetRenderer();
				CHECK(renderer.GetWidth() == 24);
				CHECK(renderer.GetHeight() == 24);
				REQUIRE(renderer.Resize(40, 30).has_value());
				CHECK(renderer.GetWidth() == 40);
				CHECK(renderer.GetHeight() == 30);
				CHECK(renderer.GetFinalTexture()->getDesc().width == 40);
				// Resizing to the current size changes nothing.
				const nvrhi::ITexture* finalTexture = renderer.GetFinalTexture();
				REQUIRE(renderer.Resize(40, 30).has_value());
				CHECK(renderer.GetFinalTexture() == finalTexture);

				// §8.3: no temporal effects, so two renders of one snapshot are identical.
				const Image first = RenderToImage(gpu.GetDevice(), renderer, MakeCubeSnapshot(40, 30, 1.0f));
				const Image second = RenderToImage(gpu.GetDevice(), renderer, MakeCubeSnapshot(40, 30, 1.0f));
				CHECK(first.Width == 40);
				CHECK(first.Pixels == second.Pixels);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: submeshes outside the view are culled" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				RendererSetup setup(gpu, assets, 32, 32);
				RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 1.0f);
				// Behind the camera, far to the left, and beyond FarClip straight ahead.
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f)), .Mesh = BuiltinAssetHandles::CubeMesh });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(-100.0f, 0.0f, 0.0f)), .Mesh = BuiltinAssetHandles::CubeMesh });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -2000.0f)), .Mesh = BuiltinAssetHandles::CubeMesh });
				// A null mesh draws nothing and is not culled either.
				snapshot.Meshes.push_back(MeshDrawItem{});
				static_cast<void>(RenderToImage(gpu.GetDevice(), setup.GetRenderer(), snapshot));
				CHECK(setup.GetRenderer().GetLastStats().MeshDraws == 1);
				CHECK(setup.GetRenderer().GetLastStats().CulledSubmeshes == 3);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: a mirrored world matrix keeps the faces towards the camera" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				RendererSetup setup(gpu, assets, 32, 32);
				const Image plain = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), MakeCubeSnapshot(32, 32, 1.0f));
				// Mirrored in X: the triangles' winding flips on screen, and the lit +Z face must still be the one drawn.
				RenderSnapshot mirrored = MakeCubeSnapshot(32, 32, 1.0f);
				mirrored.Meshes[0].World = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
				const Image image = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), mirrored);
				CHECK(setup.GetRenderer().GetLastStats().MeshDraws == 1);
				CHECK(GetRed(image, 16, 16) == GetRed(plain, 16, 16));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: the material's base colour factor tints the surface" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				RendererSetup setup(gpu, assets, 32, 32);
				// The cube's default material is the built-in Default (white); the Error material's base colour is magenta.
				const Image white = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), MakeCubeSnapshot(32, 32, 1.0f));
				RenderSnapshot tinted = MakeCubeSnapshot(32, 32, 1.0f);
				tinted.Meshes[0].Materials = { BuiltinAssetHandles::ErrorMaterial };
				const Image magenta = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), tinted);
				CHECK(GetChannel(white, 16, 16, 1) > 0);
				CHECK(GetChannel(magenta, 16, 16, 0) == GetChannel(white, 16, 16, 0));
				CHECK(GetChannel(magenta, 16, 16, 1) == 0);
				CHECK(GetChannel(magenta, 16, 16, 2) == GetChannel(white, 16, 16, 2));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: exposure scales the linear colour before encoding" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				RendererSetup setup(gpu, assets, 16, 16);
				RenderSnapshot snapshot;
				snapshot.HasCamera = true;
				snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 1000.0f, 16, 16);
				snapshot.Camera.ClearColor = glm::vec3(0.25f, 0.5f, 0.0f);
				const Image plain = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), snapshot);
				snapshot.Post.ExposureEV = 1.0f;
				const Image brighter = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), snapshot);
				// sRGB(0.25) = 137 and sRGB(0.5) = 188; doubled: sRGB(0.5) = 188 and sRGB(1.0) = 255 (Linear: clamped).
				CHECK(std::abs(GetChannel(plain, 8, 8, 0) - 137) <= 1);
				CHECK(std::abs(GetChannel(plain, 8, 8, 1) - 188) <= 1);
				CHECK(std::abs(GetChannel(brighter, 8, 8, 0) - 188) <= 1);
				CHECK(GetChannel(brighter, 8, 8, 1) == 255);
				CHECK(GetChannel(brighter, 8, 8, 2) == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: a non-finite world matrix skips its draw and names the entity" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				RendererSetup setup(gpu, assets, 16, 16);
				RenderSnapshot snapshot = MakeCubeSnapshot(16, 16, 1.0f);
				MeshDrawItem broken{ .Mesh = BuiltinAssetHandles::CubeMesh, .Entity = UUID(0xabcdef0123456789ULL) };
				broken.World[3].x = std::numeric_limits<float>::quiet_NaN();
				snapshot.Meshes.push_back(broken);
				const Status rendered = Render(gpu.GetDevice(), setup.GetRenderer(), snapshot);
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(rendered.error().GetMessageText().contains("abcdef0123456789"));
				// The other draw still rendered.
				CHECK(setup.GetRenderer().GetLastStats().MeshDraws == 1);
				const Image image = ReadFinalImage(gpu.GetDevice(), setup.GetRenderer());
				CHECK(GetRed(image, 8, 8) > 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
