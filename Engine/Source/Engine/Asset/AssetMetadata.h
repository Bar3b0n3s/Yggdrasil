#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Asset metadata (Architecture §6.4), the ".meta" sidecar every asset file has, native or foreign. The registry is rebuilt
// by scanning .meta files (§7.3), so there is no central registry file to conflict in merges. This header freezes both
// .meta formats of M6 (Roadmap M6 "the dependency-meta format").
//
// An asset meta, written canonically (JsonWriter, Pretty), keys in this order:
//     {
//         "Format": "AssetMeta", "Version": 1,
//         "Handle": "3c9f2e7a11d04b88", "Type": "Prefab", "Importer": "Gltf", "ImporterVersion": 1,
//         "Settings": { ...every field of the importer's settings struct, registry order... },
//         "SubAssets": [ { "Key": "mesh:0:Straight", "Handle": "77e1a0c4d2b95f01", "Type": "Mesh" }, ... ]
//     }
// "SubAssets" is sorted by Key (byte-wise) and empty for importers without sub-assets. Sub-asset handles are
// Hash64(Handle, Key) (§4.8, §7.1; DeriveSubAssetHandle), so reimporting a modified file keeps every handle as long as its
// keys are stable (glTF keys are "<kind>:<index>[:<name>]", GltfImporter.h).
//
// A dependency meta (§6.4) marks a file another asset reads (a glTF's external .bin buffer, or an image without a meta of
// its own), keys in this order:
//     { "Format": "AssetMeta", "Version": 1, "Handle": "…", "Type": "Dependency", "Owner": "<owner handle>" }
// A dependency is never loaded or imported on its own, has no AssetType ("Dependency" is the meta's kind, not an
// AssetType), and moves, is deleted and is trashed together with its owner (AssetRegistry::PlanMove, PlanTrash).
// A dependency has exactly one owner: a file that is already one asset's dependency cannot be read by another asset's
// import (the EditorAssetManager fails that import with ImportFailed naming the file and its owner, hint "give each glTF
// its own copy of the file"), so moving or trashing an owner never takes away a file another asset reads. Files that
// several glTFs share must be standalone assets (an image with its own Texture meta, §6.4), which a glTF references by
// handle and which never move with it.

namespace Engine {

	// What a .meta describes.
	enum class AssetMetaKind : uint8_t
	{
		Asset,     // an importable source with a main asset and optional sub-assets
		Dependency // a file read by its Owner's importer (§6.4)
	};

	// One sub-asset of an imported source (a glTF's meshes, materials and textures).
	struct SubAssetEntry
	{
		std::string Key{};                // stable identity within the source ("mesh:0:Straight"); non-empty, unique per source
		AssetHandle Handle{};             // DeriveSubAssetHandle(source handle, Key)
		AssetType Type = AssetType::None; // never None

		bool operator==(const SubAssetEntry&) const = default;
	};

	// The content of one .meta file. Plain value; thread-compatible.
	struct AssetMetadata
	{
		static constexpr std::string_view FormatName = "AssetMeta";
		// The "Type" spelling of a dependency meta.
		static constexpr std::string_view DependencyTypeName = "Dependency";
		static constexpr uint32_t CurrentVersion = 1;

		AssetHandle Handle{}; // never null in a valid meta
		AssetMetaKind Kind = AssetMetaKind::Asset;
		// Kind Asset: the main asset's type (the prefab of a glTF); never None. Kind Dependency: None.
		AssetType Type = AssetType::None;
		// Kind Asset: the importer id (IAssetImporter::GetId, "Gltf"); empty for dependencies.
		std::string Importer{};
		// Kind Asset: the version of the importer that last wrote this meta. Informational: cache keys use the registered
		// importer's current version (§7.5).
		uint32_t ImporterVersion = 0;
		// Kind Asset: the importer's settings as a JSON object holding every field of its settings struct (canonical, registry
		// order; JSON null only for an importer without settings). Kind Dependency: null.
		VariantValue Settings{};
		// Kind Asset: the sub-assets, sorted by Key. Kind Dependency: empty.
		std::vector<SubAssetEntry> SubAssets{};
		// Kind Dependency: the owning asset's handle (never null). Kind Asset: null.
		AssetHandle Owner{};

		// The sub-asset with `key`, or nullptr.
		[[nodiscard]] const SubAssetEntry* FindSubAsset(std::string_view key) const;

		bool operator==(const AssetMetadata&) const = default;
	};

	// The handle of the sub-asset `key` of the source `source` (§4.8, §7.1): Hash64(source, key), the XXH64 of the key's
	// UTF-8 bytes seeded with the source handle. Deterministic across runs, hosts and configurations. Pure.
	[[nodiscard]] inline AssetHandle DeriveSubAssetHandle(AssetHandle source, std::string_view key)
	{
		return AssetHandle(Hash64(source.GetValue(), key));
	}

	// The .meta path of `source`: the sibling "<file name>.meta" ("project://Assets/Track.glb" ->
	// "project://Assets/Track.glb.meta"). Errors: InvalidArgument for a root or empty path, or a source that is itself a
	// .meta.
	[[nodiscard]] Result<VfsPath> GetMetaPath(const VfsPath& source);

	// The source path of the .meta `metaPath` (the inverse of GetMetaPath). Errors: InvalidArgument when the file name does
	// not end in ".meta" or nothing precedes it.
	[[nodiscard]] Result<VfsPath> GetSourcePathOfMeta(const VfsPath& metaPath);

	// Reads a .meta file's text (either format above). Strict like every authored-file reader (§6): Parse errors located at
	// line and column, Validation errors located at their JSON pointer for a wrong type, a missing or null Handle, an
	// unknown Type, a Dependency without an Owner or with asset members, an asset meta without an Importer or with an
	// Owner, a SubAssets entry with an empty or repeated Key, a None type or a handle other than
	// DeriveSubAssetHandle(Handle, Key), or unsorted SubAssets; UnsupportedVersion for a Version above CurrentVersion.
	// Unknown members are a Validation error too (a .meta is engine-written; a hand edit that adds members is a mistake to
	// report, not to preserve). `metaPath` only names the file in error locations. Pure and thread-safe.
	[[nodiscard]] Result<AssetMetadata> ParseAssetMetadata(std::string_view text, std::string_view metaPath = {});

	// The canonical text of `metadata` (JsonWriter Pretty, the key order above, a final newline). Parsing the result gives
	// `metadata` back, and writing a parsed canonical file gives the same bytes (§6 "load then save is byte-identical").
	// Asserts a valid meta (non-null Handle; for Kind Asset a type, an importer and an object or null Settings; for Kind
	// Dependency an Owner and no asset members; sorted, valid SubAssets). Pure and thread-safe.
	[[nodiscard]] std::string SerializeAssetMetadata(const AssetMetadata& metadata);

	// A dependency meta for a file owned by `owner`, with handle `handle`.
	[[nodiscard]] inline AssetMetadata MakeDependencyMetadata(AssetHandle handle, AssetHandle owner)
	{
		AssetMetadata metadata;
		metadata.Handle = handle;
		metadata.Kind = AssetMetaKind::Dependency;
		metadata.Owner = owner;
		return metadata;
	}

}
