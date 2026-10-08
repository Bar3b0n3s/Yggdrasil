#include "TestsPCH.h"

#include "Engine/AssetPipeline/EngineAssetBaker.h"

#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <thread>

namespace Engine {

	namespace {

		// A stand-in for M8's blue-noise generator: a deterministic 4 x 4 linear texture.
		Result<Buffer> GenerateTestNoise()
		{
			return TextureImporter::ImportTextureFromMemory(Test::MakeTestPng(4, 4, 9), { .Usage = TextureUsage::Linear, .GenerateMips = false },
				"TestNoise");
		}

		// The repository's Resources as engine:// (read-only) and a memory enginecache://, on the fixture's services.
		struct BakeEnvironment
		{
			Test::AssetTestFixture Fixture;
			VirtualFileSystem Vfs;

			BakeEnvironment()
			{
				Result<Scope<NativeDirectoryMount>> resources = NativeDirectoryMount::Create(Test::GetRepositoryRoot() / "Resources", MountAccess::ReadOnly);
				REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
				REQUIRE(Vfs.Mount("engine", std::move(*resources)).has_value());
				REQUIRE(Vfs.Mount("enginecache", CreateScope<MemoryMount>()).has_value());
			}

			[[nodiscard]] EngineBakeSpecification GetSpecification()
			{
				return {
					.Vfs = &Vfs,
					.Importers = &Fixture.GetImporters(),
					.Registry = &Fixture.GetRegistry(),
					.Jobs = &Fixture.GetJobSystem(),
					.EnvironmentBaker = nullptr,
					.Generators = {},
				};
			}
		};

		// The threads one importer ran on. Thread-safe: imports may run on workers.
		struct ImportRecord
		{
			std::mutex Mutex;
			std::vector<std::thread::id> Threads;

			[[nodiscard]] size_t GetCount()
			{
				const std::scoped_lock lock(Mutex);
				return Threads.size();
			}
		};

		// A test importer of ".blob" files (or ".mainblob" ones on the main thread). The source text decides the outcome:
		// "fail" fails the import, "gpu" reports what a GPU importer without a baker reports (Unsupported), anything else
		// cooks the built-in White texture with the source's length as importer version, so different sources give different
		// bytes.
		class BlobImporter final : public IAssetImporter
		{
		public:
			BlobImporter(ImportRecord& record, bool mainThread)
				: m_Record(&record), m_MainThread(mainThread)
			{
			}

			[[nodiscard]] std::string_view GetId() const override { return m_MainThread ? "MainThreadBlob" : "Blob"; }
			[[nodiscard]] uint32_t GetVersion() const override { return 1; }
			[[nodiscard]] AssetType GetMainType() const override { return AssetType::Texture; }
			[[nodiscard]] std::span<const std::string_view> GetExtensions() const override
			{
				static constexpr std::string_view Blob[] = { ".blob" };
				static constexpr std::string_view MainThreadBlob[] = { ".mainblob" };
				return m_MainThread ? std::span<const std::string_view>(MainThreadBlob) : std::span<const std::string_view>(Blob);
			}
			[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }
			[[nodiscard]] bool RequiresMainThread() const override { return m_MainThread; }

			[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override
			{
				{
					const std::scoped_lock lock(m_Record->Mutex);
					m_Record->Threads.push_back(std::this_thread::get_id());
				}
				const std::string_view source = AsStringView(context.GetSourceBytes());
				if (source == "fail")
					return MakeError(ErrorCode::ImportFailed, "the blob says fail");
				if (source == "gpu")
					return MakeError(ErrorCode::Unsupported, "the blob needs a GPU");
				ImportResult result;
				result.Artifacts.push_back({
					.Handle = metadata.Handle,
					.Type = AssetType::Texture,
					.SubAssetKey = {},
					.Cooked = CookTexture(GenerateBuiltinTexture(BuiltinTexture::White), static_cast<uint32_t>(source.size())),
				});
				return result;
			}
		private:
			ImportRecord* m_Record = nullptr; // documented back-reference: the test's record outlives the fixture
			bool m_MainThread = false;
		};

		// Memory engine:// and enginecache:// with the two blob importers registered on a fixture with `workerCount` workers.
		struct BlobEnvironment
		{
			Test::AssetTestFixture Fixture;
			VirtualFileSystem Vfs;
			ImportRecord Jobs;
			ImportRecord MainThread;

