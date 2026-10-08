#include "EnginePCH.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/AssetReference.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/AssetPipeline/AssetCache.h"
#include "Engine/AssetPipeline/AssetHotReloader.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <map>
#include <set>

namespace Engine {

	namespace {

		// What the manager last saw of one file under the Assets folder (Refresh's change detection). The hash is known only
		// once the file was read (an import, an editor write, a confirmed change).
		struct KnownFile
		{
			uint64_t Size = 0;
			uint64_t ModificationTime = 0;
			std::optional<uint64_t> Hash{};
		};

		// One published artifact: the loaded asset and the XXH64 of the cooked bytes it was decoded from.
		struct LoadedArtifact
		{
			AssetRef<Asset> Value{};
			uint64_t ContentHash = 0;
		};

		// The last import attempt of one source, the inputs it was based on and what it produced.
		struct ImportRecord
		{
			VfsPath SourcePath{};
			uint64_t Key = 0;
			uint64_t SourceHash = 0;
			std::string Importer{};
			VariantValue Settings{}; // the .meta's settings the import used
			std::vector<ImportDependencyRead> Reads{};
			std::vector<ImportAssetLookup> Lookups{};
			std::vector<AssetHandle> Artifacts{}; // of the last successful import, main first
			std::vector<AssetHandle> LookedUp{};  // standalone assets the last successful import found by path, sorted
			// A remembered failure (§7.2): Load returns it without importing again until an input changes.
			std::optional<Error> Failure{};
		};

		// Everything one import needs, built on the main thread and read by the import (on a job or inline).
		struct ImportRequest
		{
			const VirtualFileSystem* Vfs = nullptr;
			const TypeRegistry* Registry = nullptr;
			const IAssetImporter* Importer = nullptr;
			const AssetLoaderRegistry* Loaders = nullptr;
			IEnvironmentBaker* EnvironmentBaker = nullptr;
			IScriptDiagnosticsProvider* ScriptDiagnostics = nullptr;
			Ref<AssetCache> Cache{}; // null: no cache (a dry run never stores, but may read)
			// M8: the importer needs a GPU bake this editor cannot run (an environment without an IEnvironmentBaker, §7.4), so
			// the import is served from any cooked bake with the same cache key before the importer is called: the source's
			// own entry, another source's in the project cache, then the engine cooked cache (EngineCache, null when
			// enginecache:// is not mounted). Docs/Decisions/0013-m8-decisions.md decision 9.
			bool ReuseBakes = false;
			Ref<AssetCache> EngineCache{};
			AssetMetadata Metadata{};
			VfsPath SourcePath{};
			VariantValue Settings{}; // complete
			std::vector<ImportAssetLookupEntry> Assets{};
			bool UseCache = true;
		};

		// What one import produced, or why it failed.
		struct ImportOutput
		{
			std::optional<Error> Failure{};
			uint64_t Key = 0;
			uint64_t SourceHash = 0;
			std::optional<FileInfo> SourceInfo{}; // when the source was read
			CachedImport Import{};
			std::vector<AssetRef<Asset>> Decoded{}; // parallel to Import.Import.Artifacts
			bool FromCache = false;
			// Served from another source's cache entry with the same key (ImportRequest::ReuseBakes): stored under this source
			// as well, so the exporter and later sessions find it under the source's own handle.
			bool ReusedBake = false;
			std::optional<std::string> CacheWarning{}; // a cache that could not be read
		};

		// A cooked bake an import may reuse (ImportRequest::ReuseBakes), and whether it is the source's own cache entry.
		struct ReusableBake
		{
			CachedImport Import{};
			bool IsOwnEntry = false;
		};

		// What FindReusableBake found: the bake, if any, and the error of the last cache that could not be read.
		struct ReusableBakeSearch
		{
			std::optional<ReusableBake> Bake{};
			std::optional<std::string> Warning{};
		};

		struct DryRunSnapshot
		{
			AssetRegistry Registry;
			AssetDependencyGraph Graph;
			std::map<AssetHandle, LoadedArtifact> Loaded;
			std::map<AssetHandle, ImportRecord> Imports;
			std::map<VfsPath, KnownFile> KnownFiles;
			std::map<VfsPath, AssetMetadata> TransientMetas;
		};

		constexpr std::string_view AssetLocationsFileName = "AssetLocations.json";
		constexpr std::string_view AssetLocationsFormatName = "AssetLocations";
		constexpr uint32_t AssetLocationsVersion = 1;
		constexpr std::string_view EngineScheme = "engine";
		constexpr std::string_view EngineCacheScheme = "enginecache";

		// The codes an import reports for its source and artifacts: cleared by the next successful import.
		constexpr std::array<std::string_view, 5> ImportDiagnosticCodes = { AssetImportFailedCode, AssetTangentsApproximatedCode,
			AssetUnsupportedUvSetCode, AssetVertexColorsIgnoredCode, AssetContentSkippedCode };

	}

	namespace Utils {

		static bool IsMetaFile(const VfsPath& path)
		{
			const std::string_view name = path.GetFileName();
			return name.size() > 5 && name.ends_with(".meta");
		}

		static std::string RelativeText(const VfsPath& path)
		{
			return std::string(path.GetPath());
		}

		// `path` with its prefix `from` replaced by `to` (a moved file or directory), or nullopt when it is not under `from`.
		static std::optional<VfsPath> RemapPath(const VfsPath& path, const VfsPath& from, const VfsPath& to)
		{
			if (path == from)
				return to;
			if (!path.IsUnder(from))
				return std::nullopt;
			const size_t prefixLength = from.GetPath().empty() ? 0 : from.GetPath().size() + 1;
			Result<VfsPath> remapped = to.Join(path.GetPath().substr(prefixLength));
			if (!remapped.has_value())
				return std::nullopt;
			return std::move(*remapped);
		}

		static bool IsUnderOrEqual(const VfsPath& path, const VfsPath& directory)
		{
			return path == directory || path.IsUnder(directory);
		}

		// The project-relative lookup snapshot entry of a record.
		static ImportAssetLookupEntry MakeLookupEntry(const AssetRecord& record)
		{
			return {
				.SourcePath = record.SourcePath,
				.Handle = record.Metadata.Handle,
				.Kind = record.Metadata.Kind,
				.Type = record.Metadata.Type,
				.Owner = record.Metadata.Owner,
			};
		}

		static std::vector<ImportAssetLookupEntry> MakeLookupSnapshot(const AssetRegistry& registry)
		{
			std::vector<ImportAssetLookupEntry> snapshot;
			for (const AssetRecord* record : registry.GetRecords())
				snapshot.push_back(MakeLookupEntry(*record));
			return snapshot;
		}

		static const ImportAssetLookupEntry* FindLookupEntry(std::span<const ImportAssetLookupEntry> snapshot, const VfsPath& path)
		{
			const auto found = std::ranges::lower_bound(snapshot, path, std::less<>(), &ImportAssetLookupEntry::SourcePath);
			return found != snapshot.end() && found->SourcePath == path ? &*found : nullptr;
		}

		// The checks an import's result must pass before anything is stored (importers are trusted to be pure, not to be
		// bug-free): the main artifact first with the source's handle and type, then the sub-assets, sorted by unique key,
		// with derived handles.
		static Status ValidateImportResult(const ImportResult& result, const AssetMetadata& metadata, std::string_view importerId)
		{
			if (result.Artifacts.empty())
				return MakeError(ErrorCode::ImportFailed, "importer '{}' produced no artifact", importerId);
			const ImportedArtifact& main = result.Artifacts.front();
			if (main.Handle != metadata.Handle || !main.SubAssetKey.empty() || main.Type != metadata.Type)
			{
				return MakeError(ErrorCode::ImportFailed, "importer '{}' produced a main artifact {} of type {} for the {} {}", importerId,
					main.Handle.ToString(), AssetTypeToString(main.Type), AssetTypeToString(metadata.Type), metadata.Handle.ToString());
			}
			for (size_t index = 1; index < result.Artifacts.size(); ++index)
			{
				const ImportedArtifact& artifact = result.Artifacts[index];
				if (artifact.SubAssetKey.empty() || artifact.Type == AssetType::None
					|| artifact.Handle != DeriveSubAssetHandle(metadata.Handle, artifact.SubAssetKey)
					|| (index > 1 && !(result.Artifacts[index - 1].SubAssetKey < artifact.SubAssetKey)))
				{
					return MakeError(ErrorCode::ImportFailed, "importer '{}' produced an invalid sub-asset '{}'", importerId, artifact.SubAssetKey);
				}
			}
			return {};
		}

		static std::vector<SubAssetEntry> MakeSubAssetEntries(const ImportResult& result)
		{
			std::vector<SubAssetEntry> entries;
			for (size_t index = 1; index < result.Artifacts.size(); ++index)
			{
				const ImportedArtifact& artifact = result.Artifacts[index];
				entries.push_back({ .Key = artifact.SubAssetKey, .Handle = artifact.Handle, .Type = artifact.Type });
			}
			return entries;
		}

		// A dependency has exactly one owner (AssetMetadata.h): a read of another asset's dependency fails the import.
		static Status CheckDependencyOwners(std::span<const ImportDependencyRead> reads, std::span<const ImportAssetLookupEntry> snapshot,
			AssetHandle owner)
		{
			for (const ImportDependencyRead& read : reads)
			{
				const ImportAssetLookupEntry* entry = FindLookupEntry(snapshot, read.Path);
				if (entry == nullptr || entry->Kind != AssetMetaKind::Dependency || entry->Owner == owner)
					continue;
				const auto ownerEntry = std::ranges::find_if(snapshot, [entry](const ImportAssetLookupEntry& candidate)
				{
					return candidate.Handle == entry->Owner && candidate.Kind == AssetMetaKind::Asset;
				});
				const std::string ownerText = ownerEntry != snapshot.end() ? std::format("'{}' ({})", RelativeText(ownerEntry->SourcePath), entry->Owner.ToString())
																		   : entry->Owner.ToString();
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' is already a dependency of {}, and a dependency has exactly one owner", RelativeText(read.Path), ownerText))
						.WithHint("give each glTF its own copy of the file, or give the shared file a meta of its own (a standalone texture)"));
			}
			return {};
		}

		// A cached import of the same cache key as an import of `metadata`'s source (ImportRequest::ReuseBakes): only an import
		// that read and looked up nothing but its source and produced exactly its main artifact, without dependencies or
		// diagnostics (a bake), since the key then covers everything it depends on; the artifact takes the source's handle.
		static std::optional<CachedImport> AdoptCachedBake(CachedImport cached, const AssetMetadata& metadata, std::string_view importerId)
		{
			ImportResult& result = cached.Import;
			if (result.Artifacts.size() != 1 || !result.Dependencies.empty() || !result.Diagnostics.empty() || !cached.Reads.empty()
				|| !cached.Lookups.empty())
			{
				return std::nullopt;
			}
			result.Artifacts.front().Handle = metadata.Handle;
			if (!ValidateImportResult(result, metadata, importerId).has_value())
				return std::nullopt;
			return cached;
		}

		// The first cooked bake with `key` the request may reuse: under the source's own handle, under another source of the
		// project cache, then in the engine cooked cache, each cache's sources in handle order. A cache or entry that cannot
		// be read is skipped, the last such error kept as the search's warning. No bake when none holds one.
		static ReusableBakeSearch FindReusableBake(const ImportRequest& request, uint64_t key)
		{
			ReusableBakeSearch search;
			const std::string_view importerId = request.Importer->GetId();
			for (const AssetCache* cache : { request.Cache.get(), request.EngineCache.get() })
			{
				if (cache == nullptr)
					continue;
				Result<std::vector<AssetHandle>> sources = cache->FindSourcesWithKey(key);
				if (!sources.has_value())
				{
					search.Warning = sources.error().ToString();
					continue;
				}
				// The source's own entry first (a reimport bypasses the cache, but a bake that cannot be redone here is reused).
				if (const auto own = std::ranges::find(*sources, request.Metadata.Handle); cache == request.Cache.get() && own != sources->end())
					std::rotate(sources->begin(), own, own + 1);
				for (const AssetHandle source : *sources)
				{
					Result<std::optional<CachedImport>> cached = cache->Find(source, key);
					if (!cached.has_value())
					{
						search.Warning = cached.error().ToString();
						continue;
					}
					if (!cached->has_value())
						continue;
					if (std::optional<CachedImport> adopted = AdoptCachedBake(std::move(**cached), request.Metadata, importerId))
					{
						const bool isOwnEntry = cache == request.Cache.get() && source == request.Metadata.Handle;
						if (!isOwnEntry)
						{
							ENGINE_CORE_INFO("Asset import: '{}' needs a GPU bake this editor cannot run; it reuses the cooked bake of {} in '{}' (same cache key)",
								RelativeText(request.SourcePath), source, cache->GetRoot().ToString());
						}
						search.Bake = ReusableBake{ .Import = std::move(*adopted), .IsOwnEntry = isOwnEntry };
						return search;
					}
				}
			}
			return search;
		}

