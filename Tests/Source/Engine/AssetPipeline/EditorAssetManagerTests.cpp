#include "TestsPCH.h"

#include "Engine/AssetPipeline/EditorAssetManager.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/AssetCache.h"
#include "Engine/AssetPipeline/AssetHotReloader.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>

namespace Engine {

	namespace {

		// Copies files of Tests/Data/Assets/Gltf (paths relative to it) into project://<destination><path>, as another
		// program would (not through the manager's writer).
		void CopyGltfFixtures(Test::AssetTestFixture& fixture, std::initializer_list<std::string_view> files,
			std::string_view destination = "Assets/Models/")
		{
			for (const std::string_view file : files)
			{
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/" + std::string(file)));
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				fixture.WriteProjectFile(std::string(destination) + std::string(file), *bytes);
			}
		}

		// The "Type" of the .meta at project://<relative> ("Texture", "Dependency", ...).
		std::string ReadMetaType(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			Result<AssetMetadata> meta = ParseAssetMetadata(AsStringView(fixture.ReadProjectFile(relative)));
			REQUIRE_MESSAGE(meta.has_value(), meta.error().ToString());
			return meta->Kind == AssetMetaKind::Dependency ? std::string(AssetMetadata::DependencyTypeName) : std::string(AssetTypeToString(meta->Type));
		}

		bool HasTextureSubAsset(const AssetMetadata& metadata)
		{
			return std::ranges::any_of(metadata.SubAssets, [](const SubAssetEntry& entry)
			{
				return entry.Type == AssetType::Texture;
			});
		}

		// Polls at `seconds`, then drains the main-thread queue (the start of the next frame).
		void RunFrame(Test::AssetTestFixture& fixture, double seconds)
		{
			fixture.GetManager().Update(seconds);
			static_cast<void>(fixture.GetMainThreadQueue().Drain());
		}

		size_t CountDiagnostics(const AssetManager& manager, std::string_view code, AssetHandle asset)
		{
			return static_cast<size_t>(std::ranges::count_if(manager.GetDiagnostics(), [code, asset](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Code == code && diagnostic.Asset == asset;
			}));
		}

		// ".counted" files are PNGs imported as textures; every Import call is counted.
		class CountingTextureImporter final : public IAssetImporter
		{
		public:
			explicit CountingTextureImporter(uint32_t& importCount)
				: m_ImportCount(&importCount)
			{
			}