			explicit BlobEnvironment(uint32_t workerCount = 0)
				: Fixture(1, workerCount)
			{
				REQUIRE(Vfs.Mount("engine", CreateScope<MemoryMount>()).has_value());
				REQUIRE(Vfs.Mount("enginecache", CreateScope<MemoryMount>()).has_value());
				REQUIRE(Vfs.CreateDirectories(Test::ParseVfsPath("engine://Blobs")).has_value());
				Fixture.GetImporters().Register(CreateScope<BlobImporter>(Jobs, false));
				Fixture.GetImporters().Register(CreateScope<BlobImporter>(MainThread, true));
			}

			void WriteResource(std::string_view relative, std::string_view text)
			{
				REQUIRE(Vfs.WriteFileAtomic(Test::ParseVfsPath(std::format("engine://{}", relative)), AsBytes(text)).has_value());
			}

			[[nodiscard]] EngineBakeSpecification GetSpecification()
			{
				return {
					.Vfs = &Vfs,
					.Importers = &Fixture.GetImporters(),
					.Registry = &Fixture.GetRegistry(),
					.Jobs = &Fixture.GetJobSystem(),
					.EnvironmentBaker = nullptr,
					.Generators = {},
				};
			}
		};

		// One File catalogue entry of the blob importers, in EngineAssets.json's spelling.
		std::string BlobEntry(uint64_t handle, std::string_view name, std::string_view importer = "Blob", std::string_view settings = "{}")
		{
			return std::format(R"({{ "Handle": "{:016x}", "Path": "engine://Blobs/{}", "Type": "Texture", "Source": "File", "File": "Blobs/{}.blob", )"
							   R"("Importer": "{}", "Settings": {}, "Generator": "" }})",
				handle, name, name, importer, settings);
		}

		BuiltinAssetCatalog ParseCatalog(const std::vector<std::string>& entries)
		{
			std::string assets;
			for (const std::string& entry : entries)
				assets += (assets.empty() ? "" : ", ") + entry;
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Parse(std::format(R"({{ "Format": "EngineAssets", "Version": 1, "Assets": [ {} ] }})", assets));
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());
			return std::move(*catalog);
		}

		// Every file under enginecache:// with its bytes, in path order.
		std::vector<std::pair<std::string, Buffer>> ReadEngineCache(const VirtualFileSystem& vfs)
		{
			Result<std::vector<VfsEntry>> entries = vfs.List(Test::ParseVfsPath("enginecache://"), true);
			REQUIRE(entries.has_value());
			std::vector<std::pair<std::string, Buffer>> files;
			for (const VfsEntry& entry : *entries)
			{
				if (entry.Info.IsDirectory)
					continue;
				Result<Buffer> bytes = vfs.ReadFile(entry.Path);
				REQUIRE(bytes.has_value());
				files.emplace_back(entry.Path.ToString(), std::move(*bytes));
			}
			return files;
		}

		bool Contains(const std::vector<AssetHandle>& handles, AssetHandle handle)
		{
			return std::ranges::find(handles, handle) != handles.end();
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("EngineAssetBaker: bakes the Default font into the engine cache once")
		{
			BakeEnvironment environment;
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Load(environment.Vfs);
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());

			Result<EngineBakeReport> first = BakeEngineAssets(environment.GetSpecification(), *catalog);
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK(std::ranges::find(first->Baked, BuiltinAssetHandles::DefaultFont) != first->Baked.end());
			CHECK(first->UpToDate.empty());
			// Procedural built-ins are never cached.
			CHECK(std::ranges::find(first->Baked, BuiltinAssetHandles::CubeMesh) == first->Baked.end());

			Result<EngineBakeReport> second = BakeEngineAssets(environment.GetSpecification(), *catalog);
			REQUIRE(second.has_value());
			CHECK(second->Baked.empty());
			CHECK(std::ranges::find(second->UpToDate, BuiltinAssetHandles::DefaultFont) != second->UpToDate.end());

			const BuiltinAssetEntry* font = catalog->Find(BuiltinAssetHandles::DefaultFont);
			REQUIRE(font != nullptr);
			Result<std::vector<Buffer>> artifacts = GetOrBakeEngineAsset(environment.GetSpecification(), *font);
			REQUIRE(artifacts.has_value());
			REQUIRE(artifacts->size() == 1);
			CHECK(LoadCookedFont(artifacts->front()).has_value());
		}

