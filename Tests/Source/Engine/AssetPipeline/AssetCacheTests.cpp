#include "TestsPCH.h"

#include "Engine/AssetPipeline/AssetCache.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/RingBufferSink.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		constexpr AssetHandle Wood{ 0xc0c0000000000001ull };
		constexpr AssetHandle Track{ 0x7700000000000001ull };

		CachedImport MakeImport()
		{
			CachedImport import;
			import.Import.Artifacts.push_back({
				.Handle = Wood,
				.Type = AssetType::Texture,
				.SubAssetKey = {},
				.Cooked = CookTexture(GenerateBuiltinTexture(BuiltinTexture::Checker), 1),
			});
			return import;
		}

		// A glTF-shaped import: a main artifact (a mesh stands in for the prefab) and one mesh sub-asset per key.
		CachedImport MakeTrackImport(const std::vector<std::string>& meshKeys)
		{
			CachedImport import;
			import.Import.Artifacts.push_back({ .Handle = Track, .Type = AssetType::Mesh, .SubAssetKey = {}, .Cooked = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Plane), 1) });
			for (const std::string& key : meshKeys)
			{
				import.Import.Artifacts.push_back(
					{ .Handle = DeriveSubAssetHandle(Track, key), .Type = AssetType::Mesh, .SubAssetKey = key, .Cooked = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Cube), 1) });
			}
			return import;
		}

		// The cache file <handle>/<key><extension> of `cache`'s root.
		VfsPath CacheFile(AssetHandle handle, uint64_t key, std::string_view extension)
		{
			return Test::ParseVfsPath(std::format("cache://{}/{:016x}{}", handle, key, extension));
		}

		bool IsMiss(const Result<std::optional<CachedImport>>& found)
		{
			REQUIRE_MESSAGE(found.has_value(), found.error().ToString());
			return !found->has_value();
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("AssetCache: keys change with every input and never with the settings' spelling")
		{
			const std::string source = "source bytes";
			Json settings = Json::object();
			settings["Usage"] = "Color";
			settings["GenerateMips"] = true;
			const uint64_t key = AssetCache::ComputeKey(AsBytes(source), "Texture", 1, settings, EngineCookVersion);
			CHECK(key == AssetCache::ComputeKey(AsBytes(source), "Texture", 1, settings, EngineCookVersion));
			CHECK(key != AssetCache::ComputeKey(AsBytes(std::string("source bytez")), "Texture", 1, settings, EngineCookVersion));
			CHECK(key != AssetCache::ComputeKey(AsBytes(source), "Gltf", 1, settings, EngineCookVersion));
			CHECK(key != AssetCache::ComputeKey(AsBytes(source), "Texture", 2, settings, EngineCookVersion));
			CHECK(key != AssetCache::ComputeKey(AsBytes(source), "Texture", 1, settings, EngineCookVersion + 1));
			Json other = settings;
			other["GenerateMips"] = false;
			CHECK(key != AssetCache::ComputeKey(AsBytes(source), "Texture", 1, other, EngineCookVersion));
			// Framed parts: moving bytes between the source and the importer id changes the key.
			CHECK(AssetCache::ComputeKey(AsBytes(std::string("ab")), "c", 1, settings, 1)
				!= AssetCache::ComputeKey(AsBytes(std::string("a")), "bc", 1, settings, 1));
		}

		TEST_CASE("AssetCache: the key hashes the canonical settings text, not the tree's spelling")
		{
			const std::string source = "source";
			// The same settings parsed from differently spelled text (whitespace, number spelling) give the same key.
			Result<Json> compact = JsonReader::Parse(R"({"Scale":1.0,"Name":"a"})");
			Result<Json> spaced = JsonReader::Parse("{ \"Scale\" : 1 ,\n \"Name\" : \"a\" }");
			REQUIRE(compact.has_value());
			REQUIRE(spaced.has_value());
			CHECK(AssetCache::ComputeKey(AsBytes(source), "Gltf", 1, *compact, 1) == AssetCache::ComputeKey(AsBytes(source), "Gltf", 1, *spaced, 1));
			// Null settings (an importer without settings) differ from an empty object.
			CHECK(AssetCache::ComputeKey(AsBytes(source), "Gltf", 1, Json(nullptr), 1) != AssetCache::ComputeKey(AsBytes(source), "Gltf", 1, Json::object(), 1));
			// The key is a function of its inputs alone: the same on every run, host and configuration.
			CHECK(AssetCache::ComputeKey({}, "", 0, Json(nullptr), 0) == AssetCache::ComputeKey({}, "", 0, Json(nullptr), 0));
		}

		TEST_CASE("AssetCache: an entry round-trips and replaces older keys")
		{
			Test::AssetTestFixture fixture;
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			CHECK(cache.GetRoot().ToString() == "cache://");
			Result<std::optional<CachedImport>> miss = cache.Find(Wood, 1);
			REQUIRE(miss.has_value());
			CHECK_FALSE(miss->has_value());

			const CachedImport import = MakeImport();
			REQUIRE(cache.Store(Wood, 1, import).has_value());
			Result<std::optional<CachedImport>> hit = cache.Find(Wood, 1);
			REQUIRE_MESSAGE(hit.has_value(), hit.error().ToString());
			REQUIRE(hit->has_value());
			CHECK((*hit)->Import.Artifacts.front().Cooked == import.Import.Artifacts.front().Cooked);
			// The layout of §7.5: <root>/<artifact handle>/<key>.bin, the key in 16 hex digits.
			CHECK(fixture.GetVfs().Exists(Test::ParseVfsPath("cache://c0c0000000000001/0000000000000001.bin")));

			REQUIRE(cache.Store(Wood, 2, import).has_value());
			Result<std::optional<CachedImport>> old = cache.Find(Wood, 1);
			REQUIRE(old.has_value());
			CHECK_FALSE(old->has_value());
			REQUIRE(cache.Remove(Wood).has_value());
			Result<std::optional<CachedImport>> removed = cache.Find(Wood, 2);
			REQUIRE(removed.has_value());
			CHECK_FALSE(removed->has_value());
		}

		TEST_CASE("AssetCache: corrupted entry is rebuilt")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(8, 8));
			fixture.OpenProject(false);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			Result<AssetRef<Asset>> first = fixture.GetManager().Load(handle);
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());

			// Flip one byte of every cache file of the asset.
			Result<std::vector<VfsEntry>> files = fixture.GetVfs().List(Test::ParseVfsPath("cache://" + handle.ToString()), true);
			REQUIRE(files.has_value());
			REQUIRE_FALSE(files->empty());
			for (const VfsEntry& entry : *files)
			{
				if (entry.Info.IsDirectory)
					continue;
				Result<Buffer> bytes = fixture.GetVfs().ReadFile(entry.Path);
				REQUIRE(bytes.has_value());
				(*bytes)[bytes->size() / 2] ^= std::byte{ 0x40 };
				REQUIRE(fixture.GetVfs().WriteFileAtomic(entry.Path, *bytes).has_value());
			}

			// A fresh project session finds the entry corrupted, discards it (logged once at Warn) and rebuilds it.
			fixture.GetManager().CloseProject();
			fixture.OpenProject(false);
			Test::ExpectLog discarded(LogLevel::Warn, "corrupted");
			Result<AssetRef<Asset>> rebuilt = fixture.GetManager().Load(handle);
			REQUIRE_MESSAGE(rebuilt.has_value(), rebuilt.error().ToString());
			CHECK(AssetCast<TextureData>(*rebuilt)->Pixels == AssetCast<TextureData>(*first)->Pixels);
			CHECK(discarded.GetMatchCount() >= 1);
			// The rebuilt entry exists again.
			Result<std::vector<VfsEntry>> after = fixture.GetVfs().List(Test::ParseVfsPath("cache://" + handle.ToString()), true);
			REQUIRE(after.has_value());
			CHECK_FALSE(after->empty());
		}

		TEST_CASE("AssetCache: a manifest is current only while its reads and lookups hold")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Models/Track.bin", "abcd");
			const std::vector<ImportDependencyRead> reads = { { .Path = fixture.ProjectPath("Assets/Models/Track.bin"),
				.Hash = XXH64(std::string_view("abcd")) } };
			const std::vector<ImportAssetLookupEntry> assets = {
				{ .SourcePath = fixture.ProjectPath("Assets/Models/Shared.png"), .Handle = AssetHandle(5), .Kind = AssetMetaKind::Asset, .Type = AssetType::Texture, .Owner = AssetHandle() },
			};
			const std::vector<ImportAssetLookup> lookups = { { .Path = fixture.ProjectPath("Assets/Models/Shared.png"), .Found = assets.front() } };
			CHECK(IsManifestCurrent(fixture.GetVfs(), reads, lookups, assets));
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), reads, lookups, {}));
			fixture.WriteProjectText("Assets/Models/Track.bin", "abce");
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), reads, lookups, assets));
		}

		TEST_CASE("AssetCache: a lookup that found nothing stays current until an asset appears there")
		{
			Test::AssetTestFixture fixture;
			const VfsPath image = fixture.ProjectPath("Assets/Models/Track.png");
			const std::vector<ImportAssetLookup> lookups = { { .Path = image, .Found = std::nullopt } };
			CHECK(IsManifestCurrent(fixture.GetVfs(), {}, lookups, {}));
			const std::vector<ImportAssetLookupEntry> texture = {
				{ .SourcePath = image, .Handle = AssetHandle(9), .Kind = AssetMetaKind::Asset, .Type = AssetType::Texture, .Owner = AssetHandle() },
			};
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), {}, lookups, texture));
			// Found, then the asset at the path is another one, or a dependency now.
			const std::vector<ImportAssetLookup> found = { { .Path = image, .Found = texture.front() } };
			std::vector<ImportAssetLookupEntry> other = texture;
			other.front().Handle = AssetHandle(10);
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), {}, found, other));
			std::vector<ImportAssetLookupEntry> dependency = texture;
			dependency.front().Kind = AssetMetaKind::Dependency;
			dependency.front().Type = AssetType::None;
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), {}, found, dependency));
			// A read of a file that is gone is not current.
			const std::vector<ImportDependencyRead> reads = { { .Path = fixture.ProjectPath("Assets/Models/Gone.bin"), .Hash = 0 } };
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), reads, {}, {}));
		}

		TEST_CASE("AssetCache: the manifest keeps dependencies, diagnostics, reads and lookups")
		{
			Test::AssetTestFixture fixture;
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			CachedImport import = MakeTrackImport({ "mesh:0:Straight", "mesh:1:Curve" });
			import.Import.Dependencies = { AssetHandle(3), AssetHandle(4) };
			import.Import.Diagnostics = { {
				.Severity = DiagnosticSeverity::Warning,
				.Code = std::string(AssetVertexColorsIgnoredCode),
				.Asset = Track,
				.Path = "Assets/Models/Track.glb",
				.Message = "COLOR_0 is ignored",
				.Hint = "bake the colours into a texture",
				.Subject = "meshes[0].primitives[0]",
				.AutoFixable = false,
			} };
			import.Reads = { { .Path = fixture.ProjectPath("Assets/Models/Track.bin"), .Hash = 0x0123456789abcdefull } };
			import.Lookups = {
				{ .Path = fixture.ProjectPath("Assets/Models/Missing.png"), .Found = std::nullopt },
				{ .Path = fixture.ProjectPath("Assets/Models/Track.png"),
					.Found = ImportAssetLookupEntry{ .SourcePath = fixture.ProjectPath("Assets/Models/Track.png"), .Handle = AssetHandle(4), .Kind = AssetMetaKind::Asset, .Type = AssetType::Texture, .Owner = AssetHandle() } },
				{ .Path = fixture.ProjectPath("Assets/Other/Data.bin"),
					.Found = ImportAssetLookupEntry{ .SourcePath = fixture.ProjectPath("Assets/Other/Data.bin"), .Handle = AssetHandle(8), .Kind = AssetMetaKind::Dependency, .Type = AssetType::None, .Owner = AssetHandle() } },
			};
			REQUIRE(cache.Store(Track, 7, import).has_value());

			Result<std::optional<CachedImport>> found = cache.Find(Track, 7);
			REQUIRE_MESSAGE(found.has_value(), found.error().ToString());
			REQUIRE(found->has_value());
			const CachedImport& read = **found;
			REQUIRE(read.Import.Artifacts.size() == 3);
			for (size_t index = 0; index < 3; ++index)
			{
				CHECK(read.Import.Artifacts[index].Handle == import.Import.Artifacts[index].Handle);
				CHECK(read.Import.Artifacts[index].Type == import.Import.Artifacts[index].Type);
				CHECK(read.Import.Artifacts[index].SubAssetKey == import.Import.Artifacts[index].SubAssetKey);
				CHECK(read.Import.Artifacts[index].Cooked == import.Import.Artifacts[index].Cooked);
			}
			CHECK(read.Import.Dependencies == import.Import.Dependencies);
			CHECK(read.Import.Diagnostics == import.Import.Diagnostics);
			CHECK(read.Reads == import.Reads);
			// What a manifest records of a lookup: the path, the handle and the type (the owner of a dependency is not kept).
			CHECK(read.Lookups == import.Lookups);

			// The sub-assets live under their own handles, the manifest under the source's.
			CHECK(fixture.GetVfs().Exists(CacheFile(DeriveSubAssetHandle(Track, "mesh:1:Curve"), 7, ".bin")));
			Result<std::string> manifest = fixture.GetVfs().ReadText(CacheFile(Track, 7, ".import"));
			REQUIRE(manifest.has_value());
			CHECK(manifest->starts_with("{\n\t\"Format\": \"ImportManifest\",\n\t\"Version\": 1,\n\t\"Artifacts\": [\n"));
			CHECK(manifest->contains("\"Type\": \"Dependency\""));
		}

		TEST_CASE("AssetCache: a corrupted, incomplete or mistyped entry is discarded as a miss")
		{
			Test::AssetTestFixture fixture;
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			VirtualFileSystem& vfs = fixture.GetVfs();
			const CachedImport track = MakeTrackImport({ "mesh:0:Straight" });
			const AssetHandle straight = DeriveSubAssetHandle(Track, "mesh:0:Straight");

			SUBCASE("a flipped byte in a sub-asset")
			{
				REQUIRE(cache.Store(Track, 1, track).has_value());
				Result<Buffer> bytes = vfs.ReadFile(CacheFile(straight, 1, ".bin"));
				REQUIRE(bytes.has_value());
				bytes->back() ^= std::byte{ 0x01 };
				REQUIRE(vfs.WriteFileAtomic(CacheFile(straight, 1, ".bin"), *bytes).has_value());
				Test::ExpectLog discarded(LogLevel::Warn, "corrupted");
				CHECK(IsMiss(cache.Find(Track, 1)));
				CHECK(discarded.GetMatchCount() == 1);
				// Every file of the entry is gone, so the rebuild starts clean.
				CHECK_FALSE(vfs.Exists(CacheFile(Track, 1, ".import")));
				CHECK_FALSE(vfs.Exists(CacheFile(Track, 1, ".bin")));
				CHECK_FALSE(vfs.Exists(CacheFile(straight, 1, ".bin")));
			}
			SUBCASE("a missing artifact")
			{
				REQUIRE(cache.Store(Track, 1, track).has_value());
				REQUIRE(vfs.Remove(CacheFile(straight, 1, ".bin")).has_value());
				Test::ExpectLog discarded(LogLevel::Warn, "corrupted");
				CHECK(IsMiss(cache.Find(Track, 1)));
				CHECK_FALSE(vfs.Exists(CacheFile(Track, 1, ".import")));
			}
			SUBCASE("an unreadable manifest")
			{
				REQUIRE(cache.Store(Track, 1, track).has_value());
				const std::string garbage = "{ not a manifest";
				REQUIRE(vfs.WriteFileAtomic(CacheFile(Track, 1, ".import"), AsBytes(garbage)).has_value());
				Test::ExpectLog discarded(LogLevel::Warn, "corrupted");
				CHECK(IsMiss(cache.Find(Track, 1)));
				CHECK_FALSE(vfs.Exists(CacheFile(Track, 1, ".bin")));
			}
			SUBCASE("an artifact of another type than the manifest names")
			{
				CachedImport mistyped = MakeImport();
				mistyped.Import.Artifacts.front().Type = AssetType::Mesh; // the bytes hold a texture
				REQUIRE(cache.Store(Wood, 1, mistyped).has_value());
				Test::ExpectLog discarded(LogLevel::Warn, "corrupted");
				CHECK(IsMiss(cache.Find(Wood, 1)));
			}
			SUBCASE("an artifact without its manifest (an interrupted store)")
			{
				REQUIRE(cache.Store(Track, 1, track).has_value());
				REQUIRE(vfs.Remove(CacheFile(Track, 1, ".import")).has_value());
				CHECK(IsMiss(cache.Find(Track, 1)));
			}
		}

		TEST_CASE("AssetCache: ReadArtifact serves one valid artifact and discards a corrupted one")
		{
			Test::AssetTestFixture fixture;
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			const CachedImport track = MakeTrackImport({ "mesh:0:Straight" });
			const AssetHandle straight = DeriveSubAssetHandle(Track, "mesh:0:Straight");
			REQUIRE(cache.Store(Track, 3, track).has_value());

			Result<std::optional<Buffer>> artifact = cache.ReadArtifact(straight, 3);
			REQUIRE_MESSAGE(artifact.has_value(), artifact.error().ToString());
			REQUIRE(artifact->has_value());
			CHECK(**artifact == track.Import.Artifacts[1].Cooked);
			Result<std::optional<Buffer>> otherKey = cache.ReadArtifact(straight, 4);
			REQUIRE(otherKey.has_value());
			CHECK_FALSE(otherKey->has_value());

			Buffer corrupted = track.Import.Artifacts[1].Cooked;
			corrupted[CookedHeader::Size] ^= std::byte{ 0x10 };
			REQUIRE(fixture.GetVfs().WriteFileAtomic(CacheFile(straight, 3, ".bin"), corrupted).has_value());
			Test::ExpectLog discarded(LogLevel::Warn, "corrupted");
			Result<std::optional<Buffer>> rejected = cache.ReadArtifact(straight, 3);
			REQUIRE(rejected.has_value());
			CHECK_FALSE(rejected->has_value());
			CHECK_FALSE(fixture.GetVfs().Exists(CacheFile(straight, 3, ".bin")));
		}

		TEST_CASE("AssetCache: sub-assets a reimport drops are removed, and Remove takes every artifact of a source")
		{
			Test::AssetTestFixture fixture;
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			VirtualFileSystem& vfs = fixture.GetVfs();
			const AssetHandle straight = DeriveSubAssetHandle(Track, "mesh:0:Straight");
			const AssetHandle curve = DeriveSubAssetHandle(Track, "mesh:1:Curve");
			REQUIRE(cache.Store(Track, 1, MakeTrackImport({ "mesh:0:Straight", "mesh:1:Curve" })).has_value());
			REQUIRE(cache.Store(Track, 2, MakeTrackImport({ "mesh:0:Straight" })).has_value());
			CHECK_FALSE(vfs.Exists(CacheFile(curve, 1, ".bin")));
			CHECK_FALSE(vfs.Exists(CacheFile(straight, 1, ".bin")));
			CHECK_FALSE(vfs.Exists(CacheFile(Track, 1, ".import")));
			CHECK(vfs.Exists(CacheFile(straight, 2, ".bin")));

			REQUIRE(cache.Remove(Track).has_value());
			CHECK_FALSE(vfs.Exists(CacheFile(Track, 2, ".import")));
			CHECK_FALSE(vfs.Exists(CacheFile(straight, 2, ".bin")));
			// Removing what is not there changes nothing.
			CHECK(cache.Remove(Track).has_value());
			CHECK(cache.Remove(AssetHandle(0x1234)).has_value());
		}

		TEST_CASE("AssetCache: a cache below a subdirectory keeps its layout there")
		{
			Test::AssetTestFixture fixture;
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://Imports"));
			REQUIRE(cache.Store(Wood, 0xabc, MakeImport()).has_value());
			CHECK(fixture.GetVfs().Exists(Test::ParseVfsPath("cache://Imports/c0c0000000000001/0000000000000abc.bin")));
			CHECK(fixture.GetVfs().Exists(Test::ParseVfsPath("cache://Imports/c0c0000000000001/0000000000000abc.import")));
			Result<std::optional<CachedImport>> found = cache.Find(Wood, 0xabc);
			REQUIRE(found.has_value());
			CHECK(found->has_value());
		}
	}

}