		// The import itself, a pure function of the request: on a job or inline, never touching the manager.
		static ImportOutput RunImport(const ImportRequest& request)
		{
			ImportOutput output;
			const auto fail = [&output, &request](Error error)
			{
				output.Failure = std::move(error).WithContext(std::format("while importing '{}'", RelativeText(request.SourcePath)));
				return std::move(output);
			};

			Result<FileInfo> sourceInfo = request.Vfs->GetInfo(request.SourcePath);
			if (!sourceInfo.has_value())
				return fail(std::move(sourceInfo).error());
			Result<Buffer> source = request.Vfs->ReadFile(request.SourcePath);
			if (!source.has_value())
				return fail(std::move(source).error());
			output.SourceInfo = *sourceInfo;
			output.SourceHash = XXH64(*source);
			output.Key = AssetCache::ComputeKey(*source, request.Importer->GetId(), request.Importer->GetVersion(), request.Settings.Get(),
				EngineCookVersion);

			const auto decodeAll = [&request](const ImportResult& result) -> Result<std::vector<AssetRef<Asset>>>
			{
				std::vector<AssetRef<Asset>> decoded;
				decoded.reserve(result.Artifacts.size());
				for (const ImportedArtifact& artifact : result.Artifacts)
				{
					ENGINE_TRY_ASSIGN(AssetRef<Asset> asset, request.Loaders->Load(artifact.Cooked, { .Registry = request.Registry, .Handle = artifact.Handle }));
					if (asset == nullptr || asset->GetAssetType() != artifact.Type)
						return MakeError(ErrorCode::ImportFailed, "artifact {} did not load as a {}", artifact.Handle.ToString(), AssetTypeToString(artifact.Type));
					decoded.push_back(std::move(asset));
				}
				return decoded;
			};

			if (request.UseCache && request.Cache != nullptr)
			{
				Result<std::optional<CachedImport>> cached = request.Cache->Find(request.Metadata.Handle, output.Key);
				if (!cached.has_value())
				{
					output.CacheWarning = cached.error().ToString();
				}
				else if (cached->has_value() && IsManifestCurrent(*request.Vfs, (*cached)->Reads, (*cached)->Lookups, request.Assets)
					&& ValidateImportResult((*cached)->Import, request.Metadata, request.Importer->GetId()).has_value())
				{
					Result<std::vector<AssetRef<Asset>>> decoded = decodeAll((*cached)->Import);
					if (decoded.has_value())
					{
						output.Import = std::move(**cached);
						// A manifest does not record a dependency's owner (AssetCache.h). IsManifestCurrent has just matched every
						// lookup with this snapshot's entry, so the entry is what a fresh import would have found, owner included;
						// ProcessChanges compares the lookups with the registry in full, so they must read the same.
						for (ImportAssetLookup& lookup : output.Import.Lookups)
						{
							if (const ImportAssetLookupEntry* entry = FindLookupEntry(request.Assets, lookup.Path); entry != nullptr && lookup.Found.has_value())
								lookup.Found = *entry;
						}
						output.Decoded = std::move(*decoded);
						output.FromCache = true;
						return output;
					}
				}
			}

			if (request.ReuseBakes)
			{
				ReusableBakeSearch search = FindReusableBake(request, output.Key);
				if (search.Warning.has_value())
					output.CacheWarning = std::move(search.Warning);
				if (std::optional<ReusableBake>& reused = search.Bake; reused.has_value())
				{
					Result<std::vector<AssetRef<Asset>>> decoded = decodeAll(reused->Import.Import);
					if (decoded.has_value())
					{
						output.Import = std::move(reused->Import);
						output.Decoded = std::move(*decoded);
						output.FromCache = true;
						output.ReusedBake = !reused->IsOwnEntry;
						return output;
					}
				}
			}

			ImportContext context({
				.Vfs = request.Vfs,
				.SourcePath = request.SourcePath,
				.SourceBytes = *source,
				.Settings = request.Settings,
				.Registry = request.Registry,
				.Assets = request.Assets,
				.EnvironmentBaker = request.EnvironmentBaker,
				.ScriptDiagnostics = request.ScriptDiagnostics,
			});
			Result<ImportResult> imported = request.Importer->Import(context, request.Metadata);
			output.Import.Reads = context.GetDependencyReads();
			output.Import.Lookups = context.GetLookups();
			if (!imported.has_value())
				return fail(std::move(imported).error());
			if (Status valid = ValidateImportResult(*imported, request.Metadata, request.Importer->GetId()); !valid.has_value())
				return fail(std::move(valid).error());
			if (Status owners = CheckDependencyOwners(output.Import.Reads, request.Assets, request.Metadata.Handle); !owners.has_value())
				return fail(std::move(owners).error());
			Result<std::vector<AssetRef<Asset>>> decoded = decodeAll(*imported);
			if (!decoded.has_value())
				return fail(std::move(decoded).error());
			output.Import.Import = std::move(*imported);
			output.Decoded = std::move(*decoded);
			return output;
		}