		TEST_CASE("EngineAssetBaker: a first-use bake that cannot be stored still serves the built-in")
		{
			// bin/EngineCache is shared by every process of the checkout: a store can fail (another process reads the entry
			// being replaced, on Windows), which must not take the built-in away from this one. Here enginecache:// is read-only.
			Test::AssetTestFixture fixture;
			VirtualFileSystem vfs;
			Result<Scope<NativeDirectoryMount>> resources = NativeDirectoryMount::Create(Test::GetRepositoryRoot() / "Resources", MountAccess::ReadOnly);
			REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
			REQUIRE(vfs.Mount("engine", std::move(*resources)).has_value());
			REQUIRE(vfs.Mount("enginecache", CreateScope<MemoryMount>(MountAccess::ReadOnly)).has_value());
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Load(vfs);
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());
			const BuiltinAssetEntry* font = catalog->Find(BuiltinAssetHandles::DefaultFont);
			REQUIRE(font != nullptr);
			const EngineBakeSpecification specification{
				.Vfs = &vfs,
				.Importers = &fixture.GetImporters(),
				.Registry = &fixture.GetRegistry(),
				.Jobs = &fixture.GetJobSystem(),
				.EnvironmentBaker = nullptr,
				.Generators = {},
			};

			const Test::ExpectLog warned(LogLevel::Warn, "could not be stored");
			Result<std::vector<Buffer>> artifacts = GetOrBakeEngineAsset(specification, *font);
			REQUIRE_MESSAGE(artifacts.has_value(), artifacts.error().ToString());
			REQUIRE(artifacts->size() == 1);
			CHECK(LoadCookedFont(artifacts->front()).has_value());
			CHECK(warned.GetMatchCount() == 1);

			// The explicit bake still reports the store's error.
			CHECK_FALSE(BakeEngineAssets(specification, *catalog).has_value());
		}

		TEST_CASE("EngineAssetBaker: entries without an importer in this build are skipped with a warning")
		{
			BakeEnvironment environment;
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Load(environment.Vfs);
			REQUIRE(catalog.has_value());
			Result<EngineBakeReport> report = BakeEngineAssets(environment.GetSpecification(), *catalog);
			REQUIRE(report.has_value());
			// The environments wait for EnvironmentImporter (M8).
			const auto skipped = [&report](AssetHandle handle)
			{
				return std::ranges::any_of(report->Skipped, [handle](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Asset == handle && diagnostic.Severity == DiagnosticSeverity::Warning;
				});
			};
			CHECK(skipped(BuiltinAssetHandles::StudioEnvironment));
			CHECK(skipped(BuiltinAssetHandles::SkyEnvironment));
			const BuiltinAssetEntry* studio = catalog->Find(BuiltinAssetHandles::StudioEnvironment);
			REQUIRE(studio != nullptr);
			Result<std::vector<Buffer>> baked = GetOrBakeEngineAsset(environment.GetSpecification(), *studio);
			REQUIRE_FALSE(baked.has_value());
			CHECK(baked.error().GetCode() == ErrorCode::Unsupported);
		}

