#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The cooked cache (Architecture §7.5): the project's Library/Cache (cache://) and the engine cooked cache bin/EngineCache
// (enginecache://, dev builds) use the same layout and keys.
//
// Key: XXH64(source bytes ‖ importer id ‖ importer version ‖ canonical settings JSON ‖ engine cook version), each part
// framed by its 8-byte little-endian length so no two different inputs concatenate to the same bytes (ComputeKey). Files
// (handles and keys as 16 lowercase hex digits):
//     <root>/<artifact handle>/<key>.bin       one cooked artifact (CookedHeader + payload), for the main asset and each
//                                              sub-asset of one import, all under the source's key
//     <root>/<source handle>/<key>.import      the import's manifest, canonical JSON (JsonWriter Pretty):
//         { "Format": "ImportManifest", "Version": 1,
//           "Artifacts": [ { "Handle", "Type", "Key" } ... ],   main first, then sub-assets by key (ImportResult order)
//           "Dependencies": [ "<handle>" ... ], "Diagnostics": [ { "Severity", "Code", "Path", "Message", "Hint",
//           "Subject" } ... ], "Reads": [ { "Path", "XXH64" } ... ], "Lookups": [ { "Path", "Handle", "Type" } ... ] }
// The source's dependency files (a glTF's .bin and images) are not part of the key, because the key is computed before the
// import knows them; the manifest records what the import read and looked up, and a hit is valid only while every read
// file still hashes the same and every lookup still finds the same asset (the EditorAssetManager checks with
// IsManifestCurrent). An entry whose artifacts fail their header or hash check is discarded and rebuilt, never trusted
// ("AssetCache: corrupted entry is rebuilt"). The EditorAssetManager also keeps its AssetLocations.json at the root of the
// project cache (EditorAssetManager.h); the cache's own calls never touch it.

namespace Engine {

	class VirtualFileSystem;

	// One cached import.
	struct CachedImport
	{
		ImportResult Import{};                     // the artifacts with their bytes, dependencies and diagnostics
		std::vector<ImportDependencyRead> Reads{}; // as recorded by the ImportContext
		std::vector<ImportAssetLookup> Lookups{};
	};

	// The cache over one root. Stateless beyond the VFS: thread-safe, and calls for different sources may run concurrently
	// on import jobs; calls for one source are serialized by its caller (the EditorAssetManager serializes them per handle,
	// even while imports of that handle overlap, and stores only a completion whose ticket is current).
	class AssetCache
	{
	public:
		static constexpr std::string_view ManifestFormatName = "ImportManifest";
		static constexpr uint32_t ManifestVersion = 1;

		// `vfs` is a documented back-reference that outlives the cache; `root` is the cache's directory ("cache://" or
		// "enginecache://"), created on first store.
		AssetCache(VirtualFileSystem& vfs, VfsPath root);
		~AssetCache();

		AssetCache(const AssetCache&) = delete;
		AssetCache& operator=(const AssetCache&) = delete;

		// The key of §7.5 (see the file comment). `canonicalSettings` is written with the canonical writer (Minified) before
		// hashing, so equal settings give equal keys whatever their source text. Pure.
		[[nodiscard]] static uint64_t ComputeKey(std::span<const std::byte> sourceBytes, std::string_view importerId, uint32_t importerVersion,
			const Json& canonicalSettings, uint32_t engineCookVersion);

		// The import of source `source` cached under `key`, every artifact read and validated (ReadCookedArtifact; its type
		// matching the manifest). nullopt on a miss. A present but corrupted or incomplete entry (an unreadable manifest, a
		// missing artifact, a failed header or hash check) is removed, logged once at Warn and reported as a miss, so the
		// caller rebuilds it (§7.5). Errors: Io for a cache that cannot be read at all.
		[[nodiscard]] Result<std::optional<CachedImport>> Find(AssetHandle source, uint64_t key) const;

		// One artifact of a cached import, validated like Find (for loading a single sub-asset). nullopt on a miss or a
		// corrupted file (removed). Errors: Io.
		[[nodiscard]] Result<std::optional<Buffer>> ReadArtifact(AssetHandle artifact, uint64_t key) const;

		// Stores `import` under (`source`, `key`): every artifact and then the manifest, each written atomically, after which
		// every other key of these handles is removed (one entry per handle). Errors: Io; the entry is incomplete then and Find
		// treats it as a miss.
		[[nodiscard]] Status Store(AssetHandle source, uint64_t key, const CachedImport& import);

		// Removes every entry of `handle` (artifacts and manifests). No effect when there is none. Errors: Io.
		[[nodiscard]] Status Remove(AssetHandle handle);

		// M8: the sources whose import manifest is cached under `key` (a <handle>/<key>.import file), sorted by handle; empty
		// for a root that does not exist yet. The EditorAssetManager serves an import it cannot run here (a GPU bake without a
		// device, §7.4) from another source's entry with the same key (Docs/Decisions/0013-m8-decisions.md decision 9), reading
		// each with Find. Errors: Io for a cache that cannot be listed.
		[[nodiscard]] Result<std::vector<AssetHandle>> FindSourcesWithKey(uint64_t key) const;

		[[nodiscard]] const VfsPath& GetRoot() const;
	private:
		// The VFS back-reference and the root (AssetCache.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	// Whether the reads and lookups recorded for a cached import still hold: every read file's current bytes hash to the
	// recorded XXH64 and every lookup finds the same entry in `assets` (sorted by SourcePath). Pure apart from reading the
	// files through `vfs`; a file that cannot be read makes it false.
	[[nodiscard]] bool IsManifestCurrent(const VirtualFileSystem& vfs, std::span<const ImportDependencyRead> reads,
		std::span<const ImportAssetLookup> lookups, std::span<const ImportAssetLookupEntry> assets);

}
