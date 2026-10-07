#pragma once

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The importer interface (Architecture §7.4), frozen by the M6 contract. Importers live in AssetPipeline (editor-only, §3:
// the Runtime never registers them, so the linker drops them) and are registered in the ImporterRegistry.
//
// Importers are pure functions of (source bytes, settings, importer version) plus the files and asset lookups they make
// through the ImportContext, which records both, so the cache can validate an entry against exactly what the import read
// (AssetCache.h). No timestamps, no pointers, no unordered iteration in output: "Importers: importing twice gives identical
// hashes".

namespace Engine {

	class IEnvironmentBaker;
	class IScriptDiagnosticsProvider;
	class TypeRegistry;
	class VirtualFileSystem;

	// One registered asset as an import may see it: the snapshot the EditorAssetManager hands to each import job (the
	// registry itself is main-thread only).
	struct ImportAssetLookupEntry
	{
		VfsPath SourcePath{};
		AssetHandle Handle{};
		AssetMetaKind Kind = AssetMetaKind::Asset;
		AssetType Type = AssetType::None; // None for a dependency
		AssetHandle Owner{};              // a dependency's owner

		bool operator==(const ImportAssetLookupEntry&) const = default;
	};

	// A file the import read through ReadDependency, with the XXH64 (seed 0) of the bytes it got.
	struct ImportDependencyRead
	{
		VfsPath Path{};
		uint64_t Hash = 0;

		bool operator==(const ImportDependencyRead&) const = default;
	};

	// A lookup the import made through FindAsset, with what it found (nullopt: no standalone asset there).
	struct ImportAssetLookup
	{
		VfsPath Path{};
		std::optional<ImportAssetLookupEntry> Found{};

		bool operator==(const ImportAssetLookup&) const = default;
	};

	// What an importer may read and look up while it imports one source. Created by the EditorAssetManager per import and
	// used by that import alone (one job): not thread-safe. ReadDependency and FindAsset record what they did (part of the
	// import's output, which the cache manifest stores), so importers receive the context by non-const reference.
	class ImportContext
	{
	public:
		struct Specification
		{
			// Reads the source's dependencies; never null; documented back-reference that outlives the context.
			const VirtualFileSystem* Vfs = nullptr;
			VfsPath SourcePath{};                     // "project://Assets/Models/Track.glb" or an engine:// path
			std::span<const std::byte> SourceBytes{}; // the source as read once by the manager; must outlive the context
			// The complete settings object (the .meta's Settings with the importer's defaults for absent fields, validated
			// against GetSettingsTypeName's struct), or null for an importer without settings.
			VariantValue Settings{};
			const TypeRegistry* Registry = nullptr; // frozen; never null
			// The registered assets, sorted by SourcePath (for standalone-texture reuse, §6.4); must outlive the context.
			std::span<const ImportAssetLookupEntry> Assets{};
			IEnvironmentBaker* EnvironmentBaker = nullptr;           // null without a GPU (§7.4 EnvironmentImporter)
			IScriptDiagnosticsProvider* ScriptDiagnostics = nullptr; // null in tools without the type checker (§7.4)
		};

		explicit ImportContext(Specification specification);

		ImportContext(const ImportContext&) = delete;
		ImportContext& operator=(const ImportContext&) = delete;

		[[nodiscard]] const VfsPath& GetSourcePath() const { return m_Specification.SourcePath; }
		[[nodiscard]] std::span<const std::byte> GetSourceBytes() const { return m_Specification.SourceBytes; }
		// The settings object (JSON null for an importer without settings).
		[[nodiscard]] const Json& GetSettings() const { return m_Specification.Settings.Get(); }
		[[nodiscard]] const TypeRegistry& GetRegistry() const { return *m_Specification.Registry; }
		[[nodiscard]] IEnvironmentBaker* GetEnvironmentBaker() const { return m_Specification.EnvironmentBaker; }
		[[nodiscard]] IScriptDiagnosticsProvider* GetScriptDiagnostics() const { return m_Specification.ScriptDiagnostics; }

		// Reads a file the source references, recording it as a dependency read (path and XXH64), so a cached import is
		// valid only while the file is unchanged. `path` must be in the source's scheme and inside its asset root: under
		// project://Assets for a project source (a script's required modules may live anywhere there, §7.4 ScriptImporter),
		// anywhere under engine:// for a built-in. Importers apply their own, stricter rules before reading (GltfImporter:
		// relative URIs inside the source's directory tree). Errors: InvalidArgument for a path in another scheme or outside
		// the asset root; the VFS errors (NotFound naming the referencing source as context).
		[[nodiscard]] Result<Buffer> ReadDependency(const VfsPath& path);

		// The registered asset whose source is exactly `path`, recording the lookup: a standalone Texture meta there makes a
		// glTF reference that texture instead of creating a texture sub-asset (§6.4). nullopt for a file without a .meta.
		[[nodiscard]] std::optional<ImportAssetLookupEntry> FindAsset(const VfsPath& path);

		// What the import read and looked up so far, sorted by path, each path once.
		[[nodiscard]] std::vector<ImportDependencyRead> GetDependencyReads() const;
		[[nodiscard]] std::vector<ImportAssetLookup> GetLookups() const;
	private:
		Specification m_Specification;
		std::vector<ImportDependencyRead> m_Reads; // recorded by ReadDependency
		std::vector<ImportAssetLookup> m_Lookups;  // recorded by FindAsset
	};

	// One cooked artifact an import produced.
	struct ImportedArtifact
	{
		AssetHandle Handle{};             // the main asset (metadata.Handle) or a sub-asset (DeriveSubAssetHandle)
		AssetType Type = AssetType::None; // never None
		std::string SubAssetKey{};        // empty for the main asset
		Buffer Cooked{};                  // the complete artifact: CookedHeader + payload (§6.8)
	};

	// What one import produced.
	struct ImportResult
	{
		// The main asset first, then the sub-assets sorted by key (each key unique). The sub-assets become the .meta's
		// SubAssets list.
		std::vector<ImportedArtifact> Artifacts{};
		// The assets the main asset and its sub-assets reference (material -> textures, prefab -> meshes and materials;
		// standalone textures a glTF reuses), sorted and unique, excluding its own sub-assets: edges of the
		// AssetDependencyGraph (§7.5). The files read through ReadDependency are the source's dependency files (§6.4).
		std::vector<AssetHandle> Dependencies{};
		// Warnings found (ASSET_UNSUPPORTED_UV_SET, ASSET_VERTEX_COLORS_IGNORED, ASSET_TANGENTS_APPROXIMATED,
		// ASSET_CONTENT_SKIPPED), in the order found; their Asset and Path are filled in by the importer. Stored in the cache
		// manifest, so they are reported again when the cache serves the import.
		std::vector<AssetDiagnostic> Diagnostics{};
	};

	// One importer. Stateless and const, so one instance serves every import job at once.
	class IAssetImporter
	{
	public:
		virtual ~IAssetImporter() = default;

		// The id written into .meta files ("Texture", "Gltf", "Material", "Scene", "Prefab", "Font"); unique, PascalCase,
		// never changes.
		[[nodiscard]] virtual std::string_view GetId() const = 0;

		// The importer version (§7.4, §7.5): bump it whenever the cooked output for the same input changes, so cache entries
		// are rebuilt. Starts at 1.
		[[nodiscard]] virtual uint32_t GetVersion() const = 0;

		// The type of the main asset (the .meta's Type): Texture, Prefab (glTF), Material, Scene, Prefab, Font.
		[[nodiscard]] virtual AssetType GetMainType() const = 0;

		// The extensions it imports, lower case with the dot (".png", ".jpg", ".jpeg"); disjoint between importers.
		[[nodiscard]] virtual std::span<const std::string_view> GetExtensions() const = 0;

		// The registry name of its settings struct ("TextureImportSettings"), or empty for an importer without settings.
		[[nodiscard]] virtual std::string_view GetSettingsTypeName() const = 0;

		// True when Import must run on the main thread because it uses main-thread-only state (§4.11): GPU work recorded
		// through IEnvironmentBaker, or an ECS registry (SceneImporter's scratch scene); the EditorAssetManager then imports
		// it there instead of on a job.
		[[nodiscard]] virtual bool RequiresMainThread() const { return false; }

		// The files Import will read through ReadDependency for `source` at `sourcePath` (its dependency closure, §6.4), in
		// `sourcePath`'s scheme, sorted, each once, without importing. The EditorAssetManager resolves the closures of every
		// new or changed source of a batch before it writes .meta files for sources without one (EditorAssetManager.h), so a
		// referenced image becomes a dependency wherever it sorts in the batch. Default: none. Errors: those of Import
		// for a source whose references cannot be read (Parse; ImportFailed naming a rejected URI). Pure and thread-safe.
		[[nodiscard]] virtual Result<std::vector<VfsPath>> ListDependencyFiles(std::span<const std::byte> source, const VfsPath& sourcePath) const;

		// Imports one source (§7.4). `metadata` is the source's .meta (its handle derives the sub-asset handles; its SubAssets
		// list holds the keys of the previous import, which a reimport keeps stable). Errors: ImportFailed with a located,
		// precise message (the offending URI, extension or primitive), Parse, Validation, Unsupported; never an assert on
		// input.
		[[nodiscard]] virtual Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const = 0;

		// Whether `extension` (with the dot) is one of GetExtensions(), ignoring ASCII case.
		[[nodiscard]] bool CanImport(std::string_view extension) const;
	};

}