			[[nodiscard]] std::string_view GetId() const override { return "CountingTexture"; }
			[[nodiscard]] uint32_t GetVersion() const override { return 1; }
			[[nodiscard]] AssetType GetMainType() const override { return AssetType::Texture; }
			[[nodiscard]] std::span<const std::string_view> GetExtensions() const override
			{
				static constexpr std::string_view Extensions[] = { ".counted" };
				return Extensions;
			}
			[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

			[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override
			{
				++*m_ImportCount; // inline JobSystem: every import runs on the test's thread
				ENGINE_TRY_ASSIGN(Buffer cooked, TextureImporter::ImportTextureFromMemory(context.GetSourceBytes(), { .Usage = TextureUsage::Color, .GenerateMips = false }, "Counted"));
				ImportResult result;
				result.Artifacts.push_back({ .Handle = metadata.Handle, .Type = AssetType::Texture, .SubAssetKey = {}, .Cooked = std::move(cooked) });
				return result;
			}
		private:
			uint32_t* m_ImportCount = nullptr; // documented back-reference: the test's counter outlives the fixture
		};

		// The cooked Prefab document {"Format": "Prefab", "Version": 1, <member>: <value>}: the test importers' artifacts,
		// which the built-in Prefab loader reads.
		Result<Buffer> CookTestDocument(std::string_view member, Json value)
		{
			Json document = Json::object();
			document["Format"] = "Prefab";
			document["Version"] = 1;
			document[std::string(member)] = std::move(value);
			return CookDocument(AssetType::Prefab, document, 1);
		}

		// The import counts of the test importers, by source path; thread-safe, for fixtures with worker threads.
		class ImportCounts
		{
		public:
			void Add(const std::string& source)
			{
				std::scoped_lock lock(m_Mutex);
				++m_Counts[source];
			}

			[[nodiscard]] uint32_t Get(const std::string& source) const
			{
				std::scoped_lock lock(m_Mutex);
				const auto found = m_Counts.find(source);
				return found == m_Counts.end() ? 0 : found->second;
			}

			[[nodiscard]] bool IsEmpty() const
			{
				std::scoped_lock lock(m_Mutex);
				return m_Counts.empty();
			}
		private:
			mutable std::mutex m_Mutex; // guards m_Counts
			std::map<std::string, uint32_t> m_Counts;
		};

		// ".leaf" files: a JSON document {"Value": <number>}, imported as one Prefab artifact holding the value.
		class LeafImporter final : public IAssetImporter
		{
		public:
			explicit LeafImporter(ImportCounts& counts)
				: m_Counts(&counts)
			{
			}

			[[nodiscard]] std::string_view GetId() const override { return "TestLeaf"; }
			[[nodiscard]] uint32_t GetVersion() const override { return 1; }
			[[nodiscard]] AssetType GetMainType() const override { return AssetType::Prefab; }
			[[nodiscard]] std::span<const std::string_view> GetExtensions() const override
			{
				static constexpr std::string_view Extensions[] = { ".leaf" };
				return Extensions;
			}
			[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

			[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override
			{
				m_Counts->Add(context.GetSourcePath().ToString());
				ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(AsStringView(context.GetSourceBytes())));
				ENGINE_TRY_ASSIGN(const int64_t value, JsonReader(document).ReadMember<int64_t>("Value"));
				ENGINE_TRY_ASSIGN(Buffer cooked, CookTestDocument("Value", Json(value)));
				ImportResult result;
				result.Artifacts.push_back({ .Handle = metadata.Handle, .Type = AssetType::Prefab, .SubAssetKey = {}, .Cooked = std::move(cooked) });
				return result;
			}
		private:
			ImportCounts* m_Counts = nullptr; // documented back-reference: the test's counts outlive the fixture
		};

		// ".pack" files: a JSON document {"Files": ["<relative path>", ...]} like a glTF's buffers and images. Each file is
		// read as a dependency and becomes a sub-asset "file:<index>", unless a standalone asset is registered at its path,
		// which the pack references by handle instead (§6.4's standalone-texture reuse).
		class PackImporter final : public IAssetImporter
		{
		public:
			explicit PackImporter(ImportCounts& counts)
				: m_Counts(&counts)
			{
			}

			[[nodiscard]] std::string_view GetId() const override { return "TestPack"; }
			[[nodiscard]] uint32_t GetVersion() const override { return 1; }
			[[nodiscard]] AssetType GetMainType() const override { return AssetType::Prefab; }
			[[nodiscard]] std::span<const std::string_view> GetExtensions() const override
			{
				static constexpr std::string_view Extensions[] = { ".pack" };
				return Extensions;
			}
			[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

			[[nodiscard]] Result<std::vector<VfsPath>> ListDependencyFiles(std::span<const std::byte> source, const VfsPath& sourcePath) const override
			{
				ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(AsStringView(source)));
				ENGINE_TRY_ASSIGN(const JsonReader files, JsonReader(document).GetMember("Files"));
				ENGINE_TRY_ASSIGN(const size_t count, files.GetArraySize());
				std::vector<VfsPath> paths;
				for (size_t index = 0; index < count; ++index)
				{
					ENGINE_TRY_ASSIGN(const JsonReader element, files.GetElement(index));
					ENGINE_TRY_ASSIGN(const std::string relative, element.ReadString());
					ENGINE_TRY_ASSIGN(VfsPath path, sourcePath.GetParent().Join(relative));
					paths.push_back(std::move(path));
				}
				std::ranges::sort(paths);
				paths.erase(std::ranges::unique(paths).begin(), paths.end());
				return paths;
			}

			[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override
			{
				m_Counts->Add(context.GetSourcePath().ToString());
				ENGINE_TRY_ASSIGN(const std::vector<VfsPath> files, ListDependencyFiles(context.GetSourceBytes(), context.GetSourcePath()));
				ImportResult result;
				ENGINE_TRY_ASSIGN(Buffer main, CookTestDocument("Files", Json(files.size())));
				result.Artifacts.push_back({ .Handle = metadata.Handle, .Type = AssetType::Prefab, .SubAssetKey = {}, .Cooked = std::move(main) });
				std::vector<ImportedArtifact> subAssets;
				for (size_t index = 0; index < files.size(); ++index)
				{
					const std::optional<ImportAssetLookupEntry> standalone = context.FindAsset(files[index]);
					if (standalone.has_value() && standalone->Kind == AssetMetaKind::Asset)
					{
						result.Dependencies.push_back(standalone->Handle);
						continue;
					}
					ENGINE_TRY_ASSIGN(const Buffer bytes, context.ReadDependency(files[index]));
					const std::string key = std::format("file:{}", index);
					ENGINE_TRY_ASSIGN(Buffer cooked, CookTestDocument("Bytes", Json(bytes.size())));
					subAssets.push_back({ .Handle = DeriveSubAssetHandle(metadata.Handle, key), .Type = AssetType::Prefab, .SubAssetKey = key, .Cooked = std::move(cooked) });
				}
				std::ranges::sort(subAssets, std::less<>(), &ImportedArtifact::SubAssetKey);
				for (ImportedArtifact& subAsset : subAssets)
					result.Artifacts.push_back(std::move(subAsset));
				std::ranges::sort(result.Dependencies);
				return result;
			}
		private:
			ImportCounts* m_Counts = nullptr; // documented back-reference: the test's counts outlive the fixture
		};

		// Registers both test importers with the fixture (before OpenProject).
		void RegisterTestImporters(Test::AssetTestFixture& fixture, ImportCounts& counts)
		{
			fixture.GetImporters().Register(CreateScope<LeafImporter>(counts));
			fixture.GetImporters().Register(CreateScope<PackImporter>(counts));
		}

		// The "Value" of a loaded leaf, or -1.
		int64_t ReadLeafValue(const AssetRef<Asset>& asset)
		{
			const AssetRef<PrefabData> prefab = AssetCast<PrefabData>(asset);
			if (prefab == nullptr || prefab->Document == nullptr)
				return -1;
			const Result<int64_t> value = JsonReader(*prefab->Document).ReadMember<int64_t>("Value");
			return value.value_or(-1);
		}

		std::string MakeLeafText(int64_t value)
		{
			return std::format("{{\"Value\": {}}}", value);
		}

		// M8: a flat (uncompressed) Radiance RGBE image of `width` x width / 2 texels of radiance `rgb`, which EnvironmentImporter
		// decodes.
		Buffer MakeFlatHdr(uint32_t width, const glm::vec3& rgb)
		{
			const std::string header = std::format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {} +X {}\n", width / 2, width);
			const std::span<const std::byte> headerBytes = AsBytes(header);
			Buffer bytes(headerBytes.begin(), headerBytes.end());
			const float largest = std::max({ rgb.r, rgb.g, rgb.b });
			int exponent = 0;
			const float mantissa = std::frexp(largest, &exponent); // largest = mantissa * 2^exponent, mantissa in [0.5, 1)
			const float scale = mantissa * 256.0f / largest;
			const std::array<std::byte, 4> texel = { static_cast<std::byte>(static_cast<uint8_t>(rgb.r * scale)),
				static_cast<std::byte>(static_cast<uint8_t>(rgb.g * scale)), static_cast<std::byte>(static_cast<uint8_t>(rgb.b * scale)),
				static_cast<std::byte>(static_cast<uint8_t>(exponent + 128)) };
			for (uint32_t index = 0; index < width * (width / 2); ++index)
				bytes.insert(bytes.end(), texel.begin(), texel.end());
			return bytes;
		}

		// M8: a valid environment as a GPU bake would cook it (a 4² skybox with its 3 mips, the 256² x 7 specular cube, every
		// texel binary16 `half`) with distinct SH coefficients.
		EnvironmentData MakeBakedEnvironment(uint16_t half)
		{
			const auto fill = [half](uint32_t faceSize, uint32_t mipCount)
			{
				CubeMapData cube{ .FaceSize = faceSize, .MipCount = mipCount, .Texels = Buffer(ComputeCubeMapByteSize(faceSize, mipCount)) };
				for (size_t offset = 0; offset < cube.Texels.size(); offset += sizeof(half))
					std::memcpy(cube.Texels.data() + offset, &half, sizeof(half));
				return cube;
			};
			EnvironmentData environment;
			environment.Skybox = fill(4, 3);
			environment.Specular = fill(EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount);
			for (size_t index = 0; index < environment.IrradianceSH9.size(); ++index)
				environment.IrradianceSH9[index] = glm::vec3(0.25f * static_cast<float>(index), 0.5f, 1.0f);
			return environment;
		}

		// The cache key (§7.5) the manager gives an environment source of `bytes` with the default settings.
		uint64_t ComputeEnvironmentKey(Test::AssetTestFixture& fixture, std::span<const std::byte> bytes)
		{
			const Result<VariantValue> settings = fixture.GetManager().MergeImportSettings(EnvironmentImporter::Id, VariantValue(), Json::object());
			REQUIRE_MESSAGE(settings.has_value(), settings.error().ToString());
			return AssetCache::ComputeKey(bytes, EnvironmentImporter::Id, EnvironmentImporter::Version, settings->Get(), EngineCookVersion);
		}

		// Stores `environment` cooked as the bake of source `source` under `key` in the cache at `root` ("cache://" or
		// "enginecache://"), as a GPU editor's import or engine bake leaves it.
		void StoreEnvironmentBake(Test::AssetTestFixture& fixture, std::string_view root, AssetHandle source, uint64_t key, const EnvironmentData& environment)
		{
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath(root));
			CachedImport bake;
			bake.Import.Artifacts.push_back(ImportedArtifact{
				.Handle = source,
				.Type = AssetType::Environment,
				.SubAssetKey = {},
				.Cooked = CookEnvironment(environment, EnvironmentImporter::Version),
			});
			const Status stored = cache.Store(source, key, bake);
			REQUIRE_MESSAGE(stored.has_value(), stored.error().ToString());
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("EditorAssetManager: opening a project writes a meta for every new source")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Textures/Wood.png", Test::MakeTestPng(8, 8));
			fixture.WriteProjectText("Assets/Readme.txt", "ignored");
			const AssetRefreshReport report = fixture.OpenProject(false);
			REQUIRE(report.CreatedMetas == std::vector<VfsPath>{ fixture.ProjectPath("Assets/Textures/Wood.png.meta") });
			CHECK(report.Added.size() == 1);
			Result<AssetMetadata> meta = ParseAssetMetadata(AsStringView(fixture.ReadProjectFile("Assets/Textures/Wood.png.meta")));
			REQUIRE_MESSAGE(meta.has_value(), meta.error().ToString());
			CHECK(meta->Importer == "Texture");
			CHECK(meta->Type == AssetType::Texture);
			CHECK(fixture.GetManager().Resolve("Assets/Textures/Wood.png") == meta->Handle);
			CHECK(fixture.GetManager().GetAssetType(meta->Handle) == AssetType::Texture);
			CHECK(fixture.GetManager().GetReferencePath(meta->Handle) == "Assets/Textures/Wood.png");
			// Opening again finds the meta: nothing new is written.
			fixture.GetManager().CloseProject();
			CHECK(fixture.OpenProject(false).CreatedMetas.empty());
		}

		TEST_CASE("EditorAssetManager: a loaded asset comes from the cache the second time")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(8, 8));
			fixture.OpenProject(false);
			const std::optional<AssetHandle> handle = fixture.GetManager().Resolve("Assets/Wood.png");
			REQUIRE(handle.has_value());

			Result<AssetRef<Asset>> first = fixture.GetManager().Load(*handle);
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK(fixture.GetManager().GetState(*handle) == AssetState::Loaded);
			CHECK(fixture.GetManager().GetVersion(*handle) == 1);
			const AssetRef<TextureData> texture = AssetCast<TextureData>(*first);
			REQUIRE(texture != nullptr);
			CHECK(texture->Width == 8);
			CHECK(texture->Mips.size() == 4);

			// A second manager over the same mounts reads the cooked bytes without importing.
			fixture.GetManager().CloseProject();
			fixture.OpenProject(false);
			Result<AssetImportOutcome> reimport = fixture.GetManager().Reimport(*handle);
			REQUIRE(reimport.has_value());
			CHECK_FALSE(reimport->FromCache);
			Result<AssetRef<Asset>> second = fixture.GetManager().Load(*handle);
			REQUIRE(second.has_value());
			CHECK(AssetCast<TextureData>(*second)->Pixels == texture->Pixels);
		}

