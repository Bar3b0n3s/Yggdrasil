#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/RenderReference.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

// The scene renderer (Architecture §8.2 to §8.6; Roadmap M7 and M8): the walking skeleton's cases
// (Docs/Decisions/0012-m7-decisions.md decision 7) and M8's PBR forward passes, IBL, light list, debug views and normal maps
// (Docs/Decisions/0013-m8-decisions.md decisions 7, 12, 23 and 27), driven by synthetic snapshots.

namespace Engine {

	namespace {

		// Post-processing that maps linear values straight to the OETF: Linear, exposure 1, no bloom, FXAA or dither effects
		// on the tested pixels.
		PostProcessSettings MakeLinearPost()
		{
			return PostProcessSettings{ .ExposureEV = 0.0f, .Tonemap = RenderTonemapper::Linear, .BloomEnabled = false, .FxaaEnabled = false };
		}

		// A perspective camera at (0, 0, 5) looking down -Z at the unit cube, whose +Z face fills the centre of the view, lit
		// by a directional light of `intensity` shining along -Z straight onto that face (N·L = 1 there), with the linear
		// post-processing (M8: the tests check values that AgX, bloom and FXAA would change).
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
			snapshot.Post = MakeLinearPost();
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
			renderer.OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**commandList));
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

		// A constant environment of `radiance` (M8): both cubes filled with it (binary16) and the SH9 of a constant, so every
		// lookup returns `radiance` (the conventions of Asset/EnvironmentData.h).
		AssetRef<Asset> MakeConstantEnvironment(double radiance)
		{
			const uint16_t half = Test::DoubleToHalf(radiance);
			const uint16_t one = Test::DoubleToHalf(1.0);
			const auto fill = [half, one](uint32_t faceSize, uint32_t mipCount)
			{
				CubeMapData cube{ .FaceSize = faceSize, .MipCount = mipCount, .Texels = Buffer(ComputeCubeMapByteSize(faceSize, mipCount)) };
				for (size_t offset = 0; offset < cube.Texels.size(); offset += CubeMapData::BytesPerTexel)
				{
					const std::array<uint16_t, 4> texel = { half, half, half, one };
					std::memcpy(cube.Texels.data() + offset, texel.data(), sizeof(texel));
				}
				return cube;
			};
			EnvironmentData environment;
			environment.Skybox = fill(16, 5);
			environment.Specular = fill(EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount);
			// Y00 = 0.282095: c00 * Y00 = radiance.
			environment.IrradianceSH9[0] = glm::vec3(static_cast<float>(radiance / 0.28209479177387814));
			return CreateRef<EnvironmentData>(std::move(environment));
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
				snapshot.Post = MakeLinearPost();
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
				// The cube's default material is the built-in Default (white); the Error material's base colour (and emission) is
				// magenta. Red and blue saturate on both; green keeps only the dielectric specular reflection (F0 0.04, M8).
				const Image white = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), MakeCubeSnapshot(32, 32, 1.0f));
				RenderSnapshot tinted = MakeCubeSnapshot(32, 32, 1.0f);
				tinted.Meshes[0].Materials = { BuiltinAssetHandles::ErrorMaterial };
				const Image magenta = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), tinted);
				CHECK(GetChannel(white, 16, 16, 1) > 0);
				CHECK(GetChannel(magenta, 16, 16, 0) == GetChannel(white, 16, 16, 0));
				CHECK(GetChannel(magenta, 16, 16, 1) < GetChannel(white, 16, 16, 1) / 2);
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
				snapshot.Post = MakeLinearPost();
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

		// M8 (Roadmap M8; Docs/Decisions/0013-m8-decisions.md decisions 7 and 12): the PBR pass list.

		TEST_CASE("Pipelines: count matches the expected total" * doctest::test_suite(Test::GpuSuite))
		{
			// §8.5: "The total pipeline count is logged and asserted by a test." Mesh 24 + Skybox 1 + Bloom 3 + Tonemap 1 +
			// FXAA 1 + debug lines 2 + text 2 + DFG LUT 1 + shadows 6 + depth pyramid 2 + GTAO 2 + selection 8 + data views 1.
			// Each material debug view adds its 9 forward variants the first time it renders, 45 for the five.
			static_assert(SceneRendererPipelines::StartupPipelineCount == 54);
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				CHECK((*pipelines)->GetPipelineCount() == 54);
				CHECK(SceneRendererPipelines::GetLayoutDescriptions().size() == 54);
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets.GetManager(),
					{ .Width = 16, .Height = 16 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				RenderSnapshot snapshot = MakeCubeSnapshot(16, 16, 1.0f);
				for (const RenderDebugView view : { RenderDebugView::Albedo, RenderDebugView::Normals, RenderDebugView::Roughness,
						 RenderDebugView::Metallic, RenderDebugView::Emissive, RenderDebugView::Albedo })
				{
					snapshot.DebugView = view;
					static_cast<void>(RenderToImage(gpu.GetDevice(), **renderer, snapshot));
				}
				CHECK((*pipelines)->GetPipelineCount() == 54 + 5 * SceneRendererPipelines::DebugViewPipelineCount);
				// EnsureDebugView, which Render called, has no effect for Lit or for a view that exists.
				const Status lit = (*pipelines)->EnsureDebugView(RenderDebugView::Lit);
				const Status existing = (*pipelines)->EnsureDebugView(RenderDebugView::Albedo);
				CHECK(lit.has_value());
				CHECK(existing.has_value());
				CHECK((*pipelines)->GetPipelineCount() == 54 + 5 * SceneRendererPipelines::DebugViewPipelineCount);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: white furnace: white surfaces under a constant environment reflect it unchanged"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §15.3 "white furnace: BRDF energy ~ 1 with multi-scatter compensation": a white material (dielectric or metal, any
			// roughness) lit only by a constant environment of radiance 0.5 reflects 0.5, so the sphere's centre encodes as
			// round(255 * OETF(0.5)) = 188 within the dither and the test's energy tolerance.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle EnvironmentHandle{ 0x9301 };
			constexpr AssetHandle MaterialHandle{ 0x9302 };
			assets.Publish(EnvironmentHandle, MakeConstantEnvironment(0.5));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				for (const float metallic : { 0.0f, 1.0f })
				{
					for (const float roughness : { 0.1f, 0.5f, 1.0f })
					{
						CAPTURE(metallic);
						CAPTURE(roughness);
						MaterialData material;
						material.Metallic = metallic;
						material.Roughness = roughness;
						assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(material)));
						RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 0.0f);
						snapshot.Lights.clear();
						snapshot.Meshes.front() = MeshDrawItem{ .Mesh = BuiltinAssetHandles::SphereMesh, .Materials = { MaterialHandle } };
						snapshot.Environment = RenderEnvironment{ .Environment = EnvironmentHandle, .Intensity = 1.0f, .ShowSkybox = false };
						snapshot.Post = MakeLinearPost();
						const Image image = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
						CHECK(std::abs(GetRed(image, 16, 16) - 188) <= 3);
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: grey surfaces under a constant environment reflect the DFG LUT's compensated albedo"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The white furnace above cannot fail on the LUT's content (with f0 = 1 the compensation cancels DFG2, and a white
			// dielectric's diffuse makes up what its specular lacks), so this one shades a grey metal (f0 = 0.5) and a grey
			// dielectric (albedo 0.5, f0 = 0.04) under a constant environment L: the specular albedo is
			// ms = lerp(DFG1, DFG2, f0) (1 + f0 (1 / DFG2 - 1)) and the radiance L ms + L albedo (1 - ms). The flat +Z face of
			// the cube at the centre of a 33x33 view has NdotV = 1, where the LUT clamps to its last column (NdotV 127.5 / 128),
			// and no specular anti-aliasing widening; the roughnesses sit on LUT rows, so the CPU reference reads the texels
			// the shader samples.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle EnvironmentHandle{ 0x9303 };
			constexpr AssetHandle MaterialHandle{ 0x9304 };
			constexpr double Radiance = 0.5;
			constexpr uint32_t Size = 33;
			assets.Publish(EnvironmentHandle, MakeConstantEnvironment(Radiance));
			constexpr double LutSize = BrdfLut::Size;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = Size, .Height = Size });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				// Rows 32 and 64 of the LUT, and roughness 1, which the sampler clamps to row 127.
				for (const double row : { 32.0, 64.0, 127.0 })
				{
					for (const float metallic : { 1.0f, 0.0f })
					{
						const double lutRoughness = (row + 0.5) / LutSize;
						const float roughness = row == 127.0 ? 1.0f : static_cast<float>(lutRoughness);
						CAPTURE(roughness);
						CAPTURE(metallic);
						MaterialData material;
						material.BaseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
						material.Metallic = metallic;
						material.Roughness = roughness;
						assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(material)));
						RenderSnapshot snapshot = MakeCubeSnapshot(Size, Size, 0.0f);
						snapshot.Lights.clear();
						snapshot.Meshes.front().Materials = { MaterialHandle };
						snapshot.Environment = RenderEnvironment{ .Environment = EnvironmentHandle, .Intensity = 1.0f, .ShowSkybox = false };
						const Image image = RenderToImage(gpu.GetDevice(), **renderer, snapshot);

						const glm::dvec2 dfg = Test::ComputeDfg((LutSize - 0.5) / LutSize, lutRoughness, BrdfLut::LutSampleCount);
						const double f0 = metallic == 1.0f ? 0.5 : 0.04;
						const double albedo = metallic == 1.0f ? 0.0 : 0.5;
						const double specularAlbedo = (dfg.x + (dfg.y - dfg.x) * f0) * (1.0 + f0 * (1.0 / dfg.y - 1.0));
						const double radiance = Radiance * specularAlbedo + Radiance * albedo * (1.0 - specularAlbedo);
						const int expected = static_cast<int>(std::lround(255.0 * Test::LinearToSrgb(radiance)));
						CAPTURE(expected);
						CHECK(std::abs(GetRed(image, Size / 2, Size / 2) - expected) <= 2);
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: a directional light shades head-on as the CPU BRDF does" * doctest::test_suite(Test::GpuSuite))
		{
			// The direct-light BRDF on the GPU against RenderReference.h: the flat +Z face at the centre of a 33x33 view, lit
			// head-on by a directional light of intensity I (n = v = l = h, so NdotV = NdotL = NdotH = VdotH = 1), no ambient
			// and no environment. Lighting.slang's artist units give radiance pi I (diffuse / pi + F D V comp), with
			// comp = 1 + f0 (1 / DFG2 - 1) from the LUT's last column.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle MaterialHandle{ 0x9305 };
			constexpr uint32_t Size = 33;
			constexpr double Intensity = 0.2;
			constexpr double LutSize = BrdfLut::Size;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = Size, .Height = Size });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				for (const double row : { 64.0, 127.0 })
				{
					for (const float metallic : { 1.0f, 0.0f })
					{
						const double lutRoughness = (row + 0.5) / LutSize;
						const float roughness = row == 127.0 ? 1.0f : static_cast<float>(lutRoughness);
						CAPTURE(roughness);
						CAPTURE(metallic);
						MaterialData material;
						material.BaseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
						material.Metallic = metallic;
						material.Roughness = roughness;
						assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(material)));
						RenderSnapshot snapshot = MakeCubeSnapshot(Size, Size, static_cast<float>(Intensity));
						snapshot.Environment.FallbackColor = glm::vec3(0.0f);
						snapshot.Meshes.front().Materials = { MaterialHandle };
						const Image image = RenderToImage(gpu.GetDevice(), **renderer, snapshot);

						const double alpha = static_cast<double>(roughness) * roughness;
						const double f0 = metallic == 1.0f ? 0.5 : 0.04;
						const double diffuse = metallic == 1.0f ? 0.0 : 0.5;
						const glm::dvec2 dfg = Test::ComputeDfg((LutSize - 0.5) / LutSize, lutRoughness, BrdfLut::LutSampleCount);
						const double compensation = 1.0 + f0 * (1.0 / dfg.y - 1.0);
						const double specular = Test::FresnelSchlick(glm::dvec3(f0), 1.0).x * Test::DistributionGgx(1.0, alpha)
							* Test::VisibilitySmithGgxCorrelated(1.0, 1.0, alpha) * compensation;
						const double radiance = glm::pi<double>() * Intensity * (diffuse / glm::pi<double>() + specular);
						REQUIRE(radiance < 1.0); // the Linear tonemapper would clamp it
						const int expected = static_cast<int>(std::lround(255.0 * Test::LinearToSrgb(radiance)));
						CAPTURE(expected);
						CHECK(std::abs(GetRed(image, Size / 2, Size / 2) - expected) <= 2);
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: a huge environment intensity saturates instead of turning black" * doctest::test_suite(Test::GpuSuite))
		{
			// EnvironmentComponent.Intensity has no useful upper bound for the shading (Docs/Decisions/0013-m8-decisions.md
			// decision 27): the renderer clamps it to the largest binary16 value, so the constants and the shader's products
			// stay finite and a white metal (no diffuse, whose 0 would turn an infinite irradiance into NaN) saturates to
			// white, with an environment map and with the fallback ambient alike.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle EnvironmentHandle{ 0x9306 };
			constexpr AssetHandle MaterialHandle{ 0x9307 };
			assets.Publish(EnvironmentHandle, MakeConstantEnvironment(100.0));
			MaterialData metal;
			metal.Metallic = 1.0f;
			assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(metal)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 0.0f);
				snapshot.Lights.clear();
				snapshot.Meshes.front().Materials = { MaterialHandle };
				snapshot.Environment = RenderEnvironment{ .Environment = EnvironmentHandle, .Intensity = 1e37f, .ShowSkybox = false };
				const Image mapped = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
				snapshot.Environment = RenderEnvironment{ .Intensity = std::numeric_limits<float>::max(), .ShowSkybox = false, .FallbackColor = glm::vec3(1.0f) };
				const Image ambient = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
				for (size_t channel = 0; channel < 3; ++channel)
				{
					CAPTURE(channel);
					CHECK(GetChannel(mapped, 16, 16, channel) == 255);
					CHECK(GetChannel(ambient, 16, 16, channel) == 255);
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: the forward pass leaves no hole where the prepass covered a transformed sphere"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The forward pass tests against the prepass depth with GreaterOrEqual and no writes, so both pipelines must compute
			// bit-identical positions (Scene.slang's MultiplyExact): one ULP less depth in the forward pass would leave the clear
			// colour inside the silhouette. A non-uniformly scaled, rotated and offset sphere is convex on screen, so every row
			// and column of its red emission must be one unbroken run over the blue clear colour.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle MaterialHandle{ 0x9308 };
			constexpr uint32_t Size = 128;
			MaterialData red;
			red.BaseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			red.Emissive = glm::vec3(1.0f, 0.0f, 0.0f);
			assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(red)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = Size, .Height = Size });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				RenderSnapshot snapshot = MakeCubeSnapshot(Size, Size, 0.0f);
				snapshot.Lights.clear();
				snapshot.Camera.ClearColor = glm::vec3(0.0f, 0.0f, 1.0f);
				const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.137f, -0.071f, 0.413f))
					* glm::rotate(glm::mat4(1.0f), 0.7123f, glm::normalize(glm::vec3(0.31f, 0.73f, 0.19f)))
					* glm::scale(glm::mat4(1.0f), glm::vec3(2.74f, 1.66f, 2.22f));
				snapshot.Meshes = { MeshDrawItem{ .World = world, .Mesh = BuiltinAssetHandles::SphereMesh, .Materials = { MaterialHandle } } };
				const Image image = RenderToImage(gpu.GetDevice(), **renderer, snapshot);

				const auto isCovered = [&image](uint32_t x, uint32_t y)
				{
					return GetRed(image, x, y) > 128;
				};
				uint32_t covered = 0;
				uint32_t brokenRuns = 0;
				for (uint32_t line = 0; line < Size; ++line)
				{
					// Along row `line`, then along column `line`: covered pixels form one run (entering and leaving at most once).
					for (const bool alongRow : { true, false })
					{
						uint32_t transitions = 0;
						bool previous = false;
						for (uint32_t index = 0; index < Size; ++index)
						{
							const bool current = alongRow ? isCovered(index, line) : isCovered(line, index);
							covered += alongRow && current ? 1U : 0U;
							transitions += current != previous ? 1U : 0U;
							previous = current;
						}
						transitions += previous ? 1U : 0U; // a run that reaches the edge ends there
						brokenRuns += transitions > 2 ? 1U : 0U;
					}
				}
				CHECK(covered > Size * Size / 16); // the sphere fills a good part of the view
				CHECK(brokenRuns == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: Mask discards below the cutoff and Blend composites over the opaque scene"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle MaskHandle{ 0x9311 };
			constexpr AssetHandle BlendHandle{ 0x9312 };
			MaterialData mask;
			mask.AlphaMode = AlphaMode::Mask;
			mask.AlphaCutoff = 0.5f;
			mask.BaseColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.25f);
			assets.Publish(MaskHandle, CreateRef<MaterialData>(std::move(mask)));
			MaterialData blend;
			blend.AlphaMode = AlphaMode::Blend;
			blend.BaseColor = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f);
			blend.Emissive = glm::vec3(1.0f, 0.0f, 0.0f);
			blend.Metallic = 0.0f;
			assets.Publish(BlendHandle, CreateRef<MaterialData>(std::move(blend)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());

				// A Mask cube whose alpha is below the cutoff leaves the clear colour.
				RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 1.0f);
				snapshot.Camera.ClearColor = glm::vec3(0.0f, 0.0f, 1.0f);
				snapshot.Meshes.front().Materials = { MaskHandle };
				snapshot.Post = MakeLinearPost();
				const Image masked = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
				CHECK(GetChannel(masked, 16, 16, 2) == 255);
				CHECK(GetRed(masked, 16, 16) == 0);
				CHECK((*renderer)->GetLastStats().MeshDraws == 1);

				// A Blend cube half covers the blue clear colour with its red emission.
				snapshot.Meshes.front().Materials = { BlendHandle };
				const Image blended = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
				CHECK(GetRed(blended, 16, 16) > 100);
				CHECK(GetChannel(blended, 16, 16, 2) > 100);
				CHECK((*renderer)->GetLastStats().TransparentDraws == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: each debug view outputs its quantity" * doctest::test_suite(Test::GpuSuite))
		{
			// The cube's +Z face fills the centre: base colour (1, 0, 0), metallic 0.25, roughness 0.75, emissive (0, 0.5, 0).
			// Colour views are encoded (Albedo red 255, Emissive green round(255 * OETF(0.5)) = 188); data views are stored as
			// round(255 v) (Metallic 64, Roughness 191, the +Z normal (0, 0, 1) as (128, 128, 255)).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle MaterialHandle{ 0x9321 };
			MaterialData material;
			material.BaseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
			material.Metallic = 0.25f;
			material.Roughness = 0.75f;
			material.Emissive = glm::vec3(0.0f, 0.5f, 0.0f);
			assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(material)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 1.0f);
				snapshot.Meshes.front().Materials = { MaterialHandle };
				const auto centre = [&](RenderDebugView view)
				{
					snapshot.DebugView = view;
					const Image image = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
					return glm::ivec3(GetChannel(image, 16, 16, 0), GetChannel(image, 16, 16, 1), GetChannel(image, 16, 16, 2));
				};
				// Each view renders outside its CHECK: the helpers REQUIRE a successful render and readback.
				const glm::ivec3 albedo = centre(RenderDebugView::Albedo);
				const glm::ivec3 metallic = centre(RenderDebugView::Metallic);
				const glm::ivec3 roughness = centre(RenderDebugView::Roughness);
				const glm::ivec3 normals = centre(RenderDebugView::Normals);
				const glm::ivec3 emissive = centre(RenderDebugView::Emissive);
				CHECK(albedo == glm::ivec3(255, 0, 0));
				CHECK(metallic == glm::ivec3(64, 64, 64));
				CHECK(roughness == glm::ivec3(191, 191, 191));
				CHECK(normals == glm::ivec3(128, 128, 255));
				CHECK(emissive == glm::ivec3(0, 188, 0));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: more visible lights than the limit shade the most important and warn once"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				RendererSetup setup(gpu, assets, 16, 16);
				RenderSnapshot snapshot = MakeCubeSnapshot(16, 16, 1.0f);
				for (uint32_t index = 0; index < MaxVisibleLights + 4; ++index)
				{
					snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Point,
						.Intensity = 1.0f,
						.Position = glm::vec3(0.0f, 0.0f, 1.0f),
						.Range = 5.0f,
						.Entity = UUID(index + 1) });
				}
				Test::ExpectLog warning(LogLevel::Warn, "RENDER_LIGHT_LIMIT_EXCEEDED");
				static_cast<void>(RenderToImage(gpu.GetDevice(), setup.GetRenderer(), snapshot));
				static_cast<void>(RenderToImage(gpu.GetDevice(), setup.GetRenderer(), snapshot));
				CHECK(warning.GetMatchCount() == 1); // once per renderer
				CHECK(setup.GetRenderer().GetLastStats().Lights == MaxVisibleLights);
				CHECK(setup.GetRenderer().GetLastStats().DroppedLights == 5);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: point and spot lights shade within their range and cone" * doctest::test_suite(Test::GpuSuite))
		{
			// The light list (t0, Shared/ShaderLight.h) read by the forward pass: only the snapshot's lights shade the cube (no
			// ambient, the directional light at intensity 0 is culled), so a light that does not reach the +Z face leaves it
			// black.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				RendererSetup setup(gpu, assets, 32, 32);
				RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 0.0f);
				snapshot.Environment.FallbackColor = glm::vec3(0.0f);
				const auto centre = [&](const LightData& light)
				{
					snapshot.Lights = { light };
					const Image image = RenderToImage(gpu.GetDevice(), setup.GetRenderer(), snapshot);
					return GetRed(image, 16, 16);
				};
				const glm::vec3 front(0.0f, 0.0f, 2.0f); // 1.5 m in front of the +Z face
				const int inRange = centre(LightData{ .Type = RenderLightType::Point, .Position = front, .Range = 5.0f });
				CHECK(setup.GetRenderer().GetLastStats().Lights == 1);
				const int outOfRange = centre(LightData{ .Type = RenderLightType::Point, .Position = front, .Range = 1.0f });
				const int towards = centre(LightData{ .Type = RenderLightType::Spot, .Position = front, .Direction = glm::vec3(0.0f, 0.0f, -1.0f), .Range = 5.0f });
				const int away = centre(LightData{ .Type = RenderLightType::Spot, .Position = front, .Direction = glm::vec3(0.0f, 0.0f, 1.0f), .Range = 5.0f });
				CHECK(setup.GetRenderer().GetLastStats().Lights == 1); // its cone's sphere holds the camera: not culled
				CHECK(inRange > 100);
				CHECK(outOfRange == 0);
				CHECK(towards == inRange); // the centre is on the cone's axis: no cone attenuation
				CHECK(away == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: double-sided materials draw back faces, lit with flipped normals" * doctest::test_suite(Test::GpuSuite))
		{
			// The quad faces +Z; turned half a turn about Y it shows the camera its back, which only a double-sided material
			// draws (CullNone), shaded with the normal flipped towards the camera and its light. A mirrored world draws through
			// the CullFront variants and SV_IsFrontFace inverted, with the same result.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle SingleSidedHandle{ 0x9331 };
			constexpr AssetHandle DoubleSidedHandle{ 0x9332 };
			assets.Publish(SingleSidedHandle, CreateRef<MaterialData>());
			MaterialData doubleSided;
			doubleSided.DoubleSided = true;
			assets.Publish(DoubleSidedHandle, CreateRef<MaterialData>(std::move(doubleSided)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 1.0f);
				snapshot.Environment.FallbackColor = glm::vec3(0.0f);
				const glm::mat4 turned = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
				const glm::mat4 mirrored = turned * glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
				const auto centre = [&](const glm::mat4& world, AssetHandle material)
				{
					snapshot.Meshes = { MeshDrawItem{ .World = world, .Mesh = BuiltinAssetHandles::QuadMesh, .Materials = { material } } };
					const Image image = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
					return GetRed(image, 16, 16);
				};
				// Each render runs outside its CHECK: the helpers REQUIRE a successful render and readback.
				const int turnedSingle = centre(turned, SingleSidedHandle);
				const int mirroredSingle = centre(mirrored, SingleSidedHandle);
				const int turnedDouble = centre(turned, DoubleSidedHandle);
				const int mirroredDouble = centre(mirrored, DoubleSidedHandle);
				CHECK(turnedSingle == 0);
				CHECK(mirroredSingle == 0);
				// Lit head-on by the directional light of intensity 1: a white Lambertian surface of radiance 1.
				CHECK(turnedDouble == 255);
				CHECK(mirroredDouble == 255);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: an emissive map multiplies the emissive factor, which emits alone without a map" * doctest::test_suite(Test::GpuSuite))
		{
			// An empty emissive slot binds Black (§8.4), so the shader takes the factor alone then (MaterialFlagEmissiveMap).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle TextureHandle{ 0x9341 };
			constexpr AssetHandle UnmappedHandle{ 0x9342 };
			constexpr AssetHandle MappedHandle{ 0x9343 };
			TextureData black;
			black.Format = TextureFormat::RGBA8Srgb;
			black.Width = 1;
			black.Height = 1;
			black.Mips.push_back({ .Width = 1, .Height = 1, .Offset = 0, .Size = 4 });
			black.Pixels = { std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 } };
			assets.Publish(TextureHandle, CreateRef<TextureData>(std::move(black)));
			MaterialData unmapped;
			unmapped.Emissive = glm::vec3(0.0f, 1.0f, 0.0f);
			MaterialData mapped = unmapped;
			mapped.EmissiveMap = TypedAssetHandle<AssetType::Texture>(TextureHandle);
			assets.Publish(UnmappedHandle, CreateRef<MaterialData>(std::move(unmapped)));
			assets.Publish(MappedHandle, CreateRef<MaterialData>(std::move(mapped)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 1.0f);
				snapshot.DebugView = RenderDebugView::Emissive;
				snapshot.Meshes.front().Materials = { UnmappedHandle };
				const Image alone = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
				snapshot.Meshes.front().Materials = { MappedHandle };
				const Image masked = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
				CHECK(GetChannel(alone, 16, 16, 1) == 255);
				CHECK(GetChannel(masked, 16, 16, 1) == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: a normal map tilts the shading normal in the tangent frame the world matrix carries"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §8.5: the map's tangent-space normal 2 * texel - 1, its xy scaled by NormalScale, is expressed in the frame of the
			// transformed vertex: the tangent, the bitangent cross(normal, tangent) times the tangent's sign, and the normal. The
			// cube's +Z face has the tangent +X (Asset/BuiltinMeshes.cpp: every face shows its texture upright seen from
			// outside); the Normals debug view stores the world-space shading normal n as round(255 (0.5 n + 0.5)).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle NormalMapHandle{ 0x9351 };
			constexpr AssetHandle FullHandle{ 0x9352 };
			constexpr AssetHandle HalfHandle{ 0x9353 };
			constexpr std::array<uint8_t, 3> Texel = { 204, 128, 230 }; // tilted towards +tangent
			TextureData normalMap;
			normalMap.Format = TextureFormat::RGBA8Unorm;
			normalMap.Width = 1;
			normalMap.Height = 1;
			normalMap.Mips.push_back({ .Width = 1, .Height = 1, .Offset = 0, .Size = 4 });
			normalMap.Pixels = { std::byte{ Texel[0] }, std::byte{ Texel[1] }, std::byte{ Texel[2] }, std::byte{ 255 } };
			assets.Publish(NormalMapHandle, CreateRef<TextureData>(std::move(normalMap)));
			MaterialData full;
			full.NormalMap = TypedAssetHandle<AssetType::Texture>(NormalMapHandle);
			MaterialData half = full;
			half.NormalScale = 0.5f;
			assets.Publish(FullHandle, CreateRef<MaterialData>(std::move(full)));
			assets.Publish(HalfHandle, CreateRef<MaterialData>(std::move(half)));

			// The expected encoded normal for the face's world tangent and bitangent (its normal stays +Z) and a NormalScale.
			const glm::dvec3 tangentNormal = glm::dvec3(Texel[0], Texel[1], Texel[2]) / 255.0 * 2.0 - 1.0;
			const auto expected = [&tangentNormal](const glm::dvec3& tangent, const glm::dvec3& bitangent, double scale)
			{
				const glm::dvec3 normal =
					glm::normalize(tangentNormal.x * scale * tangent + tangentNormal.y * scale * bitangent + tangentNormal.z * glm::dvec3(0.0, 0.0, 1.0));
				return glm::ivec3(glm::round((normal * 0.5 + 0.5) * 255.0));
			};
			const auto checkNear = [](const glm::ivec3& actual, const glm::ivec3& wanted)
			{
				INFO("actual (", actual.x, ", ", actual.y, ", ", actual.z, "), expected (", wanted.x, ", ", wanted.y, ", ", wanted.z, ")");
				CHECK(std::abs(actual.x - wanted.x) <= 1);
				CHECK(std::abs(actual.y - wanted.y) <= 1);
				CHECK(std::abs(actual.z - wanted.z) <= 1);
			};
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				RenderSnapshot snapshot = MakeCubeSnapshot(32, 32, 1.0f);
				snapshot.DebugView = RenderDebugView::Normals;
				const auto centre = [&](AssetHandle material, const glm::mat4& world)
				{
					snapshot.Meshes.front().Materials = { material };
					snapshot.Meshes.front().World = world;
					const Image image = RenderToImage(gpu.GetDevice(), **renderer, snapshot);
					return glm::ivec3(GetChannel(image, 16, 16, 0), GetChannel(image, 16, 16, 1), GetChannel(image, 16, 16, 2));
				};
				// Each render runs outside its CHECK: the helpers REQUIRE a successful render and readback.
				const glm::ivec3 unrotated = centre(FullHandle, glm::mat4(1.0f));
				const glm::ivec3 halfScale = centre(HalfHandle, glm::mat4(1.0f));
				// Rolled a quarter turn about +Z: the +Z face still faces the camera, its tangent is now +Y and its bitangent -X.
				const glm::ivec3 rolled = centre(FullHandle, glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)));
				checkNear(unrotated, expected(glm::dvec3(1.0, 0.0, 0.0), glm::dvec3(0.0, 1.0, 0.0), 1.0));
				checkNear(halfScale, expected(glm::dvec3(1.0, 0.0, 0.0), glm::dvec3(0.0, 1.0, 0.0), 0.5));
				checkNear(rolled, expected(glm::dvec3(0.0, 1.0, 0.0), glm::dvec3(-1.0, 0.0, 0.0), 1.0));
				CHECK(unrotated != halfScale);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("SceneRenderer: directional and spot shadows affect receivers and honor both shadow flags")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			for (const auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
				for (const auto type : { RenderLightType::Directional, RenderLightType::Spot })
				{
					CAPTURE(static_cast<uint32_t>(projection));
					CAPTURE(static_cast<uint32_t>(type));
					auto snapshot = fixture.Snapshot(projection);
					snapshot.Environment.FallbackColor = glm::vec3(0);
					snapshot.Meshes[0].World = glm::translate(glm::mat4(1), glm::vec3(0, 0, -5)) * glm::scale(glm::mat4(1), glm::vec3(3, 3, 1));
					snapshot.Meshes[0].CastShadows = false;
					auto blocker = snapshot.Meshes[0];
					blocker.Entity = UUID(40);
					blocker.PickId = 2;
					blocker.CastShadows = true;
					blocker.World = glm::translate(glm::mat4(1), glm::vec3(type == RenderLightType::Directional ? 0.7f : 0.4f, 0, -3))
						* glm::scale(glm::mat4(1), glm::vec3(0.3f));
					snapshot.Meshes.push_back(blocker);
					LightData shadow;
					shadow.Type = type;
					shadow.Position = { 1, 0, 0 };
					shadow.Direction = glm::normalize(type == RenderLightType::Directional ? glm::vec3(-0.35f, 0, -1) : glm::vec3(-1, 0, -5));
					shadow.Intensity = type == RenderLightType::Directional ? 0.5f : 100.0f;
					shadow.CastShadows = true;
					shadow.ShadowDistance = 10;
					shadow.CascadeCount = 1;
					shadow.DepthBias = shadow.NormalBias = 0;
					shadow.LightAngle = 0;
					shadow.SourceRadius = 0;
					shadow.Entity = UUID(22);
					LightData culled;
					culled.Intensity = 0;
					// The shadowed light's snapshot index differs from its uploaded index.
					snapshot.Lights = { culled, shadow };
					const auto shaded = fixture.Render(snapshot);
					snapshot.Meshes[0].ReceiveShadows = false;
					const auto unreceived = fixture.Render(snapshot);
					snapshot.Meshes[0].ReceiveShadows = true;
					snapshot.Meshes[1].CastShadows = false;
					const auto uncast = fixture.Render(snapshot);
					const int dark = Test::SceneRendererIntegrationFixture::Channel(shaded, 32, 32);
					const int lit = Test::SceneRendererIntegrationFixture::Channel(unreceived, 32, 32);
					CAPTURE(dark);
					CAPTURE(lit);
					CHECK(lit > dark + 20);
					CHECK(std::abs(lit - Test::SceneRendererIntegrationFixture::Channel(uncast, 32, 32)) <= 1);
					const auto stats = fixture.Renderer().GetRenderStats();
					CHECK(stats.ShadowDraws == 0);
				}
		}

		TEST_CASE("SceneRenderer: GTAO shades only indirect light and data views show the final occlusion")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 67, 49);
			MaterialData grey;
			grey.BaseColor = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f); // White's multibounce compensation can remove diffuse darkening.
			fixture.Assets().Publish(UUID(8911), CreateRef<MaterialData>(grey));
			for (const auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				auto snapshot = fixture.Snapshot(projection);
				snapshot.Meshes[0].World = glm::translate(glm::mat4(1), glm::vec3(0, 0, -3)) * glm::scale(glm::mat4(1), glm::vec3(2));
				auto raised = snapshot.Meshes[0];
				raised.Entity = UUID(40);
				raised.World = glm::translate(glm::mat4(1), glm::vec3(0.4f, 0, -2.8f)) * glm::scale(glm::mat4(1), glm::vec3(0.35f));
				snapshot.Meshes.push_back(raised);
				snapshot.DebugView = RenderDebugView::AO;
				snapshot.Post.ExposureEV = 8;
				snapshot.Post.SsaoIntensity = 4; // Make the low-contrast orthographic contact exceed RGBA8 quantization after multibounce.
				const auto disabled = fixture.Render(snapshot);
				for (size_t pixel = 0; pixel < disabled.Pixels.size(); pixel += 4)
					CHECK(disabled.Pixels[pixel] == std::byte{ 255 });
				for (const auto quality : { RenderSsaoQuality::Low, RenderSsaoQuality::Medium, RenderSsaoQuality::High })
				{
					snapshot.Post.SsaoQuality = quality;
					snapshot.Post.SsaoEnabled = true;
					snapshot.Quality.SsaoHalfResolution = quality == RenderSsaoQuality::High;
					const auto enabled = fixture.Render(snapshot);
					int darkest = 255;
					for (size_t pixel = 0; pixel < enabled.Pixels.size(); pixel += 4)
					{
						darkest = std::min(darkest, std::to_integer<int>(enabled.Pixels[pixel]));
						CHECK(enabled.Pixels[pixel] == enabled.Pixels[pixel + 1]);
						CHECK(enabled.Pixels[pixel] == enabled.Pixels[pixel + 2]);
					}
					CHECK(darkest < 245);
					const auto stats = fixture.Renderer().GetRenderStats();
					REQUIRE(Test::SceneRendererIntegrationFixture::FindPass(stats, "GTAO") != nullptr);
				}
				snapshot.DebugView = RenderDebugView::Lit;
				snapshot.Post.ExposureEV = 0;
				const auto occludedAmbient = fixture.Render(snapshot);
				snapshot.Post.SsaoEnabled = false;
				const auto ambient = fixture.Render(snapshot);
				size_t darker = 0;
				int maximumDifference = 0;
				for (size_t pixel = 0; pixel < ambient.Pixels.size(); pixel += 4)
				{
					maximumDifference = std::max(maximumDifference, std::to_integer<int>(ambient.Pixels[pixel]) - std::to_integer<int>(occludedAmbient.Pixels[pixel]));
					if (std::to_integer<int>(occludedAmbient.Pixels[pixel]) + 1 < std::to_integer<int>(ambient.Pixels[pixel]))
						++darker;
				}
				CAPTURE(static_cast<uint32_t>(projection));
				CAPTURE(maximumDifference);
				CAPTURE(GetRed(ambient, 33, 24));
				CAPTURE(GetRed(occludedAmbient, 33, 24));
				CHECK(darker > 10);
				snapshot.Environment.FallbackColor = glm::vec3(0);
				snapshot.Lights = { LightData{} };
				const auto direct = fixture.Render(snapshot);
				snapshot.Post.SsaoEnabled = true;
				const auto directWithAo = fixture.Render(snapshot);
				CHECK(direct.Pixels == directWithAo.Pixels);
			}
		}

		TEST_CASE("SceneRenderer: capture keeps the snapshot projection at a different target aspect")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			MaterialData flat;
			flat.NormalScale = 0.0f; // Remove the normal-map texel's byte-quantization tilt from the flat-plane AO oracle.
			fixture.Assets().Publish(UUID(8911), CreateRef<MaterialData>(flat));
			auto capture = ViewportCapture::CreateForScenes(gpu.GetDevice(), fixture.Pipelines(), fixture.Cache(), fixture.Assets());
			REQUIRE(capture);
			for (const auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				auto snapshot = fixture.Snapshot(projection);
				snapshot.DebugView = RenderDebugView::Albedo;
				const auto originalProjection = snapshot.Camera.Projection;
				const auto image = (*capture)->Capture({ .Width = 32, .Height = 16 }, snapshot);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				CHECK(image->Width == 32);
				CHECK(image->Height == 16);
				// The square projection covers this pixel. Rebuilding it for a 2:1 target would expose the black background.
				CHECK(Test::SceneRendererIntegrationFixture::Channel(*image, 8, 8) == 255);
				CHECK(Test::SceneRendererIntegrationFixture::Channel(*image, 0, 8) == 0);
				CHECK(snapshot.Camera.Projection == originalProjection);
				CHECK(snapshot.Camera.ViewportWidth == 64);
				CHECK(snapshot.Camera.ViewportHeight == 64);
				CHECK(snapshot.FrameIndex == 17);

				// GTAO targets use the actual capture dimensions even while the projection retains its original aspect.
				snapshot.DebugView = RenderDebugView::AO;
				snapshot.Post.SsaoEnabled = true;
				const auto ao = (*capture)->Capture({ .Width = 31, .Height = 17 }, snapshot);
				REQUIRE_MESSAGE(ao.has_value(), ao.error().ToString());
				CHECK(ao->Width == 31);
				CHECK(ao->Height == 17);
				for (size_t pixel = 0; pixel < ao->Pixels.size(); pixel += 4)
					CHECK(ao->Pixels[pixel] == std::byte{ 255 }); // Isolated flat surface and background are unoccluded.
			}
		}

		TEST_CASE("SceneRenderer: cascade view emits exact primaries and blends without color transforms")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			auto snapshot = fixture.Snapshot();
			snapshot.DebugView = RenderDebugView::ShadowCascades;
			snapshot.Post.ExposureEV = 9;
			snapshot.Post.BloomEnabled = snapshot.Post.FxaaEnabled = true;
			LightData light;
			light.CastShadows = true;
			light.CascadeCount = 2;
			light.CascadeSplitLambda = 0;
			light.ShadowDistance = 10;
			snapshot.Lights = { light };
			for (const auto& [distance, expected] : std::array<std::pair<float, glm::ivec3>, 5>{ { { 3.0f, { 255, 0, 0 } }, { 7.0f, { 0, 255, 0 } }, { 4.8025f, { 128, 128, 0 } }, { 9.7525f, { 0, 128, 0 } }, { 11.0f, { 0, 0, 0 } } } })
			{
				CAPTURE(distance);
				snapshot.Meshes[0].World[3].z = -distance;
				const auto image = fixture.Render(snapshot);
				for (size_t channel = 0; channel < 3; ++channel)
					CHECK(std::abs(Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, channel) - expected[static_cast<glm::length_t>(channel)]) <= 1);
				CHECK(Test::SceneRendererIntegrationFixture::Channel(image, 0, 0) == 0);
			}
			snapshot.Lights[0].CastShadows = false;
			snapshot.Meshes[0].World[3].z = -3;
			CHECK(Test::SceneRendererIntegrationFixture::Channel(fixture.Render(snapshot), 32, 32) == 0);
		}

		TEST_CASE("SceneRenderer: submitted picking owns the exact table despite culled draws and later mutations")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			auto snapshot = fixture.Snapshot();
			snapshot.Flags = RenderViewFlags::Picking;
			auto center = snapshot.Meshes[0];
			center.Entity = UUID(40);
			center.PickId = 2;
			snapshot.Meshes[0].World[3].x = 100;
			snapshot.Meshes.push_back(center);
			fixture.Submit(snapshot);
			snapshot.PickTable = { UUID(11), UUID(12) };
			PickRequest request{ .X = 32, .Y = 32, .FrameIndex = snapshot.FrameIndex, .SceneRevision = snapshot.SceneRevision, .Sequence = 5, .ViewGeneration = fixture.Renderer().GetViewGeneration() };
			auto ticket = fixture.Renderer().RequestPick(request);
			REQUIRE(ticket);
			gpu.GetDevice().WaitForIdle();
			const auto early = fixture.Renderer().PollPick(*ticket, snapshot.FrameIndex + 1);
			REQUIRE(early);
			CHECK_FALSE(early->has_value());
			const auto ready = fixture.Renderer().PollPick(*ticket, snapshot.FrameIndex + 2);
			REQUIRE(ready);
			REQUIRE(ready->has_value());
			CHECK((**ready).Entity == UUID(40));
			CHECK((**ready).PickId == 2);
			CHECK((**ready).Sequence == 5);
			CHECK((**ready).SceneRevision == 41);
			// A non-picking render cannot advertise or accept the previous ID image.
			snapshot.Flags = RenderViewFlags::None;
			++snapshot.FrameIndex;
			fixture.Submit(snapshot);
			CHECK(fixture.Renderer().GetEntityIdTexture() == nullptr);
			const auto unavailable = fixture.Renderer().RequestPick(request);
			REQUIRE_FALSE(unavailable);
			CHECK(unavailable.error().GetCode() == ErrorCode::InvalidState);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("SceneRenderer: scene game and capture views keep independent extents picks and timing frames")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 64, 48);
			auto game = SceneRenderer::Create(gpu.GetDevice(), fixture.Pipelines(), fixture.Cache(), fixture.Assets(), { 23, 11 });
			REQUIRE(game);
			auto scene = fixture.Snapshot();
			scene.Flags = RenderViewFlags::Picking;
			scene.Post.SsaoEnabled = true;
			auto gameSnapshot = scene;
			gameSnapshot.FrameIndex = 900;
			gameSnapshot.SceneRevision = 72;
			gameSnapshot.Meshes[0].Entity = UUID(777);
			gameSnapshot.PickTable = { UUID(777) };
			for (uint32_t frame = 0; frame <= gpu.GetDevice().GetFramesInFlight(); ++frame)
			{
				scene.FrameIndex = 31 + frame;
				gameSnapshot.FrameIndex = 900 + frame;
				fixture.Submit(scene);
				auto list = gpu.GetDevice().CreateCommandList();
				REQUIRE(list);
				(*list)->open();
				const auto rendered = (*game)->Render(**list, gameSnapshot);
				(*list)->close();
				(*game)->OnSubmitted(gameSnapshot.FrameIndex, gpu.GetDevice().ExecuteCommandList(**list));
				REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
				gpu.GetDevice().WaitForIdle();
			}
			const auto sceneStats = fixture.Renderer().GetRenderStats();
			const auto gameStats = (*game)->GetRenderStats();
			CHECK(sceneStats.Width == 64);
			CHECK(sceneStats.Height == 48);
			CHECK(gameStats.Width == 23);
			CHECK(gameStats.Height == 11);
			CHECK(sceneStats.GpuFrameIndex == 31);
			CHECK(gameStats.GpuFrameIndex == 900);
			CHECK(sceneStats.GpuAvailable);
			CHECK(gameStats.GpuAvailable);
			CHECK(sceneStats.MemoryAllocationCount > 0);
			CHECK(gameStats.MemoryAllocationCount > 0);
			CHECK(fixture.Renderer().GetEntityIdTexture() != (*game)->GetEntityIdTexture());
			auto sceneTicket = fixture.Renderer().RequestPick({ .X = 32, .Y = 24, .FrameIndex = scene.FrameIndex, .SceneRevision = scene.SceneRevision, .ViewGeneration = fixture.Renderer().GetViewGeneration() });
			auto gameTicket = (*game)->RequestPick({ .X = 11, .Y = 5, .FrameIndex = gameSnapshot.FrameIndex, .SceneRevision = gameSnapshot.SceneRevision, .ViewGeneration = (*game)->GetViewGeneration() });
			REQUIRE(sceneTicket);
			REQUIRE(gameTicket);
			gpu.GetDevice().WaitForIdle();
			auto scenePick = fixture.Renderer().PollPick(*sceneTicket, scene.FrameIndex + 2);
			auto gamePick = (*game)->PollPick(*gameTicket, gameSnapshot.FrameIndex + 2);
			REQUIRE(scenePick);
			REQUIRE(scenePick->has_value());
			REQUIRE(gamePick);
			REQUIRE(gamePick->has_value());
			CHECK((**scenePick).Entity == UUID(900));
			CHECK((**gamePick).Entity == UUID(777));
			const auto oldGameGeneration = (*game)->GetViewGeneration();
			REQUIRE(fixture.Renderer().Resize(31, 17));
			CHECK((*game)->GetViewGeneration() == oldGameGeneration);
			CHECK((*game)->GetRenderStats().FrameIndex == gameStats.FrameIndex);
			gpu.GetDevice().WaitForIdle();
		}

		TEST_CASE("SceneRenderer: selection and icon composites respect the editor gate and data view precedence")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			auto snapshot = fixture.Snapshot();
			const auto clean = fixture.Render(snapshot);
			snapshot.SelectedEntities = { UUID(900) };
			snapshot.Flags = RenderViewFlags::Selection | RenderViewFlags::Icons;
			snapshot.Icons = { { .Position = { 0, 0, -2 }, .Color = { 1, 0, 0, 1 }, .Size = 20 } };
			CHECK(fixture.Render(snapshot).Pixels == clean.Pixels);
			snapshot.Flags |= RenderViewFlags::EditorOverlays;
			const auto decorated = fixture.Render(snapshot);
			size_t changed = 0;
			for (size_t pixel = 0; pixel < clean.Pixels.size(); pixel += 4)
				if (clean.Pixels[pixel] != decorated.Pixels[pixel] || clean.Pixels[pixel + 1] != decorated.Pixels[pixel + 1])
					++changed;
			CHECK(changed > 100);
			const auto stats = fixture.Renderer().GetRenderStats();
			const auto* composite = Test::SceneRendererIntegrationFixture::FindPass(stats, "SelectionComposite");
			REQUIRE(composite != nullptr);
			CHECK(composite->DrawCalls == 0);
			CHECK(composite->Dispatches == 1);
			REQUIRE(Test::SceneRendererIntegrationFixture::FindPass(stats, "Overlays") != nullptr);
			snapshot.DebugView = RenderDebugView::AO;
			const auto data = fixture.Render(snapshot);
			for (size_t pixel = 0; pixel < data.Pixels.size(); ++pixel)
				CHECK(data.Pixels[pixel] == std::byte{ 255 });
			// Capture-only annotations deliberately survive data-view suppression.
			snapshot.Annotations.Axes = true;
			snapshot.DebugDraw.AddLine({ -1, 0, -1 }, { 1, 0, -1 }, { 1, 0, 0, 1 }, 0, DebugDepthMode::OnTop);
			CHECK(fixture.Render(snapshot).Pixels != data.Pixels);
		}

		TEST_CASE("SceneRenderer: partial error submissions retire targets and preserve valid picking work")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			auto snapshot = fixture.Snapshot();
			snapshot.Flags = RenderViewFlags::Picking;
			auto invalid = snapshot.Meshes[0];
			invalid.World[0][0] = std::numeric_limits<float>::quiet_NaN();
			invalid.Entity = UUID(40);
			snapshot.Meshes.push_back(invalid);
			auto list = gpu.GetDevice().CreateCommandList();
			REQUIRE(list);
			(*list)->open();
			const auto rendered = fixture.Renderer().Render(**list, snapshot);
			(*list)->close();
			fixture.Renderer().OnSubmitted(snapshot.FrameIndex, gpu.GetDevice().ExecuteCommandList(**list));
			REQUIRE_FALSE(rendered);
			CHECK(rendered.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(fixture.Renderer().GetRenderStats().VisibleMeshes == 1);
			auto ticket = fixture.Renderer().RequestPick({ .X = 32, .Y = 32, .FrameIndex = snapshot.FrameIndex, .SceneRevision = snapshot.SceneRevision, .ViewGeneration = fixture.Renderer().GetViewGeneration() });
			REQUIRE(ticket);
			const uint64_t generation = fixture.Renderer().GetViewGeneration();
			// Resize immediately, before waiting for either submission. The old pool targets remain owned until retirement.
			REQUIRE(fixture.Renderer().Resize(17, 9));
			CHECK(fixture.Renderer().GetViewGeneration() > generation);
			const auto cancelled = fixture.Renderer().PollPick(*ticket, snapshot.FrameIndex + 2);
			REQUIRE_FALSE(cancelled);
			CHECK(cancelled.error().GetCode() == ErrorCode::Cancelled);
			snapshot = fixture.Snapshot();
			snapshot.HasCamera = false;
			fixture.Submit(snapshot);
			REQUIRE(fixture.Renderer().Resize(19, 13));
			fixture.Submit(snapshot);
			CHECK(fixture.Renderer().GetRenderStats().Width == 19);
			CHECK(fixture.Renderer().GetRenderStats().Height == 13);
		}
	}

}
