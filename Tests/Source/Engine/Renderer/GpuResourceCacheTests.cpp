#include "TestsPCH.h"

#include "Engine/Renderer/GpuResourceCache.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

#include <algorithm>

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("GpuResourceCache: uploads a mesh and a texture through the M5 upload paths"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				const GpuMesh& cube = cache.GetMesh(BuiltinAssetHandles::CubeMesh);
				CHECK(cube.VertexBuffer != nullptr);
				CHECK(cube.IndexBuffer != nullptr);
				CHECK(cube.VertexCount == 24);
				CHECK(cube.IndexCount == 36);
				CHECK(cube.Submeshes.size() == 1);
				CHECK(cube.Version == 1);
				CHECK_FALSE(cube.IsPlaceholder);

				const GpuTexture& checker = cache.GetTexture(BuiltinAssetHandles::CheckerTexture);
				REQUIRE(checker.Texture != nullptr);
				CHECK_FALSE(checker.IsPlaceholder);
				// The texels read back exactly (mip 0), whichever upload path the device took.
				Readback readback(gpu.GetDevice());
				Result<Image> image = readback.ReadTexture(*checker.Texture);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				const TextureData expected = GenerateBuiltinTexture(BuiltinTexture::Checker);
				CHECK(std::ranges::equal(image->Pixels, GetMipPixels(expected, 0)));

				// The same handle at the same version is served from the cache.
				CHECK(&cache.GetMesh(BuiltinAssetHandles::CubeMesh) == &cube);
				CHECK(cache.GetStats().MeshCount == 1);
				CHECK(cache.GetStats().TextureCount == 1);
				cache.Clear();
				CHECK(cache.GetStats().MeshCount == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: an injected texture OOM yields the placeholder and a diagnostic"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// Roadmap M5 acceptance "=oom-texture yields a placeholder plus diagnostic", deferred to GpuResourceCache (ADR 0009
			// decision 8): every sampled texture creation fails, the mirror is the placeholder's (null when even that fails) and
			// the asset manager records ASSET_UPLOAD_FAILED, logged once; nothing crashes.
			Test::HeadlessGpuFixture gpu({ .SynchronizationValidation = true, .InjectFault = GpuFault::OomTexture, .FramesInFlight = 2 });
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Test::ExpectLog expected(LogLevel::Error, std::string(AssetUploadFailedCode));
				const GpuTexture& texture = cache.GetTexture(BuiltinAssetHandles::CheckerTexture);
				CHECK(texture.IsPlaceholder);
				static_cast<void>(cache.GetTexture(BuiltinAssetHandles::CheckerTexture));
				CHECK(expected.GetMatchCount() == 1);
				CHECK(cache.GetStats().UploadFailures >= 1);
				CHECK(std::ranges::any_of(assets.GetManager().GetDiagnostics(), [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetUploadFailedCode && diagnostic.Asset == BuiltinAssetHandles::CheckerTexture;
				}));
				// Meshes still upload: the fault hits sampled textures only.
				CHECK_FALSE(cache.GetMesh(BuiltinAssetHandles::CubeMesh).IsPlaceholder);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: a new asset version re-uploads and CollectStale releases the old mirror"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			assets.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(8, 8, 1));
			assets.OpenProject(false);
			const AssetHandle wood = assets.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				const nvrhi::TextureHandle first = cache.GetTexture(wood).Texture;
				REQUIRE(first != nullptr);
				CHECK(cache.GetTexture(wood).Version == 1);
				assets.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(8, 8, 2));
				REQUIRE(assets.GetManager().Refresh().has_value());
				const GpuTexture& second = cache.GetTexture(wood);
				CHECK(second.Version == 2);
				CHECK(second.Texture != first);
				cache.CollectStale();
				CHECK(cache.GetStats().TextureCount == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