		TEST_CASE("EditorAssetManager: refresh reimports a changed source and bumps its version")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 1));
			fixture.OpenProject(false);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(handle).has_value());
			CHECK(fixture.GetManager().GetVersion(handle) == 1);

			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 2));
			Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE_MESSAGE(refreshed.has_value(), refreshed.error().ToString());
			CHECK(refreshed->Changed == std::vector<AssetHandle>{ handle });
			CHECK(fixture.GetManager().GetVersion(handle) == 2);
			// Nothing changed since: a second refresh reimports nothing.
			Result<AssetRefreshReport> again = fixture.GetManager().Refresh();
			REQUIRE(again.has_value());
			CHECK(again->Changed.empty());
		}

		TEST_CASE("EditorAssetManager: built-ins resolve by engine path and serve placeholders")
		{
			Test::AssetTestFixture fixture;
			CHECK(fixture.GetManager().Resolve("engine://Meshes/Cube") == BuiltinAssetHandles::CubeMesh);
			CHECK(fixture.GetManager().GetAssetType(BuiltinAssetHandles::ErrorMaterial) == AssetType::Material);
			CHECK(fixture.GetManager().GetReferencePath(BuiltinAssetHandles::CubeMesh) == "engine://Meshes/Cube");
			Result<AssetRef<Asset>> cube = fixture.GetManager().Load(BuiltinAssetHandles::CubeMesh);
			REQUIRE_MESSAGE(cube.has_value(), cube.error().ToString());
			CHECK(AssetCast<MeshData>(*cube) != nullptr);
			CHECK(fixture.GetManager().GetMetadata(BuiltinAssetHandles::CubeMesh) == nullptr);
		}

		TEST_CASE("EditorAssetManager: a dependency handle cannot be loaded")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Data.bin", "data");
			const AssetHandle owner(0x3000000000000001ull);
			const AssetHandle dependency(0x3000000000000002ull);
			fixture.WriteProjectText("Assets/Data.bin.meta", SerializeAssetMetadata(MakeDependencyMetadata(dependency, owner)));
			fixture.OpenProject(false);
			Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(dependency);
			REQUIRE_FALSE(loaded.has_value());
			CHECK(loaded.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(fixture.GetManager().GetAssetType(dependency) == AssetType::None);
		}

		TEST_CASE("EditorAssetManager: a dry run leaves the registry as it was")
		{
			Test::AssetTestFixture fixture;
			fixture.OpenProject(false);
			const size_t before = fixture.GetManager().GetRegistry().GetRecordCount();
			fixture.GetManager().BeginDryRun();
			CHECK(fixture.GetManager().IsDryRun());
			CHECK(fixture.GetManager().GetWriter().IsDryRun());
			MaterialData material;
			material.Roughness = 0.2f;
			Result<std::string> text = MaterialToText(material, fixture.GetRegistry());
			REQUIRE(text.has_value());
			Result<AssetMetadata> meta = fixture.GetManager().CreateMetadata(fixture.ProjectPath("Assets/Red.material"));
			REQUIRE(meta.has_value());
			REQUIRE(fixture.GetManager().GetWriter().Write(fixture.ProjectPath("Assets/Red.material"), AsBytes(*text)).has_value());
			REQUIRE(fixture.GetManager().GetWriter().Write(fixture.ProjectPath("Assets/Red.material.meta"), AsBytes(SerializeAssetMetadata(*meta))).has_value());
			CHECK(fixture.GetManager().GetRegistry().GetRecordCount() == before + 1);
			fixture.GetManager().EndDryRun();
			CHECK_FALSE(fixture.GetManager().IsDryRun());
			CHECK(fixture.GetManager().GetRegistry().GetRecordCount() == before);
		}

		TEST_CASE("EditorAssetManager: import settings merge over the importer's defaults")
		{
			Test::AssetTestFixture fixture;
			Json patch = Json::object();
			patch["Usage"] = "NormalMap";
			Result<VariantValue> merged = fixture.GetManager().MergeImportSettings("Texture", VariantValue(), patch);
			REQUIRE_MESSAGE(merged.has_value(), merged.error().ToString());
			Json settings = merged->Get();
			CHECK(settings["Usage"] == Json("NormalMap"));
			CHECK(settings["GenerateMips"] == Json(true));
			Json invalid = Json::object();
			invalid["Usage"] = "Sometimes";
			Result<VariantValue> rejected = fixture.GetManager().MergeImportSettings("Texture", VariantValue(), invalid);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK_FALSE(fixture.GetManager().MergeImportSettings("Material", VariantValue(), patch).has_value());
		}

		TEST_CASE("EditorAssetManager: a glTF's referenced image becomes a dependency whatever order it arrives in")
		{
			// One batch at open: the glTF and the files it references arrive together.
			{
				Test::AssetTestFixture fixture;
				CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin", "Textures/Checker.png" });
				fixture.OpenProject(false);
				CHECK(ReadMetaType(fixture, "Assets/Models/Textured.bin.meta") == "Dependency");
				CHECK(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Dependency");
				CHECK(ReadMetaType(fixture, "Assets/Models/Textured.gltf.meta") == "Prefab");
			}

			// One poll: "Textured.gltf" sorts before "Textures/Checker.png" in the batch; the closure is resolved before any
			// meta is written, and the glTF's import sees the image's dependency meta (a texture sub-asset, not a reuse).
			{
				Test::AssetTestFixture fixture;
				fixture.OpenProject(true);
				RunFrame(fixture, 0.0);
				CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin", "Textures/Checker.png" });
				for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
					RunFrame(fixture, seconds);
				fixture.GetManager().WaitIdle();
				CHECK(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Dependency");
				const AssetHandle gltf = fixture.GetManager().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
				REQUIRE(fixture.GetManager().Load(gltf).has_value());
				REQUIRE(fixture.GetManager().GetMetadata(gltf) != nullptr);
				CHECK(HasTextureSubAsset(*fixture.GetManager().GetMetadata(gltf)));
			}

			// The image in an earlier batch than the glTF: it keeps its own Texture meta (never converted), and the glTF reuses
			// that standalone texture by handle, also after the project is opened again (its manifest stays current).
			{
				Test::AssetTestFixture fixture;
				CopyGltfFixtures(fixture, { "Textures/Checker.png" });
				fixture.OpenProject(false);
				CHECK(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Texture");
				const AssetHandle checker = fixture.GetManager().Resolve("Assets/Models/Textures/Checker.png").value_or(AssetHandle());
				CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin" });
				REQUIRE(fixture.GetManager().Refresh().has_value());
				CHECK(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Texture");
				const AssetHandle gltf = fixture.GetManager().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
				REQUIRE(fixture.GetManager().Load(gltf).has_value());
				CHECK_FALSE(HasTextureSubAsset(*fixture.GetManager().GetMetadata(gltf)));
				CHECK(fixture.GetManager().GetPathDependents(checker) == std::vector<AssetHandle>{ gltf });
				const std::vector<SubAssetEntry> subAssets = fixture.GetManager().GetMetadata(gltf)->SubAssets;
				fixture.GetManager().CloseProject();
				fixture.OpenProject(false);
				REQUIRE(fixture.GetManager().Load(gltf).has_value());
				CHECK(fixture.GetManager().GetMetadata(gltf)->SubAssets == subAssets);
			}
		}

		TEST_CASE("EditorAssetManager: an import served from the cache is not stale at the next refresh")
		{
			// The glTF's lookup of its image found a dependency meta, whose owner the cache manifest does not record. A cache
			// hit must still read like the import that stored it, or every refresh would take it for changed and reimport it.
			Test::AssetTestFixture fixture;
			CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin", "Textures/Checker.png" });
			fixture.OpenProject(false);
			REQUIRE(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Dependency");
			const AssetHandle gltf = fixture.GetManager().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(gltf).has_value());

			fixture.GetManager().CloseProject();
			fixture.OpenProject(false);
			REQUIRE(fixture.GetManager().Load(gltf).has_value()); // the cache entry the first session stored
			for (int refresh = 0; refresh < 2; ++refresh)
			{
				CAPTURE(refresh);
				Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
				REQUIRE_MESSAGE(refreshed.has_value(), refreshed.error().ToString());
				CHECK(refreshed->Changed.empty());
			}
		}

		TEST_CASE("EditorAssetManager: a file that is already another glTF's dependency fails the second import")
		{
			Test::AssetTestFixture fixture;
			CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin", "Textures/Checker.png" });
			fixture.OpenProject(false);
			const AssetHandle owner = fixture.GetManager().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(owner).has_value());

			// A second glTF (an LOD export, say) that reads the same buffer and image: a dependency has one owner, so moving or
			// trashing the first glTF could otherwise take the files away from the second.
			fixture.WriteProjectFile("Assets/Models/TexturedLod.gltf", fixture.ReadProjectFile("Assets/Models/Textured.gltf"));
			// The refresh imports the new source, so it reports the failure; the load returns the remembered failure.
			Test::ExpectLog failure(LogLevel::Error, "Assets/Models/TexturedLod.gltf");
			const Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE(refreshed.has_value());
			const AssetHandle lod = fixture.GetManager().Resolve("Assets/Models/TexturedLod.gltf").value_or(AssetHandle());
			REQUIRE(lod.IsValid());
			CHECK(std::ranges::any_of(refreshed->Diagnostics, [lod](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == lod && diagnostic.Code == AssetImportFailedCode;
			}));
			Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(lod);
			REQUIRE_FALSE(loaded.has_value());
			CHECK(loaded.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(loaded.error().ToString().find("Assets/Models/Textured.gltf") != std::string::npos);
			CHECK(fixture.GetManager().HasErrorDiagnostics());
			// The first glTF keeps its files.
			Result<AssetMetadata> bufferMeta = ParseAssetMetadata(AsStringView(fixture.ReadProjectFile("Assets/Models/Textured.bin.meta")));
			REQUIRE(bufferMeta.has_value());
			CHECK(bufferMeta->Owner == owner);
		}

		TEST_CASE("EditorAssetManager: a missing reference's diagnostic clears when the asset comes back")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Red.material", R"({"Format": "Material", "Version": 1})");
			fixture.OpenProject(false);
			const AssetHandle red = fixture.GetManager().Resolve("Assets/Red.material").value_or(AssetHandle());
			REQUIRE(red.IsValid());
			const AssetRef<MaterialData> placeholder = AssetCast<MaterialData>(fixture.GetManager().GetPlaceholder(AssetType::Material));

			// asset.delete moves the pair into the trash; a scene still uses the material.
			AssetWriter& writer = fixture.GetManager().GetWriter();
			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Red.material"), fixture.ProjectPath("Library/Trash/1/Assets/Red.material")).has_value());
			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Red.material.meta"), fixture.ProjectPath("Library/Trash/1/Assets/Red.material.meta")).has_value());
			{
				Test::ExpectLog missing(LogLevel::Error, red.ToString());
				CHECK(fixture.GetManager().GetOrPlaceholder<MaterialData>(red) == placeholder);
			}
			CHECK(CountDiagnostics(fixture.GetManager(), AssetMissingCode, red) == 1);
			// A reference diagnostic does not gate play or export by itself (ProjectValidator checks current references).
			CHECK_FALSE(fixture.GetManager().HasErrorDiagnostics());

			// edit.undo moves the pair back: the handle is registered again and the diagnostic goes.
			REQUIRE(writer.Move(fixture.ProjectPath("Library/Trash/1/Assets/Red.material.meta"), fixture.ProjectPath("Assets/Red.material.meta")).has_value());
			REQUIRE(writer.Move(fixture.ProjectPath("Library/Trash/1/Assets/Red.material"), fixture.ProjectPath("Assets/Red.material")).has_value());
			CHECK(CountDiagnostics(fixture.GetManager(), AssetMissingCode, red) == 0);
			CHECK(fixture.GetManager().GetOrPlaceholder<MaterialData>(red) != placeholder);
			CHECK_FALSE(fixture.GetManager().HasErrorDiagnostics());
		}

		TEST_CASE("EditorAssetManager: a failed import is not retried until its source changes")
		{
			uint32_t importCount = 0;
			Test::AssetTestFixture fixture;
			fixture.GetImporters().Register(CreateScope<CountingTextureImporter>(importCount));
			fixture.WriteProjectText("Assets/Broken.counted", "not a png");
			fixture.OpenProject(false);
			const AssetHandle broken = fixture.GetManager().Resolve("Assets/Broken.counted").value_or(AssetHandle());
			REQUIRE(broken.IsValid());
			{
				Test::ExpectLog failure(LogLevel::Error, "Assets/Broken.counted");
				Result<AssetRef<Asset>> first = fixture.GetManager().Load(broken);
				REQUIRE_FALSE(first.has_value());
				Result<AssetRef<Asset>> second = fixture.GetManager().Load(broken);
				REQUIRE_FALSE(second.has_value());
				CHECK(second.error().GetCode() == first.error().GetCode());
				for (int use = 0; use < 3; ++use)
					CHECK(fixture.GetManager().GetOrPlaceholder<TextureData>(broken) != nullptr);
			}
			// Imported once: every later call got the recorded error.
			CHECK(importCount == 1);
			CHECK(fixture.GetManager().GetState(broken) == AssetState::Failed);

			// A changed source is imported again, and the failure's diagnostic goes.
			fixture.WriteProjectFile("Assets/Broken.counted", Test::MakeTestPng(2, 2));
			REQUIRE(fixture.GetManager().Refresh().has_value());
			REQUIRE(fixture.GetManager().Load(broken).has_value());
			CHECK(importCount == 2);
			CHECK(fixture.GetManager().GetState(broken) == AssetState::Loaded);
			CHECK(CountDiagnostics(fixture.GetManager(), AssetImportFailedCode, broken) == 0);
		}

		TEST_CASE("EditorAssetManager: a batch gives the files a new source references dependency metas")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			// "Kit.pack" sorts before "Kit/B.leaf", which it references: the closure is resolved before any meta is written.
			fixture.WriteProjectText("Assets/Kit.pack", R"({"Files": ["Kit/A.bin", "Kit/B.leaf"]})");
			fixture.WriteProjectText("Assets/Kit/A.bin", "aaaa");
			fixture.WriteProjectText("Assets/Kit/B.leaf", MakeLeafText(7));
			fixture.WriteProjectText("Assets/Loose.leaf", MakeLeafText(3));
			const AssetRefreshReport report = fixture.OpenProject(false);
			CHECK(report.CreatedMetas == std::vector<VfsPath>{ fixture.ProjectPath("Assets/Kit.pack.meta"), fixture.ProjectPath("Assets/Kit/A.bin.meta"), fixture.ProjectPath("Assets/Kit/B.leaf.meta"), fixture.ProjectPath("Assets/Loose.leaf.meta") });
			const AssetHandle pack = fixture.GetManager().Resolve("Assets/Kit.pack").value_or(AssetHandle());
			REQUIRE(pack.IsValid());
			CHECK(ReadMetaType(fixture, "Assets/Kit/A.bin.meta") == "Dependency");
			CHECK(ReadMetaType(fixture, "Assets/Kit/B.leaf.meta") == "Dependency");
			CHECK(ReadMetaType(fixture, "Assets/Loose.leaf.meta") == "Prefab");
			CHECK(fixture.GetManager().GetRegistry().GetDependencyRecords(pack).size() == 2);
			// Nothing is imported until loaded.
			CHECK(counts.IsEmpty());

			// The import turns each dependency into a sub-asset, which the .meta now lists.
			Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(pack);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetMetadata* metadata = fixture.GetManager().GetMetadata(pack);
			REQUIRE(metadata != nullptr);
			REQUIRE(metadata->SubAssets.size() == 2);
			CHECK(metadata->SubAssets[0].Key == "file:0");
			const std::vector<SubAssetEntry> subAssets = metadata->SubAssets;
			Result<AssetMetadata> written = ParseAssetMetadata(AsStringView(fixture.ReadProjectFile("Assets/Kit.pack.meta")));
			REQUIRE(written.has_value());
			CHECK(written->SubAssets == subAssets);
			const AssetHandle subAsset = subAssets[1].Handle;
			CHECK(fixture.GetManager().GetState(subAsset) == AssetState::Loaded);
			CHECK(fixture.GetManager().GetReferencePath(subAsset) == "Assets/Kit.pack#file:1");
			CHECK(fixture.GetManager().Resolve("Assets/Kit.pack#file:1") == subAsset);
			CHECK(fixture.GetManager().GetAssetType(subAsset) == AssetType::Prefab);
			CHECK(fixture.GetManager().GetVersion(subAsset) == 1);

			// A reimport keeps every handle and publishes nothing new when nothing changed.
			Result<AssetImportOutcome> reimported = fixture.GetManager().Reimport(pack);
			REQUIRE_MESSAGE(reimported.has_value(), reimported.error().ToString());
			CHECK_FALSE(reimported->FromCache);
			CHECK(reimported->SubAssets == subAssets);
			CHECK(fixture.GetManager().GetVersion(subAsset) == 1);
			CHECK(counts.Get("project://Assets/Kit.pack") == 2);
		}

		TEST_CASE("EditorAssetManager: a standalone asset a pack references keeps its meta and is reused by handle")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/Kit/B.leaf", MakeLeafText(7));
			fixture.OpenProject(false);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/Kit/B.leaf").value_or(AssetHandle());
			REQUIRE(leaf.IsValid());

			// The pack arrives in a later batch: its closure names the leaf, which already has a meta and is never converted.
			fixture.WriteProjectText("Assets/Kit.pack", R"({"Files": ["Kit/B.leaf"]})");
			Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE_MESSAGE(refreshed.has_value(), refreshed.error().ToString());
			CHECK(refreshed->CreatedMetas == std::vector<VfsPath>{ fixture.ProjectPath("Assets/Kit.pack.meta") });
			CHECK(ReadMetaType(fixture, "Assets/Kit/B.leaf.meta") == "Prefab");
			const AssetHandle pack = fixture.GetManager().Resolve("Assets/Kit.pack").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(pack).has_value());
			CHECK(fixture.GetManager().GetMetadata(pack)->SubAssets.empty());
			CHECK(fixture.GetManager().GetPathDependents(leaf) == std::vector<AssetHandle>{ pack });
			CHECK(fixture.GetManager().GetDependencyGraph().GetDependencies(pack) == std::vector<AssetHandle>{ leaf });

			// The leaf changes: it and the pack that references it are reimported.
			REQUIRE(fixture.GetManager().Load(leaf).has_value());
			fixture.WriteProjectText("Assets/Kit/B.leaf", MakeLeafText(8));
			Result<AssetRefreshReport> changed = fixture.GetManager().Refresh();
			REQUIRE_MESSAGE(changed.has_value(), changed.error().ToString());
			std::vector<AssetHandle> expected = { leaf, pack };
			std::ranges::sort(expected);
			CHECK(changed->Changed == expected);
			CHECK(ReadLeafValue(fixture.GetManager().Load(leaf).value_or(nullptr)) == 8);
			CHECK(fixture.GetManager().GetVersion(leaf) == 2);
			// The pack's own artifact did not change, so its version stays.
			CHECK(fixture.GetManager().GetVersion(pack) == 1);
		}

		TEST_CASE("EditorAssetManager: a pack that reads another pack's dependency fails, naming the owner")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/Kit.pack", R"({"Files": ["Shared.bin"]})");
			fixture.WriteProjectText("Assets/Shared.bin", "shared");
			fixture.OpenProject(false);
			const AssetHandle owner = fixture.GetManager().Resolve("Assets/Kit.pack").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(owner).has_value());

			fixture.WriteProjectText("Assets/Copy.pack", R"({"Files": ["Shared.bin"]})");
			AssetHandle copy;
			{
				// The refresh imports the new source and reports the failure once; the load returns the remembered failure.
				Test::ExpectLog failure(LogLevel::Error, "Assets/Copy.pack");
				REQUIRE(fixture.GetManager().Refresh().has_value());
				copy = fixture.GetManager().Resolve("Assets/Copy.pack").value_or(AssetHandle());
				REQUIRE(copy.IsValid());
				Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(copy);
				REQUIRE_FALSE(loaded.has_value());
				CHECK(loaded.error().GetCode() == ErrorCode::ImportFailed);
				CHECK(loaded.error().ToString().find("'Assets/Kit.pack'") != std::string::npos);
				CHECK(loaded.error().GetHint().find("own copy") != std::string::npos);
			}
			CHECK(fixture.GetManager().GetState(copy) == AssetState::Failed);
			CHECK(CountDiagnostics(fixture.GetManager(), AssetImportFailedCode, copy) == 1);
			CHECK(fixture.GetManager().HasErrorDiagnostics());
			Result<AssetMetadata> shared = ParseAssetMetadata(AsStringView(fixture.ReadProjectFile("Assets/Shared.bin.meta")));
			REQUIRE(shared.has_value());
			CHECK(shared->Owner == owner);

			// The failure is remembered: neither a refresh without changes nor another load imports it again.
			REQUIRE(fixture.GetManager().Refresh().has_value());
			CHECK_FALSE(fixture.GetManager().Load(copy).has_value());
			CHECK(counts.Get("project://Assets/Copy.pack") == 1);
			CHECK(fixture.GetManager().GetState(copy) == AssetState::Failed);
		}

		TEST_CASE("EditorAssetManager: a refresh reports each external change to the listener once")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(1));
			fixture.OpenProject(true);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/A.leaf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(leaf).has_value());
			std::vector<AssetExternalChange> heard;
			fixture.GetManager().SetExternalChangeListener([&heard](const AssetExternalChange& change)
			{
				heard.push_back(change);
			});

			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(22));
			Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE(refreshed.has_value());
			CHECK(refreshed->Changed == std::vector<AssetHandle>{ leaf });
			REQUIRE(heard.size() == 1);
			CHECK(heard.front().Path == fixture.ProjectPath("Assets/A.leaf"));
			CHECK(heard.front().Handle == leaf);
			CHECK(heard.front().Type == AssetType::Prefab);
			CHECK(heard.front().Kind == FileChangeKind::Modified);
			CHECK(fixture.GetManager().GetVersion(leaf) == 2);
			CHECK(counts.Get("project://Assets/A.leaf") == 2);

			// The hot reloader never reports it again: one change, one reimport, one notice, one event.
			for (double seconds = 0.0; seconds <= 2.0; seconds += 0.5)
				RunFrame(fixture, seconds);
			fixture.GetManager().WaitIdle();
			CHECK(heard.size() == 1);
			CHECK(counts.Get("project://Assets/A.leaf") == 2);
			CHECK(fixture.GetManager().GetVersion(leaf) == 2);
			const EventReadResult reloaded = fixture.GetEventLog().Read(0, std::array<EngineEventType, 1>{ EngineEventType::AssetReloaded });
			REQUIRE(reloaded.Events.size() == 1);
			CHECK(reloaded.Events.front().Id == leaf);
			CHECK(reloaded.Events.front().Path == "Assets/A.leaf");
		}

		TEST_CASE("EditorAssetManager: a poll reimports a changed source on a job and swaps it at a later frame")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(1));
			fixture.OpenProject(true);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/A.leaf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(leaf).has_value());
			RunFrame(fixture, 0.0);

			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(5));
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
				RunFrame(fixture, seconds);
			fixture.GetManager().WaitIdle();
			CHECK(fixture.GetManager().GetVersion(leaf) == 2);
			CHECK(ReadLeafValue(fixture.GetManager().Load(leaf).value_or(nullptr)) == 5);
			CHECK(counts.Get("project://Assets/A.leaf") == 2);
		}

		TEST_CASE("EditorAssetManager: deferred reloads are held by Refresh and published when the deferral ends")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(1));
			fixture.OpenProject(false);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/A.leaf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(leaf).has_value());
			size_t heard = 0;
			fixture.GetManager().SetExternalChangeListener([&heard](const AssetExternalChange& /*change*/)
			{
				++heard;
			});

			fixture.GetManager().SetReloadsDeferred(true);
			CHECK(fixture.GetManager().AreReloadsDeferred());
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(2));
			fixture.WriteProjectText("Assets/New.leaf", MakeLeafText(9));
			Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE(refreshed.has_value());
			// The new source's meta is written, but nothing is reimported or announced.
			CHECK(refreshed->CreatedMetas == std::vector<VfsPath>{ fixture.ProjectPath("Assets/New.leaf.meta") });
			CHECK(refreshed->Changed.empty());
			CHECK(refreshed->Deferred == std::vector<AssetHandle>{ leaf });
			CHECK(fixture.GetManager().GetVersion(leaf) == 1);
			CHECK(heard == 0);

			fixture.GetManager().SetReloadsDeferred(false);
			fixture.GetManager().WaitIdle();
			CHECK(heard == 2);
			CHECK(fixture.GetManager().GetVersion(leaf) == 2);
			CHECK(ReadLeafValue(fixture.GetManager().Load(leaf).value_or(nullptr)) == 2);
		}

		TEST_CASE("EditorAssetManager: a poll's report of a change Refresh already published is dropped")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(1));
			fixture.OpenProject(true);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/A.leaf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(leaf).has_value());
			size_t heard = 0;
			fixture.GetManager().SetExternalChangeListener([&heard](const AssetExternalChange& /*change*/)
			{
				++heard;
			});
			RunFrame(fixture, 0.0);

			// The poll at 1.0 s reports the change (seen at 0.5 s, older than the debounce), but its main-thread continuation
			// has not run yet when Refresh detects the same change: Refresh marks it known on the watcher too late for it.
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(22));
			RunFrame(fixture, 0.5);
			fixture.GetManager().Update(1.0);
			REQUIRE(fixture.GetMainThreadQueue().GetPendingCount() == 1);
			Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE(refreshed.has_value());
			CHECK(refreshed->Changed == std::vector<AssetHandle>{ leaf });
			CHECK(heard == 1);

			// The late report changes nothing: one change, one notice, one reimport.
			static_cast<void>(fixture.GetMainThreadQueue().Drain());
			fixture.GetManager().WaitIdle();
			CHECK(heard == 1);
			CHECK(counts.Get("project://Assets/A.leaf") == 2);
			CHECK(fixture.GetManager().GetVersion(leaf) == 2);
		}

		TEST_CASE("EditorAssetManager: a change the deferred hot reloader and a deferred Refresh both hold is published once")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(1));
			fixture.OpenProject(true);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/A.leaf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(leaf).has_value());
			size_t heard = 0;
			fixture.GetManager().SetExternalChangeListener([&heard](const AssetExternalChange& /*change*/)
			{
				++heard;
			});
			RunFrame(fixture, 0.0);

			fixture.GetManager().SetReloadsDeferred(true);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(22));
			RunFrame(fixture, 0.5);
			RunFrame(fixture, 1.0);
			REQUIRE(fixture.GetManager().GetHotReloader() != nullptr);
			REQUIRE(fixture.GetManager().GetHotReloader()->GetDeferredChanges().size() == 1);
			Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE(refreshed.has_value());
			CHECK(refreshed->Deferred == std::vector<AssetHandle>{ leaf });
			CHECK(heard == 0);

			fixture.GetManager().SetReloadsDeferred(false);
			fixture.GetManager().WaitIdle();
			CHECK(heard == 1);
			CHECK(counts.Get("project://Assets/A.leaf") == 2);
			CHECK(fixture.GetManager().GetVersion(leaf) == 2);
			CHECK(ReadLeafValue(fixture.GetManager().Load(leaf).value_or(nullptr)) == 22);
		}

		TEST_CASE("EditorAssetManager: an asynchronous load is Loading until its publication runs")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(4));
			fixture.OpenProject(false);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/A.leaf").value_or(AssetHandle());

			JobHandle<AssetRef<Asset>> job = fixture.GetManager().LoadAsync(leaf);
			// The inline job has run; its publication waits for the next frame.
			CHECK(fixture.GetManager().GetState(leaf) == AssetState::Loading);
			Result<AssetRef<Asset>> loaded = job.Take();
			REQUIRE(loaded.has_value());
			CHECK(ReadLeafValue(*loaded) == 4);
			static_cast<void>(fixture.GetMainThreadQueue().Drain());
			CHECK(fixture.GetManager().GetState(leaf) == AssetState::Loaded);
			CHECK(fixture.GetManager().GetVersion(leaf) == 1);
			// Loading a loaded asset completes at once.
			CHECK(fixture.GetManager().LoadAsync(leaf).Take().has_value());
			CHECK(counts.Get("project://Assets/A.leaf") == 1);
		}

		TEST_CASE("EditorAssetManager: an older asynchronous reimport's completion is dropped")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(1));
			fixture.OpenProject(false);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/A.leaf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(leaf).has_value());

			// Two overlapping reimports of one handle (inline jobs: both have imported before either is published).
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(2));
			JobHandle<AssetImportOutcome> older = fixture.GetManager().ReimportAsync(leaf);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(3));
			JobHandle<AssetImportOutcome> newer = fixture.GetManager().ReimportAsync(leaf);
			CHECK(older.Take().has_value());
			CHECK(newer.Take().has_value());
			fixture.GetManager().WaitIdle();
			// Only the newest completion is published: one new version, with the newest content.
			CHECK(ReadLeafValue(fixture.GetManager().Load(leaf).value_or(nullptr)) == 3);
			CHECK(fixture.GetManager().GetVersion(leaf) == 2);
		}

		TEST_CASE("EditorAssetManager: the write observer hears the manager's own metas with their hashes")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			// Each event with the hash of the file as it is when the observer hears of it (after the manager processed it).
			std::vector<std::pair<AssetWriteEvent, uint64_t>> events;
			fixture.GetManager().SetWriteObserver([&events, &fixture](const AssetWriteEvent& event)
			{
				events.emplace_back(event, XXH64(fixture.ReadProjectFile(event.Path.GetPath())));
			});
			fixture.WriteProjectText("Assets/Kit.pack", R"({"Files": ["Data.bin"]})");
			fixture.WriteProjectText("Assets/Data.bin", "data");
			fixture.OpenProject(false);
			const AssetHandle pack = fixture.GetManager().Resolve("Assets/Kit.pack").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(pack).has_value());

			// The batch's two metas, then the pack's .meta rewritten with its sub-asset.
			REQUIRE(events.size() == 3);
			for (const auto& [event, fileHash] : events)
			{
				CAPTURE(event.Path.ToString());
				CHECK(event.Kind == AssetWriteKind::Written);
				CHECK(event.ContentHash == fileHash);
			}
			CHECK(events[0].first.Path == fixture.ProjectPath("Assets/Data.bin.meta"));
			CHECK(events[1].first.Path == fixture.ProjectPath("Assets/Kit.pack.meta"));
			CHECK(events[2].first.Path == fixture.ProjectPath("Assets/Kit.pack.meta"));
			CHECK(events[2].second == XXH64(fixture.ReadProjectFile("Assets/Kit.pack.meta")));
		}

		TEST_CASE("EditorAssetManager: a read-only project registers transient metas and writes nothing")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/Kit.pack", R"({"Files": ["Data.bin"]})");
			fixture.WriteProjectText("Assets/Data.bin", "data");
			Result<AssetRefreshReport> opened = fixture.GetManager().OpenProject({
				.AssetsRoot = fixture.ProjectPath("Assets"),
				.CacheRoot = Test::ParseVfsPath("cache://"),
				.ReadOnly = true,
				.HotReload = true,
			});
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			CHECK(fixture.GetManager().GetHotReloader() == nullptr);
			const AssetHandle expected(Hash64(0, std::string_view("Assets/Kit.pack")));
			CHECK(fixture.GetManager().Resolve("Assets/Kit.pack") == expected);
			CHECK_FALSE(fixture.GetVfs().Exists(fixture.ProjectPath("Assets/Kit.pack.meta")));
			CHECK_FALSE(fixture.GetVfs().Exists(fixture.ProjectPath("Assets/Data.bin.meta")));
			const AssetRecord* data = fixture.GetManager().GetRegistry().FindBySourcePath(fixture.ProjectPath("Assets/Data.bin"));
			REQUIRE(data != nullptr);
			CHECK(data->Metadata.Kind == AssetMetaKind::Dependency);
			CHECK(data->Metadata.Handle == AssetHandle(Hash64(0, std::string_view("Assets/Data.bin"))));
			CHECK(data->Metadata.Owner == expected);
			REQUIRE(fixture.GetManager().Load(expected).has_value());
			CHECK(fixture.GetManager().GetMetadata(expected)->SubAssets.size() == 1);
			CHECK_FALSE(fixture.GetVfs().Exists(fixture.ProjectPath("Assets/Kit.pack.meta")));

			// The transient metas survive a rescan with the same handles.
			REQUIRE(fixture.GetManager().Refresh().has_value());
			CHECK(fixture.GetManager().Resolve("Assets/Kit.pack") == expected);
			CHECK(fixture.GetManager().GetRegistry().GetDependencyRecords(expected).size() == 1);
		}

		TEST_CASE("EditorAssetManager: the last session's locations keep a duplicated handle with its original")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/Levels/Track.leaf", MakeLeafText(1));
			fixture.OpenProject(false);
			const AssetHandle track = fixture.GetManager().Resolve("Assets/Levels/Track.leaf").value_or(AssetHandle());
			REQUIRE(track.IsValid());
			Result<std::string> locations = fixture.GetVfs().ReadText(Test::ParseVfsPath("cache://AssetLocations.json"));
			REQUIRE(locations.has_value());
			CHECK(locations->find(track.ToString()) != std::string::npos);
			CHECK(locations->find("\"Assets/Levels/Track.leaf\"") != std::string::npos);
			fixture.GetManager().CloseProject();

			// While the editor is closed, a file manager copies the pair to a shorter path, which would win by length alone.
			fixture.WriteProjectText("Assets/Track.leaf", MakeLeafText(1));
			fixture.WriteProjectFile("Assets/Track.leaf.meta", fixture.ReadProjectFile("Assets/Levels/Track.leaf.meta"));
			const AssetRefreshReport reopened = fixture.OpenProject(false);
			REQUIRE(reopened.Diagnostics.size() == 1);
			CHECK(reopened.Diagnostics.front().Code == AssetDuplicateHandleCode);
			CHECK(reopened.Diagnostics.front().Path == "Assets/Track.leaf.meta");
			CHECK(fixture.GetManager().Resolve("Assets/Levels/Track.leaf") == track);
			CHECK(fixture.GetManager().HasErrorDiagnostics());
		}

		TEST_CASE("EditorAssetManager: a dry run's imports leave no version, state or cache trace")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/A.leaf", MakeLeafText(1));
			fixture.OpenProject(false);
			const AssetHandle leaf = fixture.GetManager().Resolve("Assets/A.leaf").value_or(AssetHandle());

			fixture.GetManager().BeginDryRun();
			REQUIRE(fixture.GetManager().Load(leaf).has_value());
			CHECK(fixture.GetManager().GetVersion(leaf) == 1);
			fixture.GetManager().EndDryRun();
			CHECK(fixture.GetManager().GetVersion(leaf) == 0);
			CHECK(fixture.GetManager().GetState(leaf) == AssetState::Unloaded);
			// The dry run stored nothing in the cache: the next load imports again.
			REQUIRE(fixture.GetManager().Load(leaf).has_value());
			CHECK(counts.Get("project://Assets/A.leaf") == 2);
		}

		TEST_CASE("EditorAssetManager: imports on worker threads are published on the main thread")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture(1, 2);
			RegisterTestImporters(fixture, counts);
			for (int index = 0; index < 8; ++index)
				fixture.WriteProjectText(std::format("Assets/Leaf{}.leaf", index), MakeLeafText(index));
			fixture.OpenProject(false);

			std::vector<std::pair<AssetHandle, JobHandle<AssetRef<Asset>>>> jobs;
			for (int index = 0; index < 8; ++index)
			{
				const AssetHandle leaf = fixture.GetManager().Resolve(std::format("Assets/Leaf{}.leaf", index)).value_or(AssetHandle());
				REQUIRE(leaf.IsValid());
				jobs.emplace_back(leaf, fixture.GetManager().LoadAsync(leaf));
			}
			fixture.GetManager().WaitIdle();
			for (size_t index = 0; index < jobs.size(); ++index)
			{
				CAPTURE(index);
				Result<AssetRef<Asset>> loaded = jobs[index].second.Take();
				REQUIRE(loaded.has_value());
				CHECK(ReadLeafValue(*loaded) == static_cast<int64_t>(index));
				CHECK(fixture.GetManager().GetState(jobs[index].first) == AssetState::Loaded);
				CHECK(fixture.GetManager().GetVersion(jobs[index].first) == 1);
			}
		}

		TEST_CASE("EditorAssetManager: the second load of a source comes from the cache")
		{
			ImportCounts counts;
			Test::AssetTestFixture fixture;
			RegisterTestImporters(fixture, counts);
			fixture.WriteProjectText("Assets/Kit.pack", R"({"Files": ["Data.bin"]})");
			fixture.WriteProjectText("Assets/Data.bin", "data");
			fixture.OpenProject(false);
			const AssetHandle pack = fixture.GetManager().Resolve("Assets/Kit.pack").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(pack).has_value());
			fixture.GetManager().CloseProject();

			fixture.OpenProject(false);
			REQUIRE(fixture.GetManager().Load(pack).has_value());
			CHECK(counts.Get("project://Assets/Kit.pack") == 1);
			// A changed dependency file makes the entry stale even though the source's key is the same.
			fixture.GetManager().CloseProject();
			fixture.WriteProjectText("Assets/Data.bin", "other");
			fixture.OpenProject(false);
			REQUIRE(fixture.GetManager().Load(pack).has_value());
			CHECK(counts.Get("project://Assets/Kit.pack") == 2);
		}

		// M8 (§7.4, §7.5; Docs/Decisions/0013-m8-decisions.md decision 9): an editor without a GPU serves an environment from
		// any cooked bake with the same cache key.

		TEST_CASE("EditorAssetManager: without a GPU an environment is served from a cooked bake with the same key")
		{
			Test::AssetTestFixture fixture;
			const Buffer orange = MakeFlatHdr(16, glm::vec3(2.0f, 0.5f, 0.125f));
			fixture.WriteProjectFile("Assets/Environments/Orange.hdr", orange);
			fixture.OpenProject(false);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Environments/Orange.hdr").value_or(AssetHandle());
			REQUIRE(handle.IsValid());
			CHECK(fixture.GetManager().GetAssetType(handle) == AssetType::Environment);

			// A GPU editor baked the same bytes for another source (a copy of the file elsewhere in the project).
			const uint64_t key = ComputeEnvironmentKey(fixture, orange);
			const EnvironmentData baked = MakeBakedEnvironment(0x3C00);
			StoreEnvironmentBake(fixture, "cache://", AssetHandle(0x0123456789abcdefull), key, baked);
			Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(handle);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<EnvironmentData> environment = AssetCast<EnvironmentData>(*loaded);
			REQUIRE(environment != nullptr);
			CHECK(environment->IrradianceSH9 == baked.IrradianceSH9);
			CHECK(environment->Specular.Texels == baked.Specular.Texels);

			// It is stored under the source's own handle too, where the exporter and the next session look for it.
			const AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			const Result<std::optional<CachedImport>> own = cache.Find(handle, key);
			REQUIRE(own.has_value());
			REQUIRE(own->has_value());
			CHECK((*own)->Import.Artifacts.front().Handle == handle);
			const Result<std::vector<AssetHandle>> sources = cache.FindSourcesWithKey(key);
			REQUIRE(sources.has_value());
			CHECK(*sources == std::vector<AssetHandle>{ AssetHandle(0x0123456789abcdefull), handle });

			// A reimport bypasses the cache, but a bake cannot be redone here: it is served from the source's own entry.
			const Result<AssetImportOutcome> reimported = fixture.GetManager().Reimport(handle);
			REQUIRE_MESSAGE(reimported.has_value(), reimported.error().ToString());
			CHECK(reimported->FromCache);
			CHECK(CountDiagnostics(fixture.GetManager(), AssetImportFailedCode, handle) == 0);
		}

		TEST_CASE("EditorAssetManager: without a GPU a copy of a built-in HDRI is served from the engine cooked cache")
		{
			Test::AssetTestFixture fixture(Test::AssetTestFixtureOptions{ .EngineResources = true });
			const Result<Buffer> studio = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Environments/Studio.hdr");
			REQUIRE_MESSAGE(studio.has_value(), studio.error().ToString());
			fixture.WriteProjectFile("Assets/Environments/StudioCopy.hdr", *studio);
			fixture.OpenProject(false);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Environments/StudioCopy.hdr").value_or(AssetHandle());
			REQUIRE(handle.IsValid());

			// The engine cooked cache holds the Studio built-in's bake (default settings, so the same key).
			const EnvironmentData baked = MakeBakedEnvironment(0x3800);
			StoreEnvironmentBake(fixture, "enginecache://", BuiltinAssetHandles::StudioEnvironment, ComputeEnvironmentKey(fixture, *studio), baked);
			Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(handle);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<EnvironmentData> environment = AssetCast<EnvironmentData>(*loaded);
			REQUIRE(environment != nullptr);
			CHECK(environment->Skybox.Texels == baked.Skybox.Texels);
		}

		TEST_CASE("EditorAssetManager: without a GPU an environment no cache holds fails with the GPU hint")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Environments/Blue.hdr", MakeFlatHdr(16, glm::vec3(0.125f, 0.25f, 1.0f)));
			fixture.OpenProject(false);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Environments/Blue.hdr").value_or(AssetHandle());
			REQUIRE(handle.IsValid());
			// A bake of other bytes has another key and is not used.
			StoreEnvironmentBake(fixture, "cache://", AssetHandle(0x0123456789abcdefull), ComputeEnvironmentKey(fixture, MakeFlatHdr(16, glm::vec3(1.0f))),
				MakeBakedEnvironment(0x3C00));
			{
				const Test::ExpectLog failure(LogLevel::Error, "Assets/Environments/Blue.hdr");
				const Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(handle);
				REQUIRE_FALSE(loaded.has_value());
				CHECK(loaded.error().GetCode() == ErrorCode::Unsupported);
				CHECK(loaded.error().GetHint() == "start the editor with a GPU once to bake this environment");
			}
			const std::span<const AssetDiagnostic> diagnostics = fixture.GetManager().GetDiagnostics();
			const auto diagnostic = std::ranges::find_if(diagnostics, [handle](const AssetDiagnostic& candidate)
			{
				return candidate.Asset == handle && candidate.Code == AssetImportFailedCode;
			});
			REQUIRE(diagnostic != diagnostics.end());
			CHECK(diagnostic->Hint == "start the editor with a GPU once to bake this environment");
		}
	}

}