		TEST_CASE("EngineAssetBaker: generated entries and entry settings are baked under their own keys")
		{
			BakeEnvironment environment;
			// A catalogue with a Generated entry (the shape of M8's blue noise) and the Default font with non-default settings.
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Parse(R"({
	"Format": "EngineAssets",
	"Version": 1,
	"Assets": [
		{ "Handle": "0000000000000186", "Path": "engine://Textures/TestNoise", "Type": "Texture", "Source": "Generated",
		  "File": "", "Importer": "", "Settings": {}, "Generator": "TestNoise" },
		{ "Handle": "00000000000001c1", "Path": "engine://Fonts/Default", "Type": "Font", "Source": "File",
		  "File": "Fonts/Inter-Regular.ttf", "Importer": "Font", "Settings": {}, "Generator": "" }
	]
}
)");
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());
			const EngineAssetGenerator generators[] = { { .Id = "TestNoise", .Version = 1, .Generate = &GenerateTestNoise } };

			// Without the generator the entry is skipped with a warning; with it, it is baked once.
			Result<EngineBakeReport> withoutGenerator = BakeEngineAssets(environment.GetSpecification(), *catalog);
			REQUIRE(withoutGenerator.has_value());
			CHECK(std::ranges::any_of(withoutGenerator->Skipped, [](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == AssetHandle(0x186) && diagnostic.Severity == DiagnosticSeverity::Warning;
			}));
			EngineBakeSpecification specification = environment.GetSpecification();
			specification.Generators = generators;
			Result<EngineBakeReport> generated = BakeEngineAssets(specification, *catalog);
			REQUIRE(generated.has_value());
			CHECK(std::ranges::find(generated->Baked, AssetHandle(0x186)) != generated->Baked.end());
			Result<std::vector<Buffer>> noise = GetOrBakeEngineAsset(specification, *catalog->Find(AssetHandle(0x186)));
			REQUIRE_MESSAGE(noise.has_value(), noise.error().ToString());
			CHECK(LoadCookedTexture(noise->front()).has_value());
			Result<EngineBakeReport> again = BakeEngineAssets(specification, *catalog);
			REQUIRE(again.has_value());
			CHECK(again->Baked.empty());

			// Settings are part of the key: the same font with other settings is baked again.
			BuiltinAssetEntry font = *catalog->Find(BuiltinAssetHandles::DefaultFont);
			Result<std::vector<Buffer>> defaults = GetOrBakeEngineAsset(specification, font);
			REQUIRE_MESSAGE(defaults.has_value(), defaults.error().ToString());
			Json settings = Json::object();
			settings["PixelSize"] = 32.0;
			font.Settings = VariantValue(std::move(settings));
			Result<std::vector<Buffer>> resized = GetOrBakeEngineAsset(specification, font);
			REQUIRE_MESSAGE(resized.has_value(), resized.error().ToString());
			CHECK(resized->front() != defaults->front());
		}

		TEST_CASE("EngineAssetBaker: a failed import is an Error diagnostic and the other entries still bake")
		{
			BlobEnvironment environment;
			environment.WriteResource("Blobs/Good.blob", "good");
			environment.WriteResource("Blobs/Broken.blob", "fail");
			environment.WriteResource("Blobs/NeedsGpu.blob", "gpu");
			const BuiltinAssetCatalog catalog = ParseCatalog({
				BlobEntry(0x301, "Good"),
				BlobEntry(0x302, "Broken"),
				BlobEntry(0x303, "NeedsGpu"),
				BlobEntry(0x304, "Missing"),
				BlobEntry(0x305, "Unknown", "NoSuchImporter"),
			});

			Result<EngineBakeReport> report = BakeEngineAssets(environment.GetSpecification(), catalog);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->Baked == std::vector<AssetHandle>{ AssetHandle(0x301) });
			CHECK(report->UpToDate.empty());
			// Every skipped entry once, in handle order: a failed import and a missing file are errors, an import that needs a
			// GPU and a missing importer are warnings.
			REQUIRE(report->Skipped.size() == 4);
			CHECK(report->Skipped[0].Asset == AssetHandle(0x302));
			CHECK(report->Skipped[0].Severity == DiagnosticSeverity::Error);
			CHECK(report->Skipped[0].Code == AssetImportFailedCode);
			CHECK(report->Skipped[0].Path == "engine://Blobs/Broken");
			CHECK(report->Skipped[0].Message.contains("the blob says fail"));
			CHECK(report->Skipped[1].Asset == AssetHandle(0x303));
			CHECK(report->Skipped[1].Severity == DiagnosticSeverity::Warning);
			CHECK(report->Skipped[1].Hint.contains("--bake-engine-assets"));
			CHECK(report->Skipped[2].Asset == AssetHandle(0x304));
			CHECK(report->Skipped[2].Severity == DiagnosticSeverity::Error);
			CHECK(report->Skipped[3].Asset == AssetHandle(0x305));
			CHECK(report->Skipped[3].Severity == DiagnosticSeverity::Warning);

			Result<std::vector<Buffer>> broken = GetOrBakeEngineAsset(environment.GetSpecification(), *catalog.Find(AssetHandle(0x302)));
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::ImportFailed);
			Result<std::vector<Buffer>> needsGpu = GetOrBakeEngineAsset(environment.GetSpecification(), *catalog.Find(AssetHandle(0x303)));
			REQUIRE_FALSE(needsGpu.has_value());
			CHECK(needsGpu.error().GetCode() == ErrorCode::Unsupported);
			CHECK(needsGpu.error().GetHint() == "run Editor --headless --bake-engine-assets on a machine with a GPU");
			Result<std::vector<Buffer>> unknown = GetOrBakeEngineAsset(environment.GetSpecification(), *catalog.Find(AssetHandle(0x305)));
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::Unsupported);
			CHECK_FALSE(unknown.error().GetHint().empty());
		}

		TEST_CASE("EngineAssetBaker: a changed resource or a corrupted bake is baked again")
		{
			BlobEnvironment environment;
			environment.WriteResource("Blobs/Good.blob", "good");
			const BuiltinAssetCatalog catalog = ParseCatalog({ BlobEntry(0x301, "Good") });
			const EngineBakeSpecification specification = environment.GetSpecification();

			Result<EngineBakeReport> first = BakeEngineAssets(specification, catalog);
			REQUIRE(first.has_value());
			CHECK(Contains(first->Baked, AssetHandle(0x301)));
			CHECK(environment.Jobs.GetCount() == 1);

			// An up-to-date entry is served without importing.
			Result<std::vector<Buffer>> served = GetOrBakeEngineAsset(specification, *catalog.Find(AssetHandle(0x301)));
			REQUIRE(served.has_value());
			CHECK(environment.Jobs.GetCount() == 1);
			Result<AssetRef<TextureData>> texture = LoadCookedTexture(served->front());
			REQUIRE(texture.has_value());

			// A changed resource has another key.
			environment.WriteResource("Blobs/Good.blob", "better");
			Result<EngineBakeReport> changed = BakeEngineAssets(specification, catalog);
			REQUIRE(changed.has_value());
			CHECK(Contains(changed->Baked, AssetHandle(0x301)));
			CHECK(environment.Jobs.GetCount() == 2);
			// One entry per handle: the older key is gone.
			const std::vector<std::pair<std::string, Buffer>> files = ReadEngineCache(environment.Vfs);
			CHECK(files.size() == 2); // the artifact and the manifest

			// A corrupted bake is discarded (logged at Warn) and baked again.
			for (const auto& [path, bytes] : files)
			{
				if (!path.ends_with(".bin"))
					continue;
				Buffer corrupted = bytes;
				corrupted.back() ^= std::byte{ 0x08 };
				REQUIRE(environment.Vfs.WriteFileAtomic(Test::ParseVfsPath(path), corrupted).has_value());
			}
			Test::ExpectLog discarded(LogLevel::Warn, "corrupted");
			Result<EngineBakeReport> rebaked = BakeEngineAssets(specification, catalog);
			REQUIRE(rebaked.has_value());
			CHECK(Contains(rebaked->Baked, AssetHandle(0x301)));
			CHECK(environment.Jobs.GetCount() == 3);
			CHECK(ReadEngineCache(environment.Vfs) == files);
		}

		TEST_CASE("EngineAssetBaker: identical inputs leave byte-identical caches, whatever the worker count")
		{
			const auto bake = [](uint32_t workerCount)
			{
				BlobEnvironment environment(workerCount);
				environment.WriteResource("Blobs/A.blob", "alpha");
				environment.WriteResource("Blobs/B.blob", "beta!");
				environment.WriteResource("Blobs/C.blob", "c");
				environment.WriteResource("Blobs/D.blob", "on the main thread");
				const BuiltinAssetCatalog catalog = ParseCatalog({
					BlobEntry(0x301, "A"),
					BlobEntry(0x302, "B"),
					BlobEntry(0x303, "C"),
					std::format(R"({{ "Handle": "0000000000000304", "Path": "engine://Blobs/D", "Type": "Texture", "Source": "File", )"
								R"("File": "Blobs/D.blob", "Importer": "MainThreadBlob", "Settings": {{}}, "Generator": "" }})"),
				});
				Result<EngineBakeReport> report = BakeEngineAssets(environment.GetSpecification(), catalog);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
				CHECK(report->Baked.size() == 4);
				// A main-thread importer runs on the calling thread even with workers.
				const std::scoped_lock lock(environment.MainThread.Mutex);
				REQUIRE(environment.MainThread.Threads.size() == 1);
				CHECK(environment.MainThread.Threads.front() == std::this_thread::get_id());
				return ReadEngineCache(environment.Vfs);
			};
			const std::vector<std::pair<std::string, Buffer>> inlineFiles = bake(0);
			CHECK(inlineFiles.size() == 8);
			CHECK(bake(3) == inlineFiles);
		}

		TEST_CASE("EngineAssetBaker: settings an importer rejects are an Error, and procedural entries are never baked")
		{
			BlobEnvironment environment;
			environment.WriteResource("Blobs/Good.blob", "good");
			const BuiltinAssetCatalog catalog = ParseCatalog({ BlobEntry(0x301, "Good", "Blob", R"({ "Quality": 2 })") });
			Result<EngineBakeReport> report = BakeEngineAssets(environment.GetSpecification(), catalog);
			REQUIRE(report.has_value());
			REQUIRE(report->Skipped.size() == 1);
			CHECK(report->Skipped.front().Severity == DiagnosticSeverity::Error);
			CHECK(report->Baked.empty());

			const BuiltinAssetEntry cube = GetProceduralBuiltinEntries().front();
			Result<std::vector<Buffer>> procedural = GetOrBakeEngineAsset(environment.GetSpecification(), cube);
			REQUIRE_FALSE(procedural.has_value());
			CHECK(procedural.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("EngineAssetBaker: a specification without the engine mounts is InvalidArgument")
		{
			Test::AssetTestFixture fixture;
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("engine", CreateScope<MemoryMount>()).has_value());
			const EngineBakeSpecification specification{
				.Vfs = &vfs,
				.Importers = &fixture.GetImporters(),
				.Registry = &fixture.GetRegistry(),
				.Jobs = &fixture.GetJobSystem(),
				.EnvironmentBaker = nullptr,
				.Generators = {},
			};
			Result<EngineBakeReport> report = BakeEngineAssets(specification, BuiltinAssetCatalog());
			REQUIRE_FALSE(report.has_value());
			CHECK(report.error().GetCode() == ErrorCode::InvalidArgument);
			Result<std::vector<Buffer>> artifact = GetOrBakeEngineAsset(specification, GetProceduralBuiltinEntries().front());
			REQUIRE_FALSE(artifact.has_value());
			CHECK(artifact.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// M8 (Docs/Decisions/0013-m8-decisions.md decision 9): the built-in environments bake with a GPU baker. Skeleton of the
		// M8 contract; stream B removes the skip (and updates "entries without an importer in this build are skipped with a
		// warning" once EnvironmentImporter is registered: the environments then skip because no baker is given).

		TEST_CASE("EngineAssetBaker: the built-in environments bake with a GPU and are reused without one"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				BakeEnvironment environment;
				Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Load(environment.Vfs);
				REQUIRE(catalog.has_value());
				EngineBakeSpecification withGpu = environment.GetSpecification();
				withGpu.EnvironmentBaker = baker->get();
				Result<EngineBakeReport> baked = BakeEngineAssets(withGpu, *catalog);
				REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());
				CHECK(std::ranges::find(baked->Baked, BuiltinAssetHandles::StudioEnvironment) != baked->Baked.end());
				CHECK(std::ranges::find(baked->Baked, BuiltinAssetHandles::SkyEnvironment) != baked->Baked.end());

				// Without a baker (--renderer none) the cooked bakes are current and served (§7.5).
				Result<EngineBakeReport> reused = BakeEngineAssets(environment.GetSpecification(), *catalog);
				REQUIRE(reused.has_value());
				CHECK(std::ranges::find(reused->UpToDate, BuiltinAssetHandles::StudioEnvironment) != reused->UpToDate.end());
				const BuiltinAssetEntry* sky = catalog->Find(BuiltinAssetHandles::SkyEnvironment);
				REQUIRE(sky != nullptr);
				// The sun-heavy Sky turns ClampLuminance on (§8.6 step 1).
				CHECK(sky->Settings.Get().contains("ClampLuminance"));
				Result<std::vector<Buffer>> artifacts = GetOrBakeEngineAsset(environment.GetSpecification(), *sky);
				REQUIRE_MESSAGE(artifacts.has_value(), artifacts.error().ToString());
				CHECK(LoadCookedEnvironment(artifacts->front()).has_value());
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
