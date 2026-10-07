#include "TestsPCH.h"

#include "Engine/Renderer/GpuResourceCache.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>

namespace Engine {

	namespace {

		// An asset manager over in-memory assets: Publish replaces an asset and bumps its version like a hot-reload swap, so
		// the cache's version handling is tested without importers or a project. Procedural built-ins come from the base.
		class InMemoryAssetManager final : public AssetManager
		{
		public:
			InMemoryAssetManager()
				: m_Jobs(0, m_MainThreadQueue)
			{
			}

			void Publish(AssetHandle handle, AssetRef<Asset> asset)
			{
				m_Assets.insert_or_assign(handle, std::move(asset));
				BumpVersion(handle);
			}

			Result<AssetRef<Asset>> Load(AssetHandle handle) override
			{
				if (IsBuiltinAssetHandle(handle))
					return GetProceduralBuiltin(handle);
				const auto found = m_Assets.find(handle);
				if (found == m_Assets.end())
					return MakeError(ErrorCode::NotFound, "no asset {}", handle);
				return found->second;
			}

			JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override
			{
				return m_Jobs.Submit([result = Load(handle)]()
				{
					return result;
				});
			}

			AssetState GetState(AssetHandle handle) const override
			{
				return m_Assets.contains(handle) ? AssetState::Loaded : AssetState::Unloaded;
			}

			const AssetMetadata* GetMetadata(AssetHandle /*handle*/) const override { return nullptr; }

			AssetType GetAssetType(AssetHandle handle) const override
			{
				const auto found = m_Assets.find(handle);
				return found != m_Assets.end() ? found->second->GetAssetType() : AssetType::None;
			}

			std::optional<AssetHandle> Resolve(std::string_view /*reference*/) const override { return std::nullopt; }

			std::string GetReferencePath(AssetHandle handle) const override
			{
				const std::span<const BuiltinAssetEntry> builtins = GetProceduralBuiltinEntries();
				const auto builtin = std::ranges::find(builtins, handle, &BuiltinAssetEntry::Handle);
				return builtin != builtins.end() ? builtin->Path : std::format("Assets/Test/{}", handle);
			}

			void WaitIdle() override {}
		private:
			MainThreadQueue m_MainThreadQueue;
			JobSystem m_Jobs;
			std::map<AssetHandle, AssetRef<Asset>> m_Assets;
		};

	}

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
			InMemoryAssetManager assets;
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
			InMemoryAssetManager assets;
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
			InMemoryAssetManager assets;
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
	}

}
