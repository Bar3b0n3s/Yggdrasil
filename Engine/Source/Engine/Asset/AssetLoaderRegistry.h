#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/IAssetLoader.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Engine {

	// The loaders of one asset manager, one per AssetType (Architecture §3 rule 4). Built once at startup (Register on one
	// thread), then only read: every const member is thread-safe, so loads on jobs share it. Not copyable.
	class AssetLoaderRegistry
	{
	public:
		AssetLoaderRegistry();
		~AssetLoaderRegistry();

		AssetLoaderRegistry(const AssetLoaderRegistry&) = delete;
		AssetLoaderRegistry& operator=(const AssetLoaderRegistry&) = delete;

		// Adds `loader` (non-null, asserted) for its type; a second loader for the same type is a programmer error (asserted).
		void Register(Scope<IAssetLoader> loader);

		// The loader of `type`, or nullptr.
		[[nodiscard]] const IAssetLoader* Find(AssetType type) const;

		// The types that have a loader, in AssetType order.
		[[nodiscard]] std::vector<AssetType> GetTypes() const;

		// Reads the artifact's header and dispatches to the loader of its type. Errors: those of ReadCookedArtifact; Unsupported
		// "no loader for AssetType <Type>" when no loader was registered; the loader's errors.
		[[nodiscard]] Result<AssetRef<Asset>> Load(std::span<const std::byte> cooked, const AssetLoadContext& context) const;
	private:
		std::vector<Scope<IAssetLoader>> m_Loaders; // sorted by type
	};

	// Registers the built-in loaders: M6's Mesh (LoadCookedMesh), Texture (LoadCookedTexture), Material (LoadCookedMaterial),
	// Font (LoadCookedFont), Scene (LoadCookedScene) and Prefab (LoadCookedPrefab), M8's Environment (LoadCookedEnvironment)
	// and AudioClip, Script and Replay. Both asset managers call the same registry and cooked loaders.
	void RegisterBuiltinLoaders(AssetLoaderRegistry& registry);

	// The payload format version (CookedHeader::FormatVersion) that this build's built-in loader of `type` reads and its
	// importer writes: the FormatVersion of its data type (SceneData, PrefabData, MeshData, MaterialData, TextureData,
	// EnvironmentData, AudioClipData, ScriptData, FontData, ReplayData, SpriteSheetData, TilemapData, StringTableData,
	// SkeletonData, AnimationClipData), which each loader requires exactly, but for M22's font loader, which still reads
	// FontData format 1 (Docs/Decisions/0033-m22-contract.md decision 44). nullopt for None and a value outside AssetType.
	// The cooked caches treat an artifact at any other version as an entry to rebuild, never as current (AssetCache::Find
	// and ReadArtifact, Architecture §7.5), so a payload format change rebuilds the entries of an importer whose own version
	// and cache key it leaves unchanged (M23, Docs/Decisions/0034-m23-contract.md decision 20). Pure and thread-safe.
	[[nodiscard]] std::optional<uint16_t> GetCookedFormatVersion(AssetType type);

}
