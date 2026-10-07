#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/IAssetLoader.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
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
		// "no loader for AssetType <Type>" (the environment, audio clip, script and replay loaders arrive with M8, M12 and
		// M13); the loader's errors.
		[[nodiscard]] Result<AssetRef<Asset>> Load(std::span<const std::byte> cooked, const AssetLoadContext& context) const;
	private:
		std::vector<Scope<IAssetLoader>> m_Loaders; // sorted by type
	};

	// Registers the M6 loaders: Mesh (LoadCookedMesh), Texture (LoadCookedTexture), Material (LoadCookedMaterial), Font
	// (LoadCookedFont), Scene (LoadCookedScene) and Prefab (LoadCookedPrefab). Both asset managers call it; later milestones
	// add theirs here.
	void RegisterBuiltinLoaders(AssetLoaderRegistry& registry);

}
