#include "TestsPCH.h"

#include "Engine/AssetPipeline/AssetCache.h"

#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/RingBufferSink.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		constexpr AssetHandle Wood{ 0xc0c0000000000001ull };

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

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("AssetCache: keys change with every input and never with the settings' spelling" * doctest::skip(true))
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

		TEST_CASE("AssetCache: an entry round-trips and replaces older keys" * doctest::skip(true))
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

		TEST_CASE("AssetCache: corrupted entry is rebuilt" * doctest::skip(true))
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

		TEST_CASE("AssetCache: a manifest is current only while its reads and lookups hold" * doctest::skip(true))
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
	}

}
