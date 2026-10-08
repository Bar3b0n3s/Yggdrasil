#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/RenderReference.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

		// Post-processing that maps linear values straight to the OETF: Linear, exposure 1, no bloom, FXAA or dither effects
		// on the tested pixels.
		PostProcessSettings MakeLinearPost()
		{
			return PostProcessSettings{ .ExposureEV = 0.0f, .Tonemap = RenderTonemapper::Linear, .BloomEnabled = false, .FxaaEnabled = false };
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

		// M8 (Roadmap M8; Docs/Decisions/0013-m8-decisions.md decisions 7 and 12). Skeletons of the M8 contract: stream A
		// implements the PBR pass list and removes the skips; the furnace test also needs stream E's references.

		TEST_CASE("Pipelines: count matches the expected total" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// §8.5: "The total pipeline count is logged and asserted by a test." 15 mesh pipelines ({Opaque, Mask} prepass and
			// forward opaque, Blend transparent, each for CullBack, CullFront and CullNone) + Skybox 1 + Bloom 3 + Tonemap 1 +
			// FXAA 1 + debug lines 2 + text 2 + the DFG LUT 1 = 26 at startup; each non-Lit debug view adds its 9 forward
			// variants the first time it renders, 45 for the five.
			static_assert(SceneRendererPipelines::StartupPipelineCount == 26);
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				CHECK((*pipelines)->GetPipelineCount() == 26);
				CHECK(SceneRendererPipelines::GetLayoutDescriptions().size() == 26);
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
				CHECK((*pipelines)->GetPipelineCount() == 26 + 5 * SceneRendererPipelines::DebugViewPipelineCount);
				// EnsureDebugView, which Render called, has no effect for Lit or for a view that exists.
				const Status lit = (*pipelines)->EnsureDebugView(RenderDebugView::Lit);
				const Status existing = (*pipelines)->EnsureDebugView(RenderDebugView::Albedo);
				CHECK(lit.has_value());
				CHECK(existing.has_value());
				CHECK((*pipelines)->GetPipelineCount() == 26 + 5 * SceneRendererPipelines::DebugViewPipelineCount);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: white furnace: white surfaces under a constant environment reflect it unchanged"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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

		TEST_CASE("SceneRenderer: Mask discards below the cutoff and Blend composites over the opaque scene"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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

		TEST_CASE("SceneRenderer: each debug view outputs its quantity" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
	}

}
