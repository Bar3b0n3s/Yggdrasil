#include "TestsPCH.h"

#include "Engine/Renderer/GpuResourceCache.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/TestOptions.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace Engine {

	// A 2x1 RGBA8 texture whose two texels are `value`.
	static AssetRef<Asset> MakeTexture(uint8_t value)
	{
		TextureData texture;
		texture.Format = TextureFormat::RGBA8Unorm;
		texture.Width = 2;
		texture.Height = 1;
		texture.Mips.push_back({ .Width = 2, .Height = 1, .Offset = 0, .Size = 8 });
		texture.Pixels.assign(8, std::byte{ value });
		return CreateRef<TextureData>(std::move(texture));
	}

	// A black environment with a 4² skybox of three mips, the full-size specular cube and IrradianceSH9[0] = (1, 2, 3).
	static AssetRef<Asset> MakeEnvironment()
	{
		EnvironmentData environment;
		environment.Skybox = { .FaceSize = 4, .MipCount = 3, .Texels = Buffer(ComputeCubeMapByteSize(4, 3), std::byte{ 0 }) };
		environment.Specular = { .FaceSize = EnvironmentData::SpecularFaceSize,
			.MipCount = EnvironmentData::SpecularMipCount,
			.Texels = Buffer(ComputeCubeMapByteSize(EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount), std::byte{ 0 }) };
		environment.IrradianceSH9[0] = glm::vec3(1.0f, 2.0f, 3.0f);
		return CreateRef<EnvironmentData>(std::move(environment));
	}

	// A font with one glyph, 'A', filling a 4x4 atlas that is inside the outline everywhere.
	static AssetRef<Asset> MakeFont()
	{
		FontData font;
		font.Ascent = 0.8f;
		font.Descent = -0.2f;
		font.AtlasWidth = 4;
		font.AtlasHeight = 4;
		font.AtlasPixels.assign(16, std::byte{ 255 });
		font.Glyphs.push_back({ .Codepoint = 'A',
			.Advance = 0.6f,
			.PlaneMin = glm::vec2(0.0f, 0.0f),
			.PlaneMax = glm::vec2(0.5f, 0.7f),
			.AtlasMin = glm::vec2(0.0f),
			.AtlasMax = glm::vec2(1.0f) });
		return CreateRef<FontData>(std::move(font));
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("GpuResourceCache: uploads a mesh and a texture through the M5 upload paths"
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("GpuResourceCache: null and missing handles share the placeholder's mirror"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				// Null: the placeholder itself, without a diagnostic.
				const GpuMesh& none = cache.GetMesh(AssetHandle());
				CHECK(none.VertexBuffer != nullptr);
				CHECK(none.VertexCount == 24);
				CHECK_FALSE(none.IsPlaceholder);
				CHECK(&cache.GetMesh(BuiltinAssetHandles::CubeMesh) == &none);
				CHECK(assets.GetDiagnostics().empty());

				// Missing: an entry of its own that shares the placeholder's GPU objects; the manager reports it once.
				const AssetHandle unknown(0x7e57'0000'0000'0001);
				Test::ExpectLog missing(LogLevel::Error, std::string(AssetMissingCode));
				const GpuTexture& stand = cache.GetTexture(unknown);
				CHECK(stand.IsPlaceholder);
				CHECK(stand.Version == 0);
				REQUIRE(stand.Texture != nullptr);
				CHECK(stand.Texture == cache.GetTexture(BuiltinAssetHandles::MissingTexture).Texture);
				// The placeholder's whole mip chain was uploaded; its base level reads back exactly.
				CHECK(stand.Texture->getDesc().mipLevels == 7);
				Readback readback(gpu.GetDevice());
				Result<Image> image = readback.ReadTexture(*stand.Texture);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				const TextureData expected = GenerateBuiltinTexture(BuiltinTexture::Missing);
				CHECK(std::ranges::equal(image->Pixels, GetMipPixels(expected, 0)));
				CHECK(&cache.GetTexture(unknown) == &stand);
				CHECK(missing.GetMatchCount() == 1);
				// A mesh asked for as a texture is of another type: the texture placeholder again.
				Test::ExpectLog mismatch(LogLevel::Error, std::string(AssetTypeMismatchCode));
				CHECK(cache.GetTexture(BuiltinAssetHandles::SphereMesh).IsPlaceholder);
				CHECK(cache.GetStats().TextureCount == 3);
				CHECK(cache.GetStats().UploadFailures == 0);

				// Once the asset exists, its own version is uploaded and the stand-in becomes stale.
				assets.Publish(unknown, MakeTexture(40));
				const GpuTexture& real = cache.GetTexture(unknown);
				CHECK_FALSE(real.IsPlaceholder);
				CHECK(real.Version == 1);
				CHECK(real.Texture != cache.GetTexture(BuiltinAssetHandles::MissingTexture).Texture);
				cache.CollectStale();
				CHECK(cache.GetStats().TextureCount == 3);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: a failed upload is remembered per version and reported once per asset"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu({ .SynchronizationValidation = true, .InjectFault = GpuFault::OomTexture, .FramesInFlight = 2 });
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			const AssetHandle texture(0x7e57'0000'0000'0004);
			assets.Publish(texture, MakeTexture(30));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Test::ExpectLog failed(LogLevel::Error, std::string(AssetUploadFailedCode));
				// The asset and then its placeholder fail; the asset's diagnostic says that nothing can be drawn.
				const GpuTexture& first = cache.GetTexture(texture);
				CHECK(first.IsPlaceholder);
				CHECK(first.Texture == nullptr);
				CHECK(first.Version == 1);
				CHECK(cache.GetStats().UploadFailures == 2);
				CHECK(failed.GetMatchCount() == 1);
				const auto findDiagnostic = [&assets](AssetHandle handle) -> const AssetDiagnostic*
				{
					const std::span<const AssetDiagnostic> diagnostics = assets.GetDiagnostics();
					const auto found = std::ranges::find_if(diagnostics, [handle](const AssetDiagnostic& diagnostic)
					{
						return diagnostic.Code == AssetUploadFailedCode && diagnostic.Asset == handle;
					});
					return found != diagnostics.end() ? &*found : nullptr;
				};
				const AssetDiagnostic* diagnostic = findDiagnostic(texture);
				REQUIRE(diagnostic != nullptr);
				CHECK(diagnostic->Severity == DiagnosticSeverity::Error);
				CHECK(diagnostic->Message.contains("could not be uploaded either"));
				CHECK(findDiagnostic(BuiltinAssetHandles::MissingTexture) == nullptr);

				// The same version is not uploaded again.
				CHECK(&cache.GetTexture(texture) == &first);
				CHECK(cache.GetStats().UploadFailures == 2);

				// Asked for directly, the placeholder reports its own failure, once.
				const GpuTexture& placeholder = cache.GetTexture(BuiltinAssetHandles::MissingTexture);
				CHECK(placeholder.Texture == nullptr);
				CHECK(placeholder.IsPlaceholder);
				CHECK(failed.GetMatchCount() == 2);
				CHECK(findDiagnostic(BuiltinAssetHandles::MissingTexture) != nullptr);
				CHECK(cache.GetStats().UploadFailures == 2);

				// A new version tries again; the asset manager logs the asset's diagnostic only once.
				assets.Publish(texture, MakeTexture(31));
				CHECK(cache.GetTexture(texture).Version == 2);
				CHECK(cache.GetStats().UploadFailures == 3);
				CHECK(failed.GetMatchCount() == 2);

				// Buffers are not textures: meshes still upload.
				CHECK(cache.GetMesh(BuiltinAssetHandles::SphereMesh).VertexBuffer != nullptr);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: CollectStale releases old versions, then unused mirrors on request"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			const AssetHandle texture(0x7e57'0000'0000'0002);
			const AssetHandle mesh(0x7e57'0000'0000'0003);
			assets.Publish(texture, MakeTexture(10));
			MeshData plane = GenerateBuiltinMesh(BuiltinMesh::Plane);
			plane.Slots.front().DefaultMaterial = BuiltinAssetHandles::ErrorMaterial;
			assets.Publish(mesh, CreateRef<MeshData>(std::move(plane)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const GpuMesh& uploaded = cache.GetMesh(mesh);
				CHECK_FALSE(uploaded.IsPlaceholder);
				CHECK(uploaded.VertexCount == 121);
				CHECK(uploaded.IndexCount == 600);
				REQUIRE(uploaded.Submeshes.size() == 1);
				CHECK(uploaded.Submeshes.front().IndexCount == 600);
				REQUIRE(uploaded.DefaultMaterials.size() == 1);
				CHECK(uploaded.DefaultMaterials.front() == BuiltinAssetHandles::ErrorMaterial);
				CHECK(uploaded.Bounds.GetSize().x == 1.0f);

				const nvrhi::TextureHandle first = cache.GetTexture(texture).Texture;
				REQUIRE(first != nullptr);
				Readback readback(gpu.GetDevice());
				assets.Publish(texture, MakeTexture(20));
				const GpuTexture& second = cache.GetTexture(texture);
				CHECK(second.Version == 2);
				REQUIRE(second.Texture != nullptr);
				CHECK(second.Texture != first);
				Result<Image> image = readback.ReadTexture(*second.Texture);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				CHECK(std::ranges::all_of(image->Pixels, [](std::byte value)
				{
					return value == std::byte{ 20 };
				}));
				CHECK(cache.GetStats().TextureCount == 2);
				cache.CollectStale();
				CHECK(cache.GetStats().TextureCount == 1);
				CHECK(cache.GetStats().MeshCount == 1);

				// Only what was used since the previous CollectStale survives a release of unused mirrors.
				static_cast<void>(cache.GetMesh(mesh));
				cache.CollectStale(true);
				CHECK(cache.GetStats().MeshCount == 1);
				CHECK(cache.GetStats().TextureCount == 0);
				cache.CollectStale(true);
				CHECK(cache.GetStats().MeshCount == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M8 (Docs/Decisions/0013-m8-decisions.md decision 7): materials and environments, the unload and hot-reload
		// acceptance tests. Skeletons of the M8 contract; stream A implements the mirrors and removes the skips.

		TEST_CASE("GpuResourceCache: live counts return to baseline after unloading a scene" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// A scene's GPU state is a mesh, a material with its texture, an environment and a text's font atlas, mirrored by
			// the cache and the TextRenderer and bound by a SceneRenderer. Unloading it (the view's renderer destroyed, then
			// the hosts' collections of SceneRenderer.h, "Stale mirrors") returns every live count to its baseline. The
			// baseline is taken after a first load and unload, because the pass-owned vertex buffers grow once and stay by
			// design; a second load must then come back to exactly that count.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle TextureHandle{ 0x9001 };
			constexpr AssetHandle MaterialHandle{ 0x9002 };
			constexpr AssetHandle EnvironmentHandle{ 0x9003 };
			constexpr AssetHandle FontHandle{ 0x9004 };
			MaterialData material;
			material.BaseColorMap = TypedAssetHandle<AssetType::Texture>(TextureHandle);
			assets.Publish(TextureHandle, MakeTexture(200));
			assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(material)));
			assets.Publish(EnvironmentHandle, MakeEnvironment());
			assets.Publish(FontHandle, MakeFont());

			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.Position = glm::vec3(0.0f, 0.0f, 5.0f);
			snapshot.Camera.View = glm::translate(glm::mat4(1.0f), -snapshot.Camera.Position);
			snapshot.Camera.ViewportWidth = 32;
			snapshot.Camera.ViewportHeight = 32;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 1000.0f, 32, 32);
			snapshot.Meshes.push_back(MeshDrawItem{ .Mesh = BuiltinAssetHandles::SphereMesh, .Materials = { MaterialHandle } });
			snapshot.Environment.Environment = EnvironmentHandle;
			snapshot.Texts.push_back(TextItem{ .Text = "A", .Font = FontHandle });
			{
				GpuResourceCache cache(device, assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				// Loads the scene into a new view, checks it is mirrored, and unloads it; returns the live count while loaded.
				const auto loadAndUnload = [&]() -> uint64_t
				{
					uint64_t loaded = 0;
					{
						Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(device, **pipelines, cache, assets, { .Width = 32, .Height = 32 });
						REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
						Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
						REQUIRE(commandList.has_value());
						(*commandList)->open();
						const Status rendered = (*renderer)->Render(**commandList, snapshot);
						(*commandList)->close();
						REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
						device.ExecuteCommandList(**commandList);
						CHECK(cache.GetStats().MaterialCount == 1);
						CHECK(cache.GetStats().EnvironmentCount == 1);
						device.RunGarbageCollection();
						loaded = device.GetResourceTracker().GetTotalLiveCount();
					}
					// Unload: the view went with its renderer; two collections release everything not used since (§8.14
					// item 2), and the device's garbage collection destroys what NVRHI deferred.
					for (int collection = 0; collection < 2; ++collection)
					{
						cache.CollectStale(true);
						(*pipelines)->CollectStale(assets, true);
					}
					CHECK(cache.GetStats().MeshCount == 0);
					CHECK(cache.GetStats().TextureCount == 0);
					CHECK(cache.GetStats().MaterialCount == 0);
					CHECK(cache.GetStats().EnvironmentCount == 0);
					device.WaitForIdle();
					device.RunGarbageCollection();
					return loaded;
				};

				static_cast<void>(loadAndUnload());
				const uint64_t baseline = device.GetResourceTracker().GetTotalLiveCount();
				CHECK(loadAndUnload() > baseline);
				INFO("live: ", device.GetResourceTracker().DescribeLiveCounts());
				CHECK(device.GetResourceTracker().GetTotalLiveCount() == baseline);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("HotReload: texture change re-uploads" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// An external change of a texture through the editor's asset manager (polled, reimported, swapped, §7.5) gives the
			// cache a new version: the next GetTexture uploads it and a material using it is rebuilt with the new texture.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 1));
			fixture.WriteProjectText("Assets/Wood.material", R"({ "Format": "Material", "Version": 1, "BaseColorMap": "Assets/Wood.png" })");
			fixture.OpenProject(true);
			const AssetHandle texture = fixture.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			const AssetHandle material = fixture.GetManager().Resolve("Assets/Wood.material").value_or(AssetHandle());
			REQUIRE(texture.IsValid());
			REQUIRE(material.IsValid());
			{
				GpuResourceCache cache(device, fixture.GetManager());
				Readback readback(device);
				const nvrhi::TextureHandle before = cache.GetTexture(texture).Texture;
				REQUIRE(before != nullptr);
				const Result<Image> beforeImage = readback.ReadTexture(*before);
				REQUIRE(beforeImage.has_value());
				const GpuMaterial& materialBefore = cache.GetMaterial(material);
				CHECK(materialBefore.BaseColorMap == before);
				const uint64_t generationBefore = materialBefore.Generation;

				fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 2));
				for (const double seconds : { 0.0, 0.5, 1.0, 1.5 })
				{
					fixture.GetManager().Update(seconds);
					static_cast<void>(fixture.GetMainThreadQueue().Drain());
				}
				fixture.GetManager().WaitIdle();
				REQUIRE(fixture.GetManager().GetVersion(texture) == 2);

				const GpuTexture& after = cache.GetTexture(texture);
				CHECK(after.Version == 2);
				REQUIRE(after.Texture != nullptr);
				CHECK(after.Texture != before);
				const Result<Image> afterImage = readback.ReadTexture(*after.Texture);
				REQUIRE(afterImage.has_value());
				CHECK(afterImage->Pixels != beforeImage->Pixels);
				// The material's own version is unchanged, but its texture's is not: the mirror is rebuilt with the new texture.
				const nvrhi::TextureHandle afterTexture = after.Texture;
				const GpuMaterial& materialAfter = cache.GetMaterial(material);
				CHECK(materialAfter.Generation != generationBefore);
				CHECK(materialAfter.BaseColorMap == afterTexture);
				cache.CollectStale();
				CHECK(cache.GetStats().TextureCount == 1);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: a material mirror binds White, FlatNormal and Black to its empty slots" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			MaterialData material;
			material.AlphaMode = AlphaMode::Mask;
			material.AlphaCutoff = 0.25f;
			material.DoubleSided = true;
			assets.Publish(AssetHandle(0x9101), CreateRef<MaterialData>(std::move(material)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const nvrhi::TextureHandle white = cache.GetTexture(BuiltinAssetHandles::WhiteTexture).Texture;
				const nvrhi::TextureHandle flatNormal = cache.GetTexture(BuiltinAssetHandles::FlatNormalTexture).Texture;
				const nvrhi::TextureHandle black = cache.GetTexture(BuiltinAssetHandles::BlackTexture).Texture;
				const GpuMaterial& mirrored = cache.GetMaterial(AssetHandle(0x9101));
				CHECK(mirrored.BaseColorMap == white);
				CHECK(mirrored.MetallicRoughnessMap == white);
				CHECK(mirrored.OcclusionMap == white);
				CHECK(mirrored.NormalMap == flatNormal);
				CHECK(mirrored.EmissiveMap == black);
				CHECK(mirrored.AlphaMode == AlphaMode::Mask);
				CHECK(mirrored.AlphaCutoff == 0.25f);
				CHECK(mirrored.DoubleSided);
				CHECK_FALSE(mirrored.IsPlaceholder);

				// A missing material mirrors the Error material (GetOrPlaceholder reports it once); a null handle the Default
				// material.
				{
					Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
					CHECK(cache.GetMaterial(AssetHandle(0x9199)).IsPlaceholder);
				}
				CHECK_FALSE(cache.GetMaterial(AssetHandle()).IsPlaceholder);

				// A new version rebuilds the mirror under a new generation.
				const uint64_t generation = cache.GetMaterial(AssetHandle(0x9101)).Generation;
				assets.Publish(AssetHandle(0x9101), CreateRef<MaterialData>());
				const uint64_t rebuilt = cache.GetMaterial(AssetHandle(0x9101)).Generation;
				CHECK(rebuilt != generation);
				// A mirror released by CollectStale and built again never gets an earlier generation back (the renderer's
				// binding sets key on it).
				cache.CollectStale(true);
				cache.CollectStale(true);
				CHECK(cache.GetStats().MaterialCount == 0);
				const uint64_t again = cache.GetMaterial(AssetHandle(0x9101)).Generation;
				CHECK(again != generation);
				CHECK(again != rebuilt);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: an environment mirrors both cubes, and a null or unloadable one is null" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			assets.Publish(AssetHandle(0x9201), MakeEnvironment());
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const GpuEnvironment* mirrored = cache.GetEnvironment(AssetHandle(0x9201));
				REQUIRE(mirrored != nullptr);
				REQUIRE(mirrored->Skybox != nullptr);
				REQUIRE(mirrored->Specular != nullptr);
				CHECK(mirrored->Skybox->getDesc().dimension == nvrhi::TextureDimension::TextureCube);
				CHECK(mirrored->Skybox->getDesc().format == nvrhi::Format::RGBA16_FLOAT);
				CHECK(mirrored->SkyboxMipCount == 3);
				CHECK(mirrored->SpecularMipCount == EnvironmentData::SpecularMipCount);
				CHECK(mirrored->IrradianceSH9[0] == glm::vec3(1.0f, 2.0f, 3.0f));
				CHECK(cache.GetStats().EnvironmentCount == 1);
				CHECK(cache.GetEnvironment(AssetHandle()) == nullptr);
				// An unknown handle: the cache reports the load's NotFound itself, once (GpuResourceCache.h).
				{
					Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
					CHECK(cache.GetEnvironment(AssetHandle(0x9299)) == nullptr);
					CHECK(cache.GetEnvironment(AssetHandle(0x9299)) == nullptr);
					CHECK(missing.GetMatchCount() == 1);
				}
				const std::span<const AssetDiagnostic> diagnostics = assets.GetDiagnostics();
				CHECK(std::ranges::count_if(diagnostics, [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetMissingCode && diagnostic.Asset == AssetHandle(0x9299);
				}) == 1);
				// An asset of another type is not an environment.
				{
					Test::ExpectLog mismatch(LogLevel::Error, "ASSET_TYPE_MISMATCH");
					CHECK(cache.GetEnvironment(BuiltinAssetHandles::CubeMesh) == nullptr);
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