		static Result<std::vector<AssetKnownLocation>> ParseAssetLocations(std::string_view text, std::string_view scheme)
		{
			ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
			const JsonReader root(document);
			ENGINE_TRY(root.ExpectType(JsonType::Object));
			ENGINE_TRY(root.ReadFormatHeader(AssetLocationsFormatName, AssetLocationsVersion, AssetLocationsVersion));
			ENGINE_TRY_ASSIGN(const JsonReader assets, root.GetMember("Assets"));
			ENGINE_TRY_ASSIGN(const size_t count, assets.GetArraySize());
			std::vector<AssetKnownLocation> locations;
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, assets.GetElement(index));
				ENGINE_TRY_ASSIGN(const UUID handle, element.ReadMember<UUID>("Handle"));
				ENGINE_TRY_ASSIGN(const std::string path, element.ReadMember<std::string>("Path"));
				ENGINE_TRY_ASSIGN(VfsPath sourcePath, VfsPath::Create(scheme, path));
				if (handle.IsValid())
					locations.push_back({ .Handle = handle, .SourcePath = std::move(sourcePath) });
			}
			std::ranges::stable_sort(locations, std::less<>(), &AssetKnownLocation::Handle);
			const auto duplicates = std::ranges::unique(locations, std::equal_to<>(), &AssetKnownLocation::Handle);
			locations.erase(duplicates.begin(), duplicates.end());
			return locations;
		}

		static std::string WriteAssetLocations(std::span<const AssetKnownLocation> locations)
		{
			JsonWriter writer(JsonStyle::Pretty);
			writer.BeginObject();
			writer.WriteKey("Format");
			writer.WriteString(AssetLocationsFormatName);
			writer.WriteKey("Version");
			writer.WriteUInt(AssetLocationsVersion);
			writer.WriteKey("Assets");
			writer.BeginArray();
			for (const AssetKnownLocation& location : locations)
			{
				writer.BeginObject();
				writer.WriteKey("Handle");
				writer.WriteUUID(location.Handle);
				writer.WriteKey("Path");
				writer.WriteString(location.SourcePath.GetPath());
				writer.EndObject();
			}
			writer.EndArray();
			writer.EndObject();
			Result<std::string> text = writer.Finish();
			return text.has_value() ? std::move(*text) : std::string();
		}

		// The canonical EngineAssets.json text of the procedural entries (BuiltinAssets.h), for the catalogue of an editor
		// without engine://.
		static std::string WriteProceduralCatalogue()
		{
			JsonWriter writer(JsonStyle::Pretty);
			writer.BeginObject();
			writer.WriteKey("Format");
			writer.WriteString(BuiltinAssetCatalog::FormatName);
			writer.WriteKey("Version");
			writer.WriteUInt(BuiltinAssetCatalog::CurrentVersion);
			writer.WriteKey("Assets");
			writer.BeginArray();
			for (const BuiltinAssetEntry& entry : GetProceduralBuiltinEntries())
			{
				writer.BeginObject();
				writer.WriteKey("Handle");
				writer.WriteUUID(entry.Handle);
				writer.WriteKey("Path");
				writer.WriteString(entry.Path);
				writer.WriteKey("Type");
				writer.WriteString(AssetTypeToString(entry.Type));
				writer.WriteKey("Source");
				writer.WriteString("Procedural");
				writer.WriteKey("File");
				writer.WriteString("");
				writer.WriteKey("Importer");
				writer.WriteString("");
				writer.WriteKey("Settings");
				writer.BeginObject();
				writer.EndObject();
				writer.WriteKey("Generator");
				writer.WriteString("");
				writer.EndObject();
			}
			writer.EndArray();
			writer.EndObject();
			Result<std::string> text = writer.Finish();
			return text.has_value() ? std::move(*text) : std::string();
		}

	}

	namespace {

		// The project-scoped part of the manager's state, present while a project is open. A namespace-scope struct rather
		// than one nested in State: a nested class's default member initializers are parsed only once the enclosing class is
		// complete, so Clang with libstdc++ finds it not default-constructible where std::optional<OpenProjectState>::emplace
		// checks (CWG 1397).
		struct OpenProjectState
		{
			AssetProjectSpecification Specification{};
			Ref<AssetCache> Cache{};
			Scope<AssetHotReloader> HotReloader{};
			std::map<VfsPath, KnownFile> KnownFiles{};
			// A read-only editor's in-memory metas (source path -> meta), registered again after every scan.
			std::map<VfsPath, AssetMetadata> TransientMetas{};
		};

	}

	struct EditorAssetManager::State
	{
		State(EditorAssetManager& self, const EditorAssetManagerSpecification& specification)
			: Self(&self), Specification(specification), Writer(*specification.Vfs), Alive(CreateRef<bool>(true))
		{
		}

		EditorAssetManager* Self = nullptr; // the owner (documented back-reference)
		EditorAssetManagerSpecification Specification;
		AssetRegistry Registry;
		AssetDependencyGraph Graph;
		BuiltinAssetCatalog Builtins;
		AssetWriter Writer;
		std::optional<OpenProjectState> OpenProject;
		uint64_t ProjectSerial = 0; // changes at every open and close, so completions of a closed project are dropped

		std::map<AssetHandle, LoadedArtifact> Loaded; // project artifacts and File/Generated built-ins
		std::map<AssetHandle, ImportRecord> Imports;  // by source handle
		std::map<AssetHandle, Error> BuiltinFailures; // remembered failures of File/Generated built-ins
		std::map<AssetHandle, uint64_t> Generations;  // tickets when there is no hot reloader
		std::map<AssetHandle, uint32_t> InFlight;     // asynchronous imports per source
		std::set<std::string> CacheWarnings;          // logged once each
		WriteObserver Observer;
		ExternalChangeListener ChangeListener;
		ReloadListener OnReload;
		bool Deferred = false;
		std::set<AssetHandle> HeldReimports; // Refresh's changes while deferred
		std::vector<AssetExternalChange> HeldChanges;
		std::optional<DryRunSnapshot> DryRun;
		uint32_t OutstandingTasks = 0; // publications posted to the main thread, not yet run
		Ref<bool> Alive;               // expires with the manager: posted tasks check it

		// --- Small helpers ----------------------------------------------------------------------------------------------

		[[nodiscard]] bool IsReadOnly() const { return OpenProject.has_value() && OpenProject->Specification.ReadOnly; }

		[[nodiscard]] const VfsPath& GetAssetsRoot() const { return OpenProject->Specification.AssetsRoot; }

		[[nodiscard]] bool IsUnderAssets(const VfsPath& path) const
		{
			return OpenProject.has_value() && path.IsUnder(GetAssetsRoot()) && path != GetAssetsRoot();
		}

		[[nodiscard]] const BuiltinAssetEntry* FindBuiltin(AssetHandle handle) const
		{
			if (!IsBuiltinAssetHandle(handle))
				return nullptr;
			if (const BuiltinAssetEntry* entry = Builtins.Find(handle))
				return entry;
			const std::span<const BuiltinAssetEntry> procedural = GetProceduralBuiltinEntries();
			const auto found = std::ranges::find(procedural, handle, &BuiltinAssetEntry::Handle);
			return found == procedural.end() ? nullptr : &*found;
		}

		[[nodiscard]] const BuiltinAssetEntry* FindBuiltinByPath(std::string_view path) const
		{
			if (const BuiltinAssetEntry* entry = Builtins.FindByPath(path))
				return entry;
			const std::span<const BuiltinAssetEntry> procedural = GetProceduralBuiltinEntries();
			const auto found = std::ranges::find(procedural, path, &BuiltinAssetEntry::Path);
			return found == procedural.end() ? nullptr : &*found;
		}

		void LogCacheWarning(const std::string& message)
		{
			if (CacheWarnings.insert(message).second)
				ENGINE_CORE_WARN("Asset cache: {} (assets are imported again instead)", message);
		}

		// --- Tickets (§7.5 race rule 2) ---------------------------------------------------------------------------------

		[[nodiscard]] AssetReimportTicket BeginTicket(AssetHandle handle, uint64_t contentHash)
		{
			if (OpenProject.has_value() && OpenProject->HotReloader != nullptr)
				return OpenProject->HotReloader->BeginReimport(handle, contentHash);
			return { .Handle = handle, .ContentHash = contentHash, .Generation = ++Generations[handle] };
		}

		[[nodiscard]] bool IsTicketCurrent(const AssetReimportTicket& ticket) const
		{
			if (OpenProject.has_value() && OpenProject->HotReloader != nullptr)
				return OpenProject->HotReloader->IsCurrent(ticket);
			const auto found = Generations.find(ticket.Handle);
			return found != Generations.end() && found->second == ticket.Generation;
		}

		// --- Settings ---------------------------------------------------------------------------------------------------

		[[nodiscard]] Result<VariantValue> MergeSettings(const IAssetImporter& importer, const VariantValue& base, const Json& patch) const
		{
			const std::string_view typeName = importer.GetSettingsTypeName();
			if (typeName.empty())
				return MakeError(ErrorCode::InvalidArgument, "importer '{}' has no import settings", importer.GetId());
			const StructInfo* settingsType = Specification.Registry->FindStruct(typeName);
			if (settingsType == nullptr)
				return MakeError(ErrorCode::NotFound, "the settings type '{}' of importer '{}' is not registered", typeName, importer.GetId());

			ObjectPtr settings = settingsType->CreateDefault();
			if (!base.IsNull())
			{
				// The stored settings: tolerant, so a field a newer importer dropped is ignored rather than fatal.
				ENGINE_TRY(settingsType->FromJson(settings.get(), JsonReader(base.Get()), { .Schemas = nullptr, .Strict = false, .Diagnostics = nullptr }));
			}
			if (!patch.is_null())
				ENGINE_TRY(settingsType->ApplyMergePatch(settings.get(), JsonReader(patch), { .Schemas = nullptr, .Strict = true, .Diagnostics = nullptr }));
			ENGINE_TRY_ASSIGN(Json merged, settingsType->ToJson(settings.get()));
			return VariantValue(std::move(merged));
		}

		// The complete settings an import of `metadata` uses: null for an importer without settings.
		[[nodiscard]] Result<VariantValue> GetImportSettings(const IAssetImporter& importer, const AssetMetadata& metadata) const
		{
			if (importer.GetSettingsTypeName().empty())
				return VariantValue();
			return MergeSettings(importer, metadata.Settings, Json());
		}

		// --- Diagnostics and events -------------------------------------------------------------------------------------

		void AppendEvent(EngineEventType type, AssetHandle handle, const VfsPath& path, std::string message)
		{
			if (DryRun.has_value())
				return;
			Specification.Events->Append({
				.Seq = 0,
				.Tick = std::nullopt,
				.Type = type,
				.Id = handle,
				.Path = Utils::RelativeText(path),
				.Name = {},
				.Message = std::move(message),
				.Dirty = false,
			});
		}

		void NotifyChange(const AssetExternalChange& change)
		{
			if (ChangeListener)
				ChangeListener(change);
		}

		// --- Registry edits ---------------------------------------------------------------------------------------------

		// Forgets every trace of `handle` (a .meta that went away): its record, loaded artifacts, import and diagnostics.
		void Unregister(AssetHandle handle)
		{
			const AssetRecord* record = Registry.Find(handle);
			if (record == nullptr)
				return;
			std::vector<AssetHandle> handles = { handle };
			for (const SubAssetEntry& entry : record->Metadata.SubAssets)
				handles.push_back(entry.Handle);
			if (const auto import = Imports.find(handle); import != Imports.end())
			{
				for (const AssetHandle artifact : import->second.Artifacts)
					handles.push_back(artifact);
				Imports.erase(import);
			}
			// Registered (checked above), so the removal cannot fail.
			[[maybe_unused]] const Status removed = Registry.Remove(handle);
			ENGINE_CORE_ASSERT(removed.has_value(), "Unregistering {} failed: {}", handle.ToString(), removed.has_value() ? std::string() : removed.error().ToString());
			Graph.RemoveAsset(handle);
			for (const AssetHandle artifact : handles)
			{
				Loaded.erase(artifact);
				Self->ClearDiagnostics(artifact, GetAssetDiagnosticCodes());
			}
		}

		// Registers `record` after a .meta appeared (written, moved in, or found by a scan's batch), replacing what was
		// registered at its source path.
		void RegisterRecord(AssetRecord record)
		{
			const AssetHandle handle = record.Metadata.Handle;
			if (const AssetRecord* atPath = Registry.FindBySourcePath(record.SourcePath); atPath != nullptr && atPath->Metadata.Handle != handle)
				Unregister(atPath->Metadata.Handle);
			if (Status added = Registry.Add(std::move(record)); !added.has_value())
			{
				ENGINE_CORE_WARN("Asset registry: {}", added.error().ToString());
				return;
			}
			Self->ClearDiagnostics(handle, std::array<std::string_view, 1>{ AssetMissingCode });
		}

		// The .meta at `metaPath` was written or moved in: register, re-register or update it. A changed importer, type or
		// settings makes its source's import stale.
		void RegisterMetaFile(const VfsPath& metaPath)
		{
			Result<std::string> text = Specification.Vfs->ReadText(metaPath);
			if (!text.has_value())
				return;
			Result<AssetMetadata> metadata = ParseAssetMetadata(*text, Utils::RelativeText(metaPath));
			if (!metadata.has_value())
			{
				ENGINE_CORE_WARN("Asset registry: {}", metadata.error().ToString());
				return;
			}
			Result<VfsPath> sourcePath = GetSourcePathOfMeta(metaPath);
			if (!sourcePath.has_value())
				return;
			OpenProject->TransientMetas.erase(*sourcePath);

			const AssetHandle handle = metadata->Handle;
			const AssetRecord* existing = Registry.Find(handle);
			if (existing != nullptr && existing->SourcePath == *sourcePath)
			{
				const bool inputsChanged = existing->Metadata.Importer != metadata->Importer || existing->Metadata.Type != metadata->Type
					|| existing->Metadata.Settings != metadata->Settings || existing->Metadata.Kind != metadata->Kind
					|| existing->Metadata.Owner != metadata->Owner;
				AssetRecord updated{ .Metadata = std::move(*metadata), .SourcePath = *sourcePath, .MetaPath = metaPath };
				if (Status status = Registry.Update(std::move(updated)); !status.has_value())
				{
					ENGINE_CORE_WARN("Asset registry: {}", status.error().ToString());
					return;
				}
				if (inputsChanged)
					InvalidateSource(handle, true);
				return;
			}
			if (existing != nullptr)
			{
				// The path the registry holds keeps the handle (AssetRegistry::Scan, keeper rule 2); the next scan reports the
				// copy as ASSET_DUPLICATE_HANDLE with its fix. What was registered at the copy's path is no longer in its file.
				if (const AssetRecord* atPath = Registry.FindBySourcePath(*sourcePath); atPath != nullptr)
					Unregister(atPath->Metadata.Handle);
				ENGINE_CORE_WARN("Asset registry: '{}' carries the handle {} of '{}'; the next scan reports it", metaPath.ToString(), handle.ToString(),
					existing->MetaPath.ToString());
				return;
			}
			RegisterRecord({ .Metadata = std::move(*metadata), .SourcePath = *sourcePath, .MetaPath = metaPath });
		}

		// --- Change bookkeeping -----------------------------------------------------------------------------------------

		// The source `source` must be imported again: its source, a dependency file, its settings or a looked-up asset
		// changed. Its remembered failure is dropped. When `schedule` and it was imported before, the reimport starts now
		// (synchronously during a dry run, else on a job); while reloads are deferred it is held.
		void InvalidateSource(AssetHandle source, bool schedule)
		{
			const auto import = Imports.find(source);
			if (import == Imports.end())
				return;
			import->second.Failure.reset();
			if (!schedule)
				return;
			if (Deferred)
			{
				HeldReimports.insert(source);
				return;
			}
			if (DryRun.has_value())
				static_cast<void>(ImportNow(source, true, true));
			else
				StartAsyncImport(source, true, AssetHandle(), true);
		}

		void UpdateKnownFile(const VfsPath& path, std::optional<uint64_t> hash)
		{
			if (!OpenProject.has_value() || !IsUnderAssets(path))
				return;
			Result<FileInfo> info = Specification.Vfs->GetInfo(path);
			if (!info.has_value() || info->IsDirectory)
			{
				OpenProject->KnownFiles.erase(path);
				return;
			}
			OpenProject->KnownFiles[path] = { .Size = info->Size, .ModificationTime = info->ModificationTime, .Hash = hash };
		}

		// The imports that read `path` or whose source it is.
		[[nodiscard]] std::vector<AssetHandle> FindImportsUsing(const VfsPath& path) const
		{
			std::vector<AssetHandle> users;
			for (const auto& [source, import] : Imports)
			{
				const AssetRecord* record = Registry.Find(source);
				const bool isSource = record != nullptr && record->SourcePath == path;
				const bool isRead = std::ranges::any_of(import.Reads, [&path](const ImportDependencyRead& read)
				{
					return read.Path == path;
				});
				if (isSource || isRead)
					users.push_back(source);
			}
			return users;
		}

		// --- The writer's events (§7.5 race rule 1) ---------------------------------------------------------------------

		void OnWrite(const AssetWriteEvent& event)
		{
			if (OpenProject.has_value())
			{
				switch (event.Kind)
				{
					case AssetWriteKind::Written:          OnFileWritten(event.Path, event.ContentHash, event.Backup); break;
					case AssetWriteKind::Removed:          OnPathRemoved(event.Path); break;
					case AssetWriteKind::Moved:            OnPathMoved(event.From, event.Path); break;
					case AssetWriteKind::DirectoryCreated: break;
				}
			}
			if (Observer)
				Observer(event);
		}

		void OnFileWritten(const VfsPath& path, uint64_t contentHash, const VfsPath& backup)
		{
			if (!IsUnderAssets(path))
				return;
			UpdateKnownFile(path, contentHash);
			// The backup the mount kept of the replaced file is the editor's write too: Refresh must not report it (race rule 1).
			if (!backup.IsEmpty())
				UpdateKnownFile(backup, std::nullopt);
			if (Utils::IsMetaFile(path))
			{
				RegisterMetaFile(path);
				return;
			}
			for (const AssetHandle source : FindImportsUsing(path))
				InvalidateSource(source, true);
		}

		void OnPathRemoved(const VfsPath& path)
		{
			if (!OpenProject.has_value())
				return;
			std::vector<AssetHandle> unregistered;
			std::vector<AssetHandle> stale;
			for (const AssetRecord* record : Registry.GetRecords())
			{
				if (Utils::IsUnderOrEqual(record->MetaPath, path))
					unregistered.push_back(record->Metadata.Handle);
			}
			for (const AssetHandle handle : unregistered)
				Unregister(handle);
			for (auto iterator = OpenProject->TransientMetas.begin(); iterator != OpenProject->TransientMetas.end();)
			{
				if (Utils::IsUnderOrEqual(iterator->first, path))
				{
					Unregister(iterator->second.Handle);
					iterator = OpenProject->TransientMetas.erase(iterator);
				}
				else
				{
					++iterator;
				}
			}
			for (const auto& [source, import] : Imports)
			{
				const bool usesRemoved = Utils::IsUnderOrEqual(import.SourcePath, path) || std::ranges::any_of(import.Reads, [&path](const ImportDependencyRead& read)
				{
					return Utils::IsUnderOrEqual(read.Path, path);
				});
				if (usesRemoved)
					stale.push_back(source);
			}
			for (auto iterator = OpenProject->KnownFiles.begin(); iterator != OpenProject->KnownFiles.end();)
			{
				if (Utils::IsUnderOrEqual(iterator->first, path))
					iterator = OpenProject->KnownFiles.erase(iterator);
				else
					++iterator;
			}
			// A removed source keeps its last good version until its .meta goes too; a removed dependency file is reimported
			// (and fails, keeping the last good version, until it comes back).
			for (const AssetHandle source : stale)
			{
				const AssetRecord* record = Registry.Find(source);
				InvalidateSource(source, record != nullptr && !Utils::IsUnderOrEqual(record->SourcePath, path));
			}
		}

		void OnPathMoved(const VfsPath& from, const VfsPath& to)
		{
			if (!OpenProject.has_value())
				return;
			const bool fromInside = Utils::IsUnderOrEqual(from, GetAssetsRoot());
			const bool toInside = IsUnderAssets(to);

			// Records whose .meta moved follow it, keeping their handles (§7.3); a .meta moved out unregisters.
			std::vector<std::pair<AssetHandle, VfsPath>> renames;
			std::vector<AssetHandle> unregistered;
			if (fromInside)
			{
				for (const AssetRecord* record : Registry.GetRecords())
				{
					const std::optional<VfsPath> newMetaPath = Utils::RemapPath(record->MetaPath, from, to);
					if (!newMetaPath.has_value())
						continue;
					if (!toInside || !Utils::IsMetaFile(*newMetaPath))
					{
						unregistered.push_back(record->Metadata.Handle);
						continue;
					}
					Result<VfsPath> newSourcePath = GetSourcePathOfMeta(*newMetaPath);
					if (newSourcePath.has_value())
						renames.emplace_back(record->Metadata.Handle, std::move(*newSourcePath));
					else
						unregistered.push_back(record->Metadata.Handle);
				}
			}
			for (const AssetHandle handle : unregistered)
				Unregister(handle);
			for (const auto& [handle, newSourcePath] : renames)
			{
				if (Status renamed = Registry.Rename(handle, newSourcePath); !renamed.has_value())
				{
					ENGINE_CORE_WARN("Asset registry: {}", renamed.error().ToString());
					Unregister(handle);
				}
			}

			// The imports remember paths: they follow the move, so change detection stays exact.
			for (auto& entry : Imports)
			{
				ImportRecord& import = entry.second;
				if (std::optional<VfsPath> remapped = Utils::RemapPath(import.SourcePath, from, to))
					import.SourcePath = std::move(*remapped);
				for (ImportDependencyRead& read : import.Reads)
				{
					if (std::optional<VfsPath> remapped = Utils::RemapPath(read.Path, from, to))
						read.Path = std::move(*remapped);
				}
				std::ranges::sort(import.Reads, std::less<>(), &ImportDependencyRead::Path);
			}
			std::map<VfsPath, KnownFile> known;
			for (auto& [path, file] : OpenProject->KnownFiles)
			{
				if (!Utils::RemapPath(path, from, to).has_value())
					known.emplace(path, file);
			}
			OpenProject->KnownFiles = std::move(known);

			if (!toInside)
				return;
			// What arrived at the destination: metas register, other files are seen as written.
			std::vector<VfsPath> arrived;
			if (Result<FileInfo> info = Specification.Vfs->GetInfo(to); info.has_value() && info->IsDirectory)
			{
				if (Result<std::vector<VfsEntry>> listed = Specification.Vfs->List(to, true); listed.has_value())
				{
					for (const VfsEntry& entry : *listed)
					{
						if (!entry.Info.IsDirectory)
							arrived.push_back(entry.Path);
					}
				}
			}
			else
			{
				arrived.push_back(to);
			}
			for (const VfsPath& path : arrived)
			{
				UpdateKnownFile(path, std::nullopt);
				if (Utils::IsMetaFile(path))
				{
					if (!fromInside || Registry.FindBySourcePath(GetSourcePathOfMeta(path).value_or(VfsPath())) == nullptr)
						RegisterMetaFile(path);
				}
				else if (!fromInside)
				{
					for (const AssetHandle source : FindImportsUsing(path))
						InvalidateSource(source, true);
				}
			}
		}

		// --- Imports ----------------------------------------------------------------------------------------------------

		// The request for importing the source `source`, or the error that stops it before it starts.
		[[nodiscard]] Result<ImportRequest> BuildRequest(AssetHandle source, bool useCache) const
		{
			const AssetRecord* record = Registry.Find(source);
			if (record == nullptr || record->Metadata.Kind != AssetMetaKind::Asset)
				return MakeError(ErrorCode::NotFound, "no registered asset has handle {}", source.ToString());
			const IAssetImporter* importer = Specification.Importers->FindById(record->Metadata.Importer);
			if (importer == nullptr)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' names the importer '{}', which this build does not have", Utils::RelativeText(record->SourcePath),
						record->Metadata.Importer))
						.WithHint("the importer arrives with a later milestone, or the .meta names the wrong one"));
			}
			Result<VariantValue> settings = GetImportSettings(*importer, record->Metadata);
			if (!settings.has_value())
				return std::unexpected(std::move(settings).error().WithContext(std::format("in the settings of '{}'", Utils::RelativeText(record->MetaPath))));
			// An environment is a GPU bake (§8.6): without a baker its import reuses a cooked bake with the same key (§7.4).
			const bool reuseBakes = importer->GetMainType() == AssetType::Environment && Specification.EnvironmentBaker == nullptr;
			Ref<AssetCache> engineCache;
			if (reuseBakes && Specification.Vfs->IsMounted(EngineCacheScheme))
			{
				Result<VfsPath> engineCacheRoot = VfsPath::Create(EngineCacheScheme, {});
				if (engineCacheRoot.has_value())
					engineCache = CreateRef<AssetCache>(*Specification.Vfs, std::move(*engineCacheRoot));
			}
			return ImportRequest{
				.Vfs = Specification.Vfs,
				.Registry = Specification.Registry,
				.Importer = importer,
				.Loaders = Specification.Loaders,
				.EnvironmentBaker = Specification.EnvironmentBaker,
				.ScriptDiagnostics = Specification.ScriptDiagnostics,
				.Cache = OpenProject->Cache,
				.ReuseBakes = reuseBakes,
				.EngineCache = std::move(engineCache),
				.Metadata = record->Metadata,
				.SourcePath = record->SourcePath,
				.Settings = std::move(*settings),
				.Assets = Utils::MakeLookupSnapshot(Registry),
				.UseCache = useCache,
			};
		}

		void RecordFailure(AssetHandle source, const Error& error, const VfsPath& sourcePath, std::vector<ImportDependencyRead> reads)
		{
			ImportRecord& import = Imports[source];
			import.SourcePath = sourcePath;
			import.Failure = error;
			// The inputs the failed import used: an unchanged .meta must not make it look stale (only a change retries it).
			if (const AssetRecord* record = Registry.Find(source))
			{
				import.Importer = record->Metadata.Importer;
				import.Settings = record->Metadata.Settings;
			}
			// The reads so far, so that a dependency file that comes back or changes ends the remembered failure.
			for (ImportDependencyRead& read : reads)
			{
				if (std::ranges::find(import.Reads, read.Path, &ImportDependencyRead::Path) == import.Reads.end())
					import.Reads.push_back(std::move(read));
			}
			std::ranges::sort(import.Reads, std::less<>(), &ImportDependencyRead::Path);
			Self->ClearDiagnostics(source, std::array<std::string_view, 1>{ AssetImportFailedCode });
			Self->ReportDiagnostic({
				.Severity = DiagnosticSeverity::Error,
				.Code = std::string(AssetImportFailedCode),
				.Asset = source,
				.Path = Utils::RelativeText(sourcePath),
				.Message = Error(error).WithHint(std::string()).ToString(), // the hint is the diagnostic's own member
				.Hint = error.GetHint(),
				.Subject = {},
				.AutoFixable = false,
			});
			AppendEvent(EngineEventType::AssetImportFailed, source, sourcePath, error.ToString());
		}

		// Publishes a finished import of `source` on the main thread: dependency metas, the .meta's sub-assets, the cache, the
		// artifacts (a new version of each that changed), diagnostics, graph edges and path dependents. Returns the outcome,
		// or the import's error (recorded as a remembered failure). `reload`: a reimport of something already published,
		// which appends AssetReloaded; `scheduleDependents`: reimport the published asset's dependents afterwards (hot reload).
		Result<AssetImportOutcome> ApplyImport(AssetHandle source, const AssetReimportTicket& ticket, ImportOutput output, bool reload,
			bool scheduleDependents)
		{
			if (!IsTicketCurrent(ticket))
				return MakeError(ErrorCode::Cancelled, "the import of {} was superseded by a newer one", source.ToString());
			const AssetRecord* record = Registry.Find(source);
			if (record == nullptr || record->Metadata.Kind != AssetMetaKind::Asset)
				return MakeError(ErrorCode::NotFound, "asset {} was unregistered while it imported", source.ToString());
			const VfsPath sourcePath = record->SourcePath;
			if (output.CacheWarning.has_value())
				LogCacheWarning(*output.CacheWarning);
			if (output.Failure.has_value())
			{
				RecordFailure(source, *output.Failure, sourcePath, std::move(output.Import.Reads));
				return std::unexpected(std::move(*output.Failure));
			}

			ImportResult& result = output.Import.Import;
			WriteDependencyMetas(source, output.Import.Reads);
			const std::vector<SubAssetEntry> subAssets = Utils::MakeSubAssetEntries(result);
			UpdateSubAssets(source, subAssets);

			if ((!output.FromCache || output.ReusedBake) && !DryRun.has_value() && OpenProject->Cache != nullptr)
			{
				if (Status stored = OpenProject->Cache->Store(source, output.Key, output.Import); !stored.has_value())
					LogCacheWarning(stored.error().ToString());
			}

			// Publish: a new version for every artifact whose cooked bytes changed; artifacts the import no longer produces
			// are unloaded.
			ImportRecord& import = Imports[source];
			std::vector<AssetHandle> artifacts;
			bool changed = false;
			for (size_t index = 0; index < result.Artifacts.size(); ++index)
			{
				const ImportedArtifact& artifact = result.Artifacts[index];
				artifacts.push_back(artifact.Handle);
				const uint64_t hash = XXH64(artifact.Cooked);
				LoadedArtifact& loaded = Loaded[artifact.Handle];
				Self->ClearDiagnostics(artifact.Handle, ImportDiagnosticCodes);
				if (loaded.Value != nullptr && loaded.ContentHash == hash)
					continue;
				loaded = { .Value = output.Decoded[index], .ContentHash = hash };
				Self->BumpVersion(artifact.Handle);
				changed = true;
			}
			for (const AssetHandle previous : import.Artifacts)
			{
				if (std::ranges::find(artifacts, previous) == artifacts.end())
				{
					Loaded.erase(previous);
					changed = true;
				}
			}

			for (AssetDiagnostic& diagnostic : result.Diagnostics)
			{
				if (!diagnostic.Asset.IsValid())
					diagnostic.Asset = source;
				if (diagnostic.Path.empty())
					diagnostic.Path = Utils::RelativeText(sourcePath);
				Self->ReportDiagnostic(diagnostic);
			}
			Graph.SetDependencies(source, result.Dependencies);

			std::vector<AssetHandle> lookedUp;
			for (const ImportAssetLookup& lookup : output.Import.Lookups)
			{
				if (lookup.Found.has_value() && lookup.Found->Kind == AssetMetaKind::Asset)
					lookedUp.push_back(lookup.Found->Handle);
			}
			std::ranges::sort(lookedUp);
			lookedUp.erase(std::ranges::unique(lookedUp).begin(), lookedUp.end());

			if (OpenProject.has_value())
			{
				const auto knownSource = OpenProject->KnownFiles.find(sourcePath);
				if (knownSource != OpenProject->KnownFiles.end() && output.SourceInfo.has_value() && knownSource->second.Size == output.SourceInfo->Size
					&& knownSource->second.ModificationTime == output.SourceInfo->ModificationTime)
				{
					knownSource->second.Hash = output.SourceHash;
				}
			}

			import.SourcePath = sourcePath;
			import.Key = output.Key;
			import.SourceHash = output.SourceHash;
			if (const AssetRecord* current = Registry.Find(source))
			{
				import.Importer = current->Metadata.Importer;
				import.Settings = current->Metadata.Settings;
			}
			import.Reads = output.Import.Reads;
			import.Lookups = output.Import.Lookups;
			import.Artifacts = std::move(artifacts);
			import.LookedUp = std::move(lookedUp);
			import.Failure.reset();

			if (reload && changed)
			{
				AppendEvent(EngineEventType::AssetReloaded, source, sourcePath, {});
				if (OnReload && !DryRun.has_value())
					OnReload(source);
			}
			if (scheduleDependents && changed)
			{
				for (const AssetHandle dependent : Graph.GetTransitiveDependents(source))
				{
					if (Imports.contains(dependent))
						InvalidateSource(dependent, true);
				}
			}
			return AssetImportOutcome{
				.Handle = source,
				.SubAssets = subAssets,
				.Diagnostics = std::move(result.Diagnostics),
				.FromCache = output.FromCache,
			};
		}

		// Every file the import read that still has no .meta gets a dependency meta owned by `source` (§6.4).
		void WriteDependencyMetas(AssetHandle source, std::span<const ImportDependencyRead> reads)
		{
			for (const ImportDependencyRead& read : reads)
			{
				if (!IsUnderAssets(read.Path) || Registry.FindBySourcePath(read.Path) != nullptr)
					continue;
				Result<VfsPath> metaPath = GetMetaPath(read.Path);
				if (!metaPath.has_value() || Specification.Vfs->Exists(*metaPath))
					continue;
				if (IsReadOnly())
				{
					// Transient, with the handle a read-only editor gives every file without a .meta: stable for the session.
					AddTransient(read.Path, MakeDependencyMetadata(AssetHandle(Hash64(0, read.Path.GetPath())), source));
					continue;
				}
				const std::string text = SerializeAssetMetadata(Self->CreateDependencyMetadata(source));
				if (Status written = Writer.Write(*metaPath, AsBytes(text)); !written.has_value())
					ENGINE_CORE_WARN("Asset registry: the dependency meta of '{}' cannot be written: {}", read.Path.ToString(), written.error().ToString());
			}
		}

		// Rewrites the source's .meta when the import's sub-assets differ from it.
		void UpdateSubAssets(AssetHandle source, const std::vector<SubAssetEntry>& subAssets)
		{
			const AssetRecord* record = Registry.Find(source);
			if (record == nullptr || record->Metadata.SubAssets == subAssets)
				return;
			AssetRecord updated = *record;
			updated.Metadata.SubAssets = subAssets;
			const bool transient = OpenProject->TransientMetas.contains(updated.SourcePath);
			if (IsReadOnly() || transient)
			{
				if (transient)
					OpenProject->TransientMetas[updated.SourcePath] = updated.Metadata;
				if (Status status = Registry.Update(std::move(updated)); !status.has_value())
					ENGINE_CORE_WARN("Asset registry: {}", status.error().ToString());
				return;
			}
			const std::string text = SerializeAssetMetadata(updated.Metadata);
			if (Status written = Writer.Write(updated.MetaPath, AsBytes(text)); !written.has_value())
			{
				ENGINE_CORE_WARN("Asset registry: the sub-assets of '{}' cannot be written: {}", updated.MetaPath.ToString(), written.error().ToString());
				// The registry follows the import all the same, so the sub-assets resolve in this session.
				if (Status status = Registry.Update(std::move(updated)); !status.has_value())
					ENGINE_CORE_WARN("Asset registry: {}", status.error().ToString());
			}
		}

		// Imports `source` on the calling (main) thread and publishes the result.
		Result<AssetImportOutcome> ImportNow(AssetHandle source, bool useCache, bool reload)
		{
			Result<ImportRequest> request = BuildRequest(source, useCache);
			const AssetReimportTicket ticket = BeginTicket(source, 0);
			if (!request.has_value())
			{
				const AssetRecord* record = Registry.Find(source);
				if (record == nullptr)
					return std::unexpected(std::move(request).error());
				RecordFailure(source, request.error(), record->SourcePath, {});
				return std::unexpected(std::move(request).error());
			}
			return ApplyImport(source, ticket, Utils::RunImport(*request), reload, false);
		}

		// Starts an import of `source` on a job; its publication runs at a later MainThreadQueue::Drain. Returns the job, which
		// resolves with the loaded artifact `artifact` (the main asset when null). Importers that need the main thread
		// import now instead (§4.11).
		JobHandle<AssetRef<Asset>> StartAsyncImport(AssetHandle source, bool useCache, AssetHandle artifact, bool reload)
		{
			const AssetHandle requested = artifact.IsValid() ? artifact : source;
			Result<ImportRequest> request = BuildRequest(source, useCache);
			if (!request.has_value() || request->Importer->RequiresMainThread())
			{
				Result<AssetRef<Asset>> value = [&]() -> Result<AssetRef<Asset>>
				{
					ENGINE_TRY(ImportNow(source, useCache, reload));
					const auto loaded = Loaded.find(requested);
					if (loaded == Loaded.end() || loaded->second.Value == nullptr)
						return MakeError(ErrorCode::NotFound, "the import of {} did not produce {}", source.ToString(), requested.ToString());
					return loaded->second.Value;
				}();
				return Specification.Jobs->Submit([value = std::move(value)]() -> Result<AssetRef<Asset>>
				{
					return value;
				});
			}

			const AssetReimportTicket ticket = BeginTicket(source, 0);
			++InFlight[source];
			++OutstandingTasks;
			Ref<const ImportRequest> shared = CreateRef<const ImportRequest>(std::move(*request));
			MainThreadQueue* queue = Specification.MainThread;
			std::weak_ptr<bool> alive = Alive;
			const uint64_t serial = ProjectSerial;
			State* state = this;
			return Specification.Jobs->Submit([shared, requested, ticket, queue, alive, serial, state, reload]() -> Result<AssetRef<Asset>>
			{
				ImportOutput output = Utils::RunImport(*shared);
				Result<AssetRef<Asset>> value = MakeError(ErrorCode::NotFound, "the import of {} did not produce {}", shared->Metadata.Handle.ToString(),
					requested.ToString());
				if (output.Failure.has_value())
				{
					value = std::unexpected(*output.Failure);
				}
				else
				{
					for (size_t index = 0; index < output.Import.Import.Artifacts.size(); ++index)
					{
						if (output.Import.Import.Artifacts[index].Handle == requested)
							value = output.Decoded[index];
					}
				}
				queue->Post([output = std::move(output), ticket, alive, serial, state, reload]() mutable
				{
					if (alive.expired())
						return;
					state->FinishAsyncImport(ticket, std::move(output), serial, reload);
				});
				return value;
			});
		}

		void FinishAsyncImport(const AssetReimportTicket& ticket, ImportOutput output, uint64_t serial, bool reload)
		{
			--OutstandingTasks;
			if (const auto inFlight = InFlight.find(ticket.Handle); inFlight != InFlight.end() && --inFlight->second == 0)
				InFlight.erase(inFlight);
			if (serial != ProjectSerial)
				return;
			static_cast<void>(ApplyImport(ticket.Handle, ticket, std::move(output), reload, reload));
		}

		// --- Built-ins --------------------------------------------------------------------------------------------------

		Result<AssetRef<Asset>> LoadBuiltin(const BuiltinAssetEntry& entry)
		{
			if (entry.Source == BuiltinAssetSource::Procedural)
				return Self->GetProceduralBuiltin(entry.Handle);
			if (const auto loaded = Loaded.find(entry.Handle); loaded != Loaded.end() && loaded->second.Value != nullptr)
				return loaded->second.Value;
			if (const auto failed = BuiltinFailures.find(entry.Handle); failed != BuiltinFailures.end())
				return std::unexpected(failed->second);

			const auto load = [this, &entry]() -> Result<AssetRef<Asset>>
			{
				if (!Specification.Vfs->IsMounted(EngineScheme) || !Specification.Vfs->IsMounted(EngineCacheScheme))
				{
					return std::unexpected(Error(ErrorCode::Unsupported, std::format("the built-in '{}' needs engine:// and enginecache://", entry.Path))
							.WithHint("start the editor from the repository (development builds mount Resources and bin/EngineCache)"));
				}
				ENGINE_TRY_ASSIGN(const std::vector<Buffer> artifacts, GetOrBakeEngineAsset({
																								.Vfs = Specification.Vfs,
																								.Importers = Specification.Importers,
																								.Registry = Specification.Registry,
																								.Jobs = Specification.Jobs,
																								.EnvironmentBaker = Specification.EnvironmentBaker,
																								.Generators = Specification.EngineAssetGenerators,
																							},
																		   entry));
				if (artifacts.empty())
					return MakeError(ErrorCode::ImportFailed, "the built-in '{}' produced no artifact", entry.Path);
				ENGINE_TRY_ASSIGN(AssetRef<Asset> asset, Specification.Loaders->Load(artifacts.front(), { .Registry = Specification.Registry, .Handle = entry.Handle }));
				Loaded[entry.Handle] = { .Value = asset, .ContentHash = XXH64(artifacts.front()) };
				return asset;
			};
			Result<AssetRef<Asset>> loaded = load();
			if (!loaded.has_value())
			{
				Error error = std::move(loaded).error().WithContext(std::format("while loading the built-in '{}'", entry.Path));
				BuiltinFailures.emplace(entry.Handle, error);
				return std::unexpected(std::move(error));
			}
			Self->BumpVersion(entry.Handle);
			return loaded;
		}

		// --- Scans, batches and external changes -----------------------------------------------------------------------

		[[nodiscard]] std::vector<AssetKnownLocation> ReadAssetLocations() const
		{
			Result<VfsPath> path = OpenProject->Specification.CacheRoot.Join(AssetLocationsFileName);
			if (!path.has_value() || !Specification.Vfs->Exists(*path))
				return {};
			Result<std::string> text = Specification.Vfs->ReadText(*path);
			Result<std::vector<AssetKnownLocation>> locations = text.has_value()
				? Utils::ParseAssetLocations(*text, GetAssetsRoot().GetScheme())
				: Result<std::vector<AssetKnownLocation>>(std::unexpected(text.error()));
			if (!locations.has_value())
			{
				ENGINE_CORE_WARN("Asset registry: '{}' is unreadable and is ignored: {}", path->ToString(), locations.error().ToString());
				return {};
			}
			return std::move(*locations);
		}

		void WriteAssetLocations()
		{
			if (DryRun.has_value() || !OpenProject.has_value())
				return;
			const std::string text = Utils::WriteAssetLocations(Registry.GetKnownLocations());
			Result<VfsPath> path = OpenProject->Specification.CacheRoot.Join(AssetLocationsFileName);
			if (!path.has_value())
				return;
			if (Result<std::string> existing = Specification.Vfs->ReadText(*path); existing.has_value() && *existing == text)
				return;
			Status written = OpenProject->Specification.CacheRoot.IsRoot() ? Status() : Specification.Vfs->CreateDirectories(OpenProject->Specification.CacheRoot);
			if (written.has_value())
				written = Specification.Vfs->WriteFileAtomic(*path, AsBytes(text));
			if (!written.has_value())
				ENGINE_CORE_WARN("Asset registry: '{}' cannot be written: {}", path->ToString(), written.error().ToString());
		}

		void AddTransient(const VfsPath& sourcePath, const AssetMetadata& metadata)
		{
			Result<VfsPath> metaPath = GetMetaPath(sourcePath);
			if (!metaPath.has_value())
				return;
			OpenProject->TransientMetas[sourcePath] = metadata;
			RegisterRecord({ .Metadata = metadata, .SourcePath = sourcePath, .MetaPath = std::move(*metaPath) });
		}

		// A read-only editor's transient metas are not in any file, so every scan registers them again.
		void ReregisterTransients()
		{
			for (auto iterator = OpenProject->TransientMetas.begin(); iterator != OpenProject->TransientMetas.end();)
			{
				Result<VfsPath> metaPath = GetMetaPath(iterator->first);
				if (!metaPath.has_value() || !Specification.Vfs->Exists(iterator->first) || Specification.Vfs->Exists(*metaPath)
					|| Registry.FindBySourcePath(iterator->first) != nullptr)
				{
					iterator = OpenProject->TransientMetas.erase(iterator);
					continue;
				}
				if (Status added = Registry.Add({ .Metadata = iterator->second, .SourcePath = iterator->first, .MetaPath = std::move(*metaPath) });
					!added.has_value())
				{
					ENGINE_CORE_WARN("Asset registry: the transient meta of '{}' cannot be registered: {}", iterator->first.ToString(), added.error().ToString());
				}
				++iterator;
			}
		}

		// Writes the metas of one batch (EditorAssetManager.h): the closures of the new and changed sources first, then a
		// dependency meta for every file without a meta inside a closure and the importer's meta for every other new source.
		// Returns the .meta paths written (or registered in memory, read-only).
		Result<std::vector<VfsPath>> WriteBatchMetas(std::span<const VfsPath> newSources, std::span<const AssetHandle> changedSources)
		{
			// Handles first, so that a dependency's owner may itself be new in this batch.
			std::map<VfsPath, AssetHandle> newHandles;
			for (const VfsPath& source : newSources)
			{
				newHandles.emplace(source, IsReadOnly() ? AssetHandle(Hash64(0, source.GetPath())) : Specification.IdGenerator->Next());
			}

			// The closures: each listed file is owned by the first source (in path order) whose closure names it.
			std::map<VfsPath, AssetHandle> owners;
			const auto addClosure = [this, &owners](const VfsPath& source, AssetHandle owner, const IAssetImporter& importer)
			{
				Result<Buffer> bytes = Specification.Vfs->ReadFile(source);
				if (!bytes.has_value())
					return;
				// A source whose references cannot be read gets its meta all the same; its import reports the problem.
				Result<std::vector<VfsPath>> files = importer.ListDependencyFiles(*bytes, source);
				if (!files.has_value())
					return;
				for (const VfsPath& file : *files)
				{
					if (file != source)
						owners.emplace(file, owner);
				}
			};
			std::vector<std::pair<VfsPath, AssetHandle>> closureSources;
			for (const auto& [source, handle] : newHandles)
				closureSources.emplace_back(source, handle);
			for (const AssetHandle changed : changedSources)
			{
				if (const AssetRecord* record = Registry.Find(changed); record != nullptr && record->Metadata.Kind == AssetMetaKind::Asset)
					closureSources.emplace_back(record->SourcePath, changed);
			}
			std::ranges::sort(closureSources);
			for (const auto& [source, handle] : closureSources)
			{
				const AssetRecord* record = Registry.Find(handle);
				const IAssetImporter* importer = record != nullptr ? Specification.Importers->FindById(record->Metadata.Importer)
																   : Specification.Importers->FindForExtension(source.GetExtension());
				if (importer != nullptr)
					addClosure(source, handle, *importer);
			}

			// The metas, in path order.
			std::map<VfsPath, AssetMetadata> metas;
			for (const auto& [source, handle] : newHandles)
			{
				if (const auto owner = owners.find(source); owner != owners.end() && owner->second != handle)
				{
					metas.emplace(source, MakeDependencyMetadata(handle, owner->second));
					continue;
				}
				const IAssetImporter* importer = Specification.Importers->FindForExtension(source.GetExtension());
				if (importer == nullptr)
					continue;
				AssetMetadata metadata;
				metadata.Handle = handle;
				metadata.Kind = AssetMetaKind::Asset;
				metadata.Type = importer->GetMainType();
				metadata.Importer = std::string(importer->GetId());
				metadata.ImporterVersion = importer->GetVersion();
				if (!importer->GetSettingsTypeName().empty())
				{
					ENGINE_TRY_ASSIGN(metadata.Settings, MergeSettings(*importer, VariantValue(), Json()));
				}
				metas.emplace(source, std::move(metadata));
			}
			for (const auto& [file, owner] : owners)
			{
				if (metas.contains(file) || !IsUnderAssets(file) || Registry.FindBySourcePath(file) != nullptr || !Specification.Vfs->Exists(file))
					continue;
				Result<VfsPath> metaPath = GetMetaPath(file);
				if (!metaPath.has_value() || Specification.Vfs->Exists(*metaPath))
					continue;
				metas.emplace(file, MakeDependencyMetadata(IsReadOnly() ? AssetHandle(Hash64(0, file.GetPath())) : Specification.IdGenerator->Next(), owner));
			}

			std::vector<VfsPath> created;
			for (const auto& [source, metadata] : metas)
			{
				ENGINE_TRY_ASSIGN(VfsPath metaPath, GetMetaPath(source));
				if (IsReadOnly())
				{
					AddTransient(source, metadata);
				}
				else
				{
					const std::string text = SerializeAssetMetadata(metadata);
					ENGINE_TRY(WithContext(Writer.Write(metaPath, AsBytes(text)), std::format("while writing the .meta of '{}'", source.ToString())));
				}
				created.push_back(std::move(metaPath));
			}
			std::ranges::sort(created);
			return created;
		}

		// Compares the Assets folder with what the manager last saw: the external changes since (Refresh's detection).
		Result<std::vector<FileChange>> DetectFileChanges()
		{
			ENGINE_TRY_ASSIGN(const std::vector<VfsEntry> entries, Specification.Vfs->List(GetAssetsRoot(), true));
			std::map<VfsPath, KnownFile>& known = OpenProject->KnownFiles;
			std::map<VfsPath, KnownFile> current;
			std::vector<FileChange> changes;
			for (const VfsEntry& entry : entries)
			{
				if (entry.Info.IsDirectory)
					continue;
				const auto previous = known.find(entry.Path);
				if (previous != known.end() && previous->second.Size == entry.Info.Size
					&& previous->second.ModificationTime == entry.Info.ModificationTime)
				{
					current.emplace(entry.Path, previous->second);
					continue;
				}
				Result<Buffer> bytes = Specification.Vfs->ReadFile(entry.Path);
				if (!bytes.has_value())
				{
					// Being written: examined again by the next refresh.
					if (previous != known.end())
						current.emplace(entry.Path, previous->second);
					continue;
				}
				const uint64_t hash = XXH64(*bytes);
				current.emplace(entry.Path, KnownFile{ .Size = entry.Info.Size, .ModificationTime = entry.Info.ModificationTime, .Hash = hash });
				if (previous == known.end())
					changes.push_back({ .Path = entry.Path, .Kind = FileChangeKind::Created });
				else if (!previous->second.Hash.has_value() || *previous->second.Hash != hash)
					changes.push_back({ .Path = entry.Path, .Kind = FileChangeKind::Modified });
			}
			for (const auto& entry : known)
			{
				if (!current.contains(entry.first))
					changes.push_back({ .Path = entry.first, .Kind = FileChangeKind::Deleted });
			}
			known = std::move(current);
			std::ranges::sort(changes, std::less<>(), &FileChange::Path);
			return changes;
		}

		// The external changes `changes` (from a poll, or detected by Refresh): rescan, write the batch's metas, forget what
		// went, find the imports whose inputs changed, notify the listener and reimport them (synchronously for Refresh, on
		// jobs for a poll) or hold them while reloads are deferred.
		// Adds a synchronous import's failure (ASSET_IMPORT_FAILED) or warnings to a refresh report; a superseded import
		// (Cancelled) adds nothing.
		void AppendImportOutcome(AssetRefreshReport& report, AssetHandle source, const Result<AssetImportOutcome>& outcome) const
		{
			if (!outcome.has_value() && outcome.error().GetCode() != ErrorCode::Cancelled)
			{
				const AssetRecord* record = Registry.Find(source);
				report.Diagnostics.push_back({
					.Severity = DiagnosticSeverity::Error,
					.Code = std::string(AssetImportFailedCode),
					.Asset = source,
					.Path = record != nullptr ? Utils::RelativeText(record->SourcePath) : std::string(),
					.Message = Error(outcome.error()).WithHint(std::string()).ToString(), // the hint is the diagnostic's own member
					.Hint = outcome.error().GetHint(),
					.Subject = {},
					.AutoFixable = false,
				});
			}
			else if (outcome.has_value())
			{
				report.Diagnostics.insert(report.Diagnostics.end(), outcome->Diagnostics.begin(), outcome->Diagnostics.end());
			}
		}

		Result<AssetRefreshReport> ProcessChanges(std::span<const FileChange> changes, bool synchronous)
		{
			const std::vector<AssetHandle> before = Registry.GetHandles();
			std::map<VfsPath, AssetHandle> previousOwners; // path -> main asset, for Deleted changes
			for (const AssetRecord* record : Registry.GetRecords())
			{
				previousOwners.emplace(record->SourcePath,
					record->Metadata.Kind == AssetMetaKind::Asset ? record->Metadata.Handle : record->Metadata.Owner);
			}

			ENGINE_TRY_ASSIGN(AssetScanResult scan, Registry.Scan(*Specification.Vfs, GetAssetsRoot(), Specification.Importers->Describe()));
			if (IsReadOnly())
				ReregisterTransients();

			std::vector<AssetHandle> changedSources;
			for (const FileChange& change : changes)
			{
				if (change.Kind == FileChangeKind::Deleted)
					continue;
				if (const AssetRecord* record = Registry.FindBySourcePath(change.Path); record != nullptr && record->Metadata.Kind == AssetMetaKind::Asset)
					changedSources.push_back(record->Metadata.Handle);
			}
			std::vector<VfsPath> newSources = scan.SourcesWithoutMeta;
			if (IsReadOnly())
			{
				std::erase_if(newSources, [this](const VfsPath& source)
				{
					return Registry.FindBySourcePath(source) != nullptr;
				});
			}
			ENGINE_TRY_ASSIGN(std::vector<VfsPath> createdMetas, WriteBatchMetas(newSources, changedSources));

			AssetRefreshReport report;
			report.MetaCount = scan.MetaCount;
			report.CreatedMetas = std::move(createdMetas);
			const std::vector<AssetHandle> after = Registry.GetHandles();
			std::ranges::set_difference(after, before, std::back_inserter(report.Added));
			std::ranges::set_difference(before, after, std::back_inserter(report.Removed));

			// What went: main assets whose records are gone (a sub-asset that went with its source's .meta is covered too).
			for (const AssetHandle removed : report.Removed)
			{
				if (const auto import = Imports.find(removed); import != Imports.end())
				{
					for (const AssetHandle artifact : import->second.Artifacts)
						Loaded.erase(artifact);
					Imports.erase(import);
				}
				Loaded.erase(removed);
				Graph.RemoveAsset(removed);
				Self->ClearDiagnostics(removed, GetAssetDiagnosticCodes());
			}
			for (const AssetHandle added : report.Added)
				Self->ClearDiagnostics(added, std::array<std::string_view, 1>{ AssetMissingCode });

			// The imports whose inputs changed: their source, a file they read, their .meta's importer or settings, or what a
			// path lookup finds. A failed import is retried when a file next to its source changes too (a dependency file that
			// was missing has no recorded read).
			const std::vector<ImportAssetLookupEntry> snapshot = Utils::MakeLookupSnapshot(Registry);
			std::set<AssetHandle> stale;
			for (auto& [source, import] : Imports)
			{
				const AssetRecord* record = Registry.Find(source);
				if (record == nullptr || record->Metadata.Kind != AssetMetaKind::Asset)
					continue;
				bool isStale = record->Metadata.Importer != import.Importer || record->Metadata.Settings != import.Settings;
				for (const FileChange& change : changes)
				{
					const bool isRead = std::ranges::find(import.Reads, change.Path, &ImportDependencyRead::Path) != import.Reads.end();
					const bool nearFailure = import.Failure.has_value() && change.Path.IsUnder(record->SourcePath.GetParent());
					if (change.Path == record->SourcePath || isRead || nearFailure)
						isStale = true;
				}
				for (const ImportAssetLookup& lookup : import.Lookups)
				{
					const ImportAssetLookupEntry* entry = Utils::FindLookupEntry(snapshot, lookup.Path);
					const bool same = entry == nullptr ? !lookup.Found.has_value() : lookup.Found.has_value() && *lookup.Found == *entry;
					if (!same)
						isStale = true;
				}
				if (isStale)
				{
					import.Failure.reset();
					stale.insert(source);
				}
			}

			Self->SetScanDiagnostics(scan.Diagnostics);
			report.Diagnostics = scan.Diagnostics;
			WriteAssetLocations();

			// The listener hears of each change of a source file once.
			std::vector<AssetExternalChange> external;
			for (const FileChange& change : changes)
			{
				if (Utils::IsMetaFile(change.Path) || !IsUnderAssets(change.Path))
					continue;
				AssetExternalChange notice{ .Path = change.Path, .Handle = {}, .Type = AssetType::None, .Kind = change.Kind };
				if (change.Kind == FileChangeKind::Deleted)
				{
					if (const auto owner = previousOwners.find(change.Path); owner != previousOwners.end())
						notice.Handle = owner->second;
				}
				else if (change.Kind == FileChangeKind::Modified)
				{
					if (const AssetRecord* record = Registry.FindBySourcePath(change.Path))
						notice.Handle = record->Metadata.Kind == AssetMetaKind::Asset ? record->Metadata.Handle : record->Metadata.Owner;
				}
				if (notice.Handle.IsValid())
					notice.Type = Self->GetAssetType(notice.Handle);
				external.push_back(std::move(notice));
			}

			// Main assets this batch registered that were never imported (new sources, and sources whose .meta arrived) are
			// imported now too, so their .meta lists their sub-assets (a glTF's meshes, which asset.list reports) and their
			// import problems are reported by this refresh instead of at their first use.
			std::vector<AssetHandle> fresh;
			for (const AssetHandle added : report.Added)
			{
				const AssetRecord* record = Registry.Find(added);
				if (record != nullptr && record->Metadata.Kind == AssetMetaKind::Asset && !Imports.contains(added) && FindBuiltin(added) == nullptr)
					fresh.push_back(added);
			}

			if (Deferred)
			{
				HeldReimports.insert(stale.begin(), stale.end());
				HeldReimports.insert(fresh.begin(), fresh.end());
				HeldChanges.insert(HeldChanges.end(), external.begin(), external.end());
				report.Deferred.assign(stale.begin(), stale.end());
				return report;
			}
			for (const AssetExternalChange& notice : external)
				NotifyChange(notice);

			for (const AssetHandle source : fresh)
			{
				if (Registry.Find(source) == nullptr || Imports.contains(source))
					continue;
				if (synchronous)
				{
					Result<AssetImportOutcome> outcome = ImportNow(source, true, false);
					AppendImportOutcome(report, source, outcome);
				}
				else
				{
					static_cast<void>(StartAsyncImport(source, true, AssetHandle(), false));
				}
			}

			const std::vector<AssetHandle> staleList(stale.begin(), stale.end());
			for (const AssetHandle source : Graph.GetReimportOrder(staleList))
			{
				if (!Imports.contains(source) || !Registry.Find(source))
					continue;
				report.Changed.push_back(source);
				if (synchronous)
				{
					Result<AssetImportOutcome> outcome = ImportNow(source, true, true);
					AppendImportOutcome(report, source, outcome);
				}
				else
				{
					static_cast<void>(StartAsyncImport(source, true, AssetHandle(), true));
				}
			}
			std::ranges::sort(report.Changed);
			return report;
		}

		// True when KnownFiles already holds the state `change` reports for its file: Refresh, an editor write or an earlier
		// poll processed it. A file whose known state has no hash (seen only by OpenProject's listing) is never taken as known.
		[[nodiscard]] bool IsKnownState(const FileChange& change, const std::optional<uint64_t>& hash) const
		{
			const auto known = OpenProject->KnownFiles.find(change.Path);
			if (change.Kind == FileChangeKind::Deleted)
				return known == OpenProject->KnownFiles.end() && !Specification.Vfs->Exists(change.Path);
			if (known == OpenProject->KnownFiles.end() || !known->second.Hash.has_value() || !hash.has_value() || *known->second.Hash != *hash)
				return false;
			const Result<FileInfo> info = Specification.Vfs->GetInfo(change.Path);
			return info.has_value() && !info->IsDirectory && info->Size == known->second.Size && info->ModificationTime == known->second.ModificationTime;
		}

		// A poll's changes (the hot reloader's listener, main thread). A poll computes its changes on a job and delivers them
		// at a later frame, and Refresh (which marks what it detects known on the watcher, too late for a poll already
		// computed) may have processed the same change in between, as may a deferral's end. Each change is published once
		// (§7.5 race rule 3, decision 17): a change whose state KnownFiles already holds is dropped here.
		void OnPolledChanges(std::span<const FileChange> changes)
		{
			if (!OpenProject.has_value())
				return;
			std::vector<FileChange> fresh;
			fresh.reserve(changes.size());
			for (const FileChange& change : changes)
			{
				std::optional<uint64_t> hash;
				if (change.Kind != FileChangeKind::Deleted)
				{
					if (Result<Buffer> bytes = Specification.Vfs->ReadFile(change.Path); bytes.has_value())
						hash = XXH64(*bytes);
				}
				if (IsKnownState(change, hash))
					continue;
				if (change.Kind == FileChangeKind::Deleted)
					OpenProject->KnownFiles.erase(change.Path);
				else
					UpdateKnownFile(change.Path, hash);
				fresh.push_back(change);
			}
			if (fresh.empty())
				return;
			if (Result<AssetRefreshReport> processed = ProcessChanges(fresh, DryRun.has_value()); !processed.has_value())
				ENGINE_CORE_WARN("Hot reload: {}", processed.error().ToString());
		}

		// Keeps a copy of everything a dry run may change: the registry, the graph, the loaded artifacts, the imports and the file states.
		void SaveDryRun()
		{
			DryRunSnapshot snapshot{
				.Registry = Registry,
				.Graph = Graph,
				.Loaded = Loaded,
				.Imports = Imports,
				.KnownFiles = OpenProject.has_value() ? OpenProject->KnownFiles : std::map<VfsPath, KnownFile>(),
				.TransientMetas = OpenProject.has_value() ? OpenProject->TransientMetas : std::map<VfsPath, AssetMetadata>(),
			};
			DryRun = std::move(snapshot);
		}

		void RestoreDryRun()
		{
			DryRunSnapshot& snapshot = *DryRun;
			Registry = std::move(snapshot.Registry);
			Graph = std::move(snapshot.Graph);
			Loaded = std::move(snapshot.Loaded);
			Imports = std::move(snapshot.Imports);
			if (OpenProject.has_value())
			{
				OpenProject->KnownFiles = std::move(snapshot.KnownFiles);
				OpenProject->TransientMetas = std::move(snapshot.TransientMetas);
			}
			DryRun.reset();
		}
	};

	EditorAssetManager::EditorAssetManager(const EditorAssetManagerSpecification& specification)
	{
		ENGINE_ASSERT(specification.Vfs != nullptr && specification.Jobs != nullptr && specification.MainThread != nullptr
				&& specification.Events != nullptr && specification.Registry != nullptr && specification.IdGenerator != nullptr
				&& specification.Importers != nullptr && specification.Loaders != nullptr,
			"EditorAssetManager needs every service of its specification");
		m_State = CreateScope<State>(*this, specification);

		bool loaded = false;
		if (specification.Vfs->IsMounted(EngineScheme))
		{
			Result<BuiltinAssetCatalog> catalogue = BuiltinAssetCatalog::Load(*specification.Vfs);
			if (catalogue.has_value())
			{
				m_State->Builtins = std::move(*catalogue);
				loaded = true;
			}
			else
			{
				ENGINE_CORE_ERROR("Built-in assets: engine://{} cannot be read; only the procedural built-ins are available: {}", BuiltinAssetCatalog::FileName,
					catalogue.error().ToString());
			}
		}
		if (!loaded)
		{
			Result<BuiltinAssetCatalog> procedural = BuiltinAssetCatalog::Parse(Utils::WriteProceduralCatalogue());
			ENGINE_CORE_ASSERT(procedural.has_value(), "The procedural built-in catalogue does not parse: {}",
				procedural.has_value() ? std::string() : procedural.error().ToString());
			if (procedural.has_value())
				m_State->Builtins = std::move(*procedural);
		}

		State* state = m_State.get();
		m_State->Writer.SetListener([state](const AssetWriteEvent& event)
		{
			state->OnWrite(event);
		});
	}

	EditorAssetManager::~EditorAssetManager()
	{
		if (m_State->DryRun.has_value())
			EndDryRun();
		CloseProject();
		m_State->Writer.SetListener({});
		m_State->Alive.reset();
	}

	Result<AssetRef<Asset>> EditorAssetManager::Load(AssetHandle handle)
	{
		if (!handle.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "the null handle names no asset");
		if (const BuiltinAssetEntry* builtin = m_State->FindBuiltin(handle))
			return m_State->LoadBuiltin(*builtin);

		const std::optional<AssetRegistry::Location> location = m_State->Registry.Locate(handle);
		if (!location.has_value())
		{
			if (const AssetRecord* record = m_State->Registry.Find(handle))
			{
				return MakeError(ErrorCode::InvalidArgument, "{} ('{}') is a dependency file of {}, which is never loaded on its own", handle.ToString(),
					Utils::RelativeText(record->SourcePath), record->Metadata.Owner.ToString());
			}
			return MakeError(ErrorCode::NotFound, "no asset has handle {}", handle.ToString());
		}

		if (const auto loaded = m_State->Loaded.find(handle); loaded != m_State->Loaded.end() && loaded->second.Value != nullptr)
			return loaded->second.Value;

		const AssetHandle source = location->Record->Metadata.Handle;
		const std::string path = GetReferencePath(handle);
		const auto describe = [&handle, &path](Error error)
		{
			return std::unexpected(std::move(error).WithContext(std::format("while loading asset {} ('{}')", handle.ToString(), path)));
		};
		if (const auto import = m_State->Imports.find(source); import != m_State->Imports.end() && import->second.Failure.has_value())
			return describe(*import->second.Failure);

		Result<AssetImportOutcome> imported = m_State->ImportNow(source, true, false);
		if (!imported.has_value())
			return describe(std::move(imported).error());
		const auto loaded = m_State->Loaded.find(handle);
		if (loaded == m_State->Loaded.end() || loaded->second.Value == nullptr)
			return describe(Error(ErrorCode::NotFound, std::format("the import of '{}' no longer produces this asset", path)));
		return loaded->second.Value;
	}

	JobHandle<AssetRef<Asset>> EditorAssetManager::LoadAsync(AssetHandle handle)
	{
		const auto completed = [this](Result<AssetRef<Asset>> value)
		{
			return m_State->Specification.Jobs->Submit([value = std::move(value)]() -> Result<AssetRef<Asset>>
			{
				return value;
			});
		};
		const std::optional<AssetRegistry::Location> location = handle.IsValid() ? m_State->Registry.Locate(handle) : std::nullopt;
		const auto found = m_State->Loaded.find(handle);
		const bool loaded = found != m_State->Loaded.end() && found->second.Value != nullptr;
		if (!location.has_value() || loaded)
			return completed(Load(handle));
		const AssetHandle source = location->Record->Metadata.Handle;
		if (const auto import = m_State->Imports.find(source); import != m_State->Imports.end() && import->second.Failure.has_value())
			return completed(Load(handle));
		if (m_State->DryRun.has_value())
			return completed(Load(handle));
		return m_State->StartAsyncImport(source, true, handle, false);
	}

	AssetState EditorAssetManager::GetState(AssetHandle handle) const
	{
		if (const BuiltinAssetEntry* builtin = m_State->FindBuiltin(handle))
		{
			if (builtin->Source == BuiltinAssetSource::Procedural)
				return GetVersion(handle) > 0 ? AssetState::Loaded : AssetState::Unloaded;
			if (m_State->Loaded.contains(handle))
				return AssetState::Loaded;
			return m_State->BuiltinFailures.contains(handle) ? AssetState::Failed : AssetState::Unloaded;
		}
		const std::optional<AssetRegistry::Location> location = m_State->Registry.Locate(handle);
		if (!location.has_value())
			return AssetState::Unloaded;
		if (const auto loaded = m_State->Loaded.find(handle); loaded != m_State->Loaded.end() && loaded->second.Value != nullptr)
			return AssetState::Loaded;
		const AssetHandle source = location->Record->Metadata.Handle;
		if (m_State->InFlight.contains(source))
			return AssetState::Loading;
		if (const auto import = m_State->Imports.find(source); import != m_State->Imports.end() && import->second.Failure.has_value())
			return AssetState::Failed;
		return AssetState::Unloaded;
	}

	const AssetMetadata* EditorAssetManager::GetMetadata(AssetHandle handle) const
	{
		if (m_State->FindBuiltin(handle) != nullptr)
			return nullptr;
		if (const std::optional<AssetRegistry::Location> location = m_State->Registry.Locate(handle))
			return &location->Record->Metadata;
		const AssetRecord* record = m_State->Registry.Find(handle);
		return record != nullptr ? &record->Metadata : nullptr;
	}

	AssetType EditorAssetManager::GetAssetType(AssetHandle handle) const
	{
		if (const BuiltinAssetEntry* builtin = m_State->FindBuiltin(handle))
			return builtin->Type;
		const std::optional<AssetRegistry::Location> location = m_State->Registry.Locate(handle);
		return location.has_value() ? location->Type : AssetType::None;
	}

	std::optional<AssetHandle> EditorAssetManager::Resolve(std::string_view reference) const
	{
		const Result<AssetReference> parsed = ParseAssetReference(reference);
		if (!parsed.has_value())
			return std::nullopt;
		switch (parsed->Kind)
		{
			case AssetReferenceKind::Handle:
				if (m_State->FindBuiltin(parsed->Handle) != nullptr || m_State->Registry.Locate(parsed->Handle).has_value())
					return parsed->Handle;
				return std::nullopt;
			case AssetReferenceKind::EnginePath:
			{
				if (!parsed->SubAssetKey.empty())
					return std::nullopt;
				const BuiltinAssetEntry* entry = m_State->FindBuiltinByPath(parsed->Path.ToString());
				return entry != nullptr ? std::optional<AssetHandle>(entry->Handle) : std::nullopt;
			}
			case AssetReferenceKind::ProjectPath:
				return m_State->Registry.Resolve(reference);
		}
		return std::nullopt;
	}

	std::string EditorAssetManager::GetReferencePath(AssetHandle handle) const
	{
		if (const BuiltinAssetEntry* builtin = m_State->FindBuiltin(handle))
			return builtin->Path;
		return m_State->Registry.GetReferencePath(handle);
	}

	void EditorAssetManager::WaitIdle()
	{
		ENGINE_CORE_ASSERT(m_State->Specification.MainThread->IsMainThread(), "EditorAssetManager::WaitIdle runs on the main thread");
		do
		{
			m_State->Specification.Jobs->WaitIdle();
			if (m_State->OutstandingTasks > 0)
				static_cast<void>(m_State->Specification.MainThread->Drain());
		} while (m_State->OutstandingTasks > 0);
	}

	Result<AssetRefreshReport> EditorAssetManager::OpenProject(const AssetProjectSpecification& project)
	{
		if (m_State->OpenProject.has_value())
			return MakeError(ErrorCode::InvalidState, "a project is open; close it first");
		ENGINE_CORE_ASSERT(!m_State->DryRun.has_value(), "OpenProject during a dry run");

		VirtualFileSystem& vfs = *m_State->Specification.Vfs;
		m_State->OpenProject.emplace();
		m_State->OpenProject->Specification = project;
		m_State->OpenProject->Cache = CreateRef<AssetCache>(vfs, project.CacheRoot);
		++m_State->ProjectSerial;
		const auto closeOnError = [this](Error error)
		{
			CloseProject();
			return std::unexpected(std::move(error));
		};

		// What is on disk now: Refresh compares against it.
		Result<std::vector<VfsEntry>> listing = vfs.List(project.AssetsRoot, true);
		if (!listing.has_value())
			return closeOnError(std::move(listing).error());
		for (const VfsEntry& entry : *listing)
		{
			if (!entry.Info.IsDirectory)
				m_State->OpenProject->KnownFiles.emplace(entry.Path, KnownFile{ .Size = entry.Info.Size, .ModificationTime = entry.Info.ModificationTime, .Hash = {} });
		}

		const std::vector<AssetKnownLocation> known = m_State->ReadAssetLocations();
		Result<AssetScanResult> scan = m_State->Registry.Scan(vfs, project.AssetsRoot, m_State->Specification.Importers->Describe(), known);
		if (!scan.has_value())
			return closeOnError(std::move(scan).error());
		Result<std::vector<VfsPath>> created = m_State->WriteBatchMetas(scan->SourcesWithoutMeta, {});
		if (!created.has_value())
			return closeOnError(std::move(created).error());

		SetScanDiagnostics(scan->Diagnostics);
		m_State->WriteAssetLocations();

		if (project.HotReload && !project.ReadOnly)
		{
			Scope<AssetHotReloader> reloader = CreateScope<AssetHotReloader>(vfs, *m_State->Specification.Jobs,
				AssetHotReloaderSpecification{ .Root = project.AssetsRoot, .PollIntervalSeconds = 0.5, .DebounceSeconds = 0.2 });
			if (Status started = reloader->Start(); started.has_value())
			{
				State* state = m_State.get();
				reloader->SetChangeListener([state](std::span<const FileChange> changes)
				{
					state->OnPolledChanges(changes);
				});
				m_State->Writer.SetWatcher(&reloader->GetWatcher());
				reloader->SetDeferred(m_State->Deferred);
				m_State->OpenProject->HotReloader = std::move(reloader);
			}
			else
			{
				ENGINE_CORE_WARN("Hot reload is off for this project: {}", started.error().ToString());
			}
		}

		AssetRefreshReport report;
		report.MetaCount = scan->MetaCount;
		report.CreatedMetas = std::move(*created);
		report.Added = m_State->Registry.GetHandles();
		report.Diagnostics = scan->Diagnostics;
		return report;
	}

	void EditorAssetManager::CloseProject()
	{
		if (!m_State->OpenProject.has_value())
			return;
		ENGINE_CORE_ASSERT(!m_State->DryRun.has_value(), "CloseProject during a dry run");
		if (m_State->OpenProject->HotReloader != nullptr)
		{
			m_State->Writer.SetWatcher(nullptr);
			m_State->OpenProject->HotReloader->Stop();
		}
		// The project's jobs finish before its state goes; their publications see the new serial and are dropped.
		m_State->Specification.Jobs->WaitIdle();
		++m_State->ProjectSerial;

		std::set<AssetHandle> handles;
		for (const AssetHandle handle : m_State->Registry.GetHandles())
			handles.insert(handle);
		for (const auto& entry : m_State->Imports)
		{
			handles.insert(entry.first);
			handles.insert(entry.second.Artifacts.begin(), entry.second.Artifacts.end());
		}
		for (const AssetHandle handle : handles)
		{
			m_State->Loaded.erase(handle);
			ClearDiagnostics(handle, GetAssetDiagnosticCodes());
		}
		SetScanDiagnostics({});
		m_State->Registry = AssetRegistry();
		m_State->Graph.Clear();
		m_State->Imports.clear();
		m_State->InFlight.clear();
		m_State->HeldReimports.clear();
		m_State->HeldChanges.clear();
		m_State->OpenProject.reset();
	}

	bool EditorAssetManager::HasProject() const
	{
		return m_State->OpenProject.has_value();
	}

	const AssetRegistry& EditorAssetManager::GetRegistry() const
	{
		return m_State->Registry;
	}

	const AssetDependencyGraph& EditorAssetManager::GetDependencyGraph() const
	{
		return m_State->Graph;
	}

	const BuiltinAssetCatalog& EditorAssetManager::GetBuiltins() const
	{
		return m_State->Builtins;
	}

	Result<AssetRefreshReport> EditorAssetManager::Refresh()
	{
		if (!m_State->OpenProject.has_value())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		// Earlier work first, so the comparison below sees every publication.
		if (!m_State->DryRun.has_value())
			WaitIdle();
		ENGINE_TRY_ASSIGN(const std::vector<FileChange> changes, m_State->DetectFileChanges());
		if (AssetHotReloader* reloader = m_State->OpenProject->HotReloader.get())
		{
			// The watcher never reports these again: each change is published once, by whichever observer saw it first.
			for (const FileChange& change : changes)
			{
				if (Status marked = reloader->GetWatcher().MarkKnown(change.Path); !marked.has_value())
					ENGINE_CORE_WARN("Hot reload: {}", marked.error().ToString());
			}
		}
		return m_State->ProcessChanges(changes, true);
	}

	Result<AssetImportOutcome> EditorAssetManager::Reimport(AssetHandle handle)
	{
		if (!m_State->OpenProject.has_value())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		if (m_State->FindBuiltin(handle) != nullptr)
			return MakeError(ErrorCode::InvalidArgument, "{} is a built-in asset, which has no source to reimport", handle.ToString());
		const AssetRecord* record = m_State->Registry.Find(handle);
		if (record == nullptr)
		{
			if (m_State->Registry.Locate(handle).has_value())
				return MakeError(ErrorCode::InvalidArgument, "{} is a sub-asset; reimport its source", handle.ToString());
			return MakeError(ErrorCode::NotFound, "no asset has handle {}", handle.ToString());
		}
		if (record->Metadata.Kind != AssetMetaKind::Asset)
			return MakeError(ErrorCode::InvalidArgument, "{} is a dependency file; reimport its owner {}", handle.ToString(), record->Metadata.Owner.ToString());
		const bool reload = m_State->Imports.contains(handle);
		return m_State->ImportNow(handle, false, reload);
	}

	JobHandle<AssetImportOutcome> EditorAssetManager::ReimportAsync(AssetHandle handle)
	{
		JobSystem& jobs = *m_State->Specification.Jobs;
		const AssetRecord* record = m_State->Registry.Find(handle);
		const bool importable = m_State->OpenProject.has_value() && m_State->FindBuiltin(handle) == nullptr && record != nullptr
			&& record->Metadata.Kind == AssetMetaKind::Asset;
		if (!importable || m_State->DryRun.has_value())
		{
			Result<AssetImportOutcome> outcome = Reimport(handle);
			return jobs.Submit([outcome = std::move(outcome)]() -> Result<AssetImportOutcome>
			{
				return outcome;
			});
		}
		Result<ImportRequest> request = m_State->BuildRequest(handle, false);
		if (!request.has_value() || request->Importer->RequiresMainThread())
		{
			Result<AssetImportOutcome> outcome = Reimport(handle);
			return jobs.Submit([outcome = std::move(outcome)]() -> Result<AssetImportOutcome>
			{
				return outcome;
			});
		}

		State* state = m_State.get();
		const bool reload = state->Imports.contains(handle);
		const AssetReimportTicket ticket = state->BeginTicket(handle, 0);
		++state->InFlight[handle];
		++state->OutstandingTasks;
		Ref<const ImportRequest> shared = CreateRef<const ImportRequest>(std::move(*request));
		MainThreadQueue* queue = state->Specification.MainThread;
		std::weak_ptr<bool> alive = state->Alive;
		const uint64_t serial = state->ProjectSerial;
		return jobs.Submit([shared, ticket, queue, alive, serial, state, reload]() -> Result<AssetImportOutcome>
		{
			ImportOutput output = Utils::RunImport(*shared);
			Result<AssetImportOutcome> outcome = output.Failure.has_value() ? Result<AssetImportOutcome>(std::unexpected(*output.Failure))
																			: Result<AssetImportOutcome>(AssetImportOutcome{
																				  .Handle = shared->Metadata.Handle,
																				  .SubAssets = Utils::MakeSubAssetEntries(output.Import.Import),
																				  .Diagnostics = output.Import.Import.Diagnostics,
																				  .FromCache = output.FromCache,
																			  });
			queue->Post([output = std::move(output), ticket, alive, serial, state, reload]() mutable
			{
				if (alive.expired())
					return;
				state->FinishAsyncImport(ticket, std::move(output), serial, reload);
			});
			return outcome;
		});
	}

	Result<AssetMetadata> EditorAssetManager::CreateMetadata(const VfsPath& source) const
	{
		const IAssetImporter* importer = m_State->Specification.Importers->FindForExtension(source.GetExtension());
		if (importer == nullptr)
		{
			std::string extensions;
			for (const IAssetImporter* candidate : m_State->Specification.Importers->GetImporters())
			{
				for (const std::string_view extension : candidate->GetExtensions())
					extensions += extensions.empty() ? std::string(extension) : std::format(", {}", extension);
			}
			return std::unexpected(Error(ErrorCode::NotFound, std::format("no importer takes '{}' files ('{}')", source.GetExtension(), source.ToString()))
					.WithHint(std::format("importable extensions: {}", extensions)));
		}
		AssetMetadata metadata;
		metadata.Handle = m_State->Specification.IdGenerator->Next();
		metadata.Kind = AssetMetaKind::Asset;
		metadata.Type = importer->GetMainType();
		metadata.Importer = std::string(importer->GetId());
		metadata.ImporterVersion = importer->GetVersion();
		if (!importer->GetSettingsTypeName().empty())
		{
			ENGINE_TRY_ASSIGN(metadata.Settings, m_State->MergeSettings(*importer, VariantValue(), Json()));
		}
		return metadata;
	}

	AssetMetadata EditorAssetManager::CreateDependencyMetadata(AssetHandle owner) const
	{
		return MakeDependencyMetadata(m_State->Specification.IdGenerator->Next(), owner);
	}

	std::vector<AssetHandle> EditorAssetManager::GetPathDependents(AssetHandle handle) const
	{
		std::vector<AssetHandle> dependents;
		for (const auto& [source, import] : m_State->Imports)
		{
			if (std::ranges::binary_search(import.LookedUp, handle))
				dependents.push_back(source);
		}
		return dependents;
	}

	Result<VariantValue> EditorAssetManager::MergeImportSettings(std::string_view importerId, const VariantValue& base, const Json& patch) const
	{
		const IAssetImporter* importer = m_State->Specification.Importers->FindById(importerId);
		if (importer == nullptr)
			return MakeError(ErrorCode::NotFound, "no importer has id '{}'", importerId);
		return m_State->MergeSettings(*importer, base, patch);
	}

	AssetWriter& EditorAssetManager::GetWriter()
	{
		return m_State->Writer;
	}

	void EditorAssetManager::SetWriteObserver(WriteObserver observer)
	{
		m_State->Observer = std::move(observer);
	}

	void EditorAssetManager::Update(double nowSeconds)
	{
		if (!m_State->OpenProject.has_value() || m_State->OpenProject->HotReloader == nullptr)
			return;
		m_State->OpenProject->HotReloader->Update(nowSeconds);
	}

	void EditorAssetManager::SetReloadsDeferred(bool deferred)
	{
		if (m_State->Deferred == deferred)
			return;
		m_State->Deferred = deferred;
		// Ending a deferral, the reloader delivers its held changes first; OnPolledChanges drops each one a Refresh during
		// the deferral already detected (KnownFiles holds its state), so a change both observers held is published once,
		// with Refresh's held notices below.
		AssetHotReloader* reloader = m_State->OpenProject.has_value() ? m_State->OpenProject->HotReloader.get() : nullptr;
		if (reloader != nullptr)
			reloader->SetDeferred(deferred);
		if (deferred)
			return;

		// Race rule 4 ends: what Refresh held is published now, like the hot reloader's held changes.
		std::vector<AssetExternalChange> changes = std::move(m_State->HeldChanges);
		m_State->HeldChanges.clear();
		const std::vector<AssetHandle> held(m_State->HeldReimports.begin(), m_State->HeldReimports.end());
		m_State->HeldReimports.clear();
		for (const AssetExternalChange& change : changes)
			m_State->NotifyChange(change);
		for (const AssetHandle source : m_State->Graph.GetReimportOrder(held))
		{
			if (m_State->Registry.Find(source) == nullptr)
				continue;
			if (m_State->Imports.contains(source))
				m_State->InvalidateSource(source, true);
			else if (m_State->DryRun.has_value())
				static_cast<void>(m_State->ImportNow(source, true, false)); // a source registered while held, never imported
			else
				static_cast<void>(m_State->StartAsyncImport(source, true, AssetHandle(), false));
		}
	}

	bool EditorAssetManager::AreReloadsDeferred() const
	{
		return m_State->Deferred;
	}

	void EditorAssetManager::SetExternalChangeListener(ExternalChangeListener listener)
	{
		m_State->ChangeListener = std::move(listener);
	}

	void EditorAssetManager::SetReloadListener(ReloadListener listener)
	{
		m_State->OnReload = std::move(listener);
	}

	AssetHotReloader* EditorAssetManager::GetHotReloader()
	{
		return m_State->OpenProject.has_value() ? m_State->OpenProject->HotReloader.get() : nullptr;
	}

	void EditorAssetManager::BeginDryRun()
	{
		ENGINE_CORE_ASSERT(!m_State->DryRun.has_value(), "BeginDryRun while a dry run is open");
		WaitIdle();
		m_State->SaveDryRun();
		SaveSharedState();
		m_State->Writer.SetDryRun(true);
	}

	void EditorAssetManager::EndDryRun()
	{
		ENGINE_CORE_ASSERT(m_State->DryRun.has_value(), "EndDryRun without an open dry run");
		if (!m_State->DryRun.has_value())
			return;
		m_State->Writer.SetDryRun(false);
		m_State->RestoreDryRun();
		RestoreSharedState();
	}

	bool EditorAssetManager::IsDryRun() const
	{
		return m_State->DryRun.has_value();
	}

}
