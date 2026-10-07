#pragma once

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <optional>
#include <string>
#include <string_view>

// The exported game's asset manager (Architecture §3 rule 4, §7.2, §14.3): cooked assets from paks, decoded by the same
// loaders the editor uses on its cache entries. Built by RuntimeApp (M7), which injects it into EngineContext.

namespace Engine {

	class AssetLoaderRegistry;
	class JobSystem;
	class MainThreadQueue;
	class TypeRegistry;

	struct RuntimeAssetManagerSpecification
	{
		// The context's services (documented back-references that outlive the manager); never null.
		JobSystem* Jobs = nullptr;
		MainThreadQueue* MainThread = nullptr;
		const TypeRegistry* Registry = nullptr;
		// The loaders (RegisterBuiltinLoaders); never null; outlives the manager.
		const AssetLoaderRegistry* Loaders = nullptr;
	};

	// Serves the cooked-asset entries of its paks by handle. Paks are added once at startup (Engine.pak, then Game.pak) and
	// never change, so a loaded asset's version stays 1: there is no hot reload in an exported game (§14.4). An entry that
	// fails its hash check or its loader is reported once as an asset diagnostic (AssetImportFailedCode) and the caller gets
	// the error (GetOrPlaceholder: the placeholder). Main thread, as AssetManager; loads decode on jobs.
	class RuntimeAssetManager final : public AssetManager
	{
	public:
		explicit RuntimeAssetManager(const RuntimeAssetManagerSpecification& specification);
		~RuntimeAssetManager() override;

		// Adds the cooked-asset entries of `pak` (non-null, asserted); its plain files are PakMount's. An entry whose type has
		// no loader in this build is still added and fails when loaded (Unsupported). Errors: AlreadyExists naming both paks
		// when a handle or path of `pak` is already served.
		[[nodiscard]] Status AddPak(Ref<const PakReader> pak);

		[[nodiscard]] Result<AssetRef<Asset>> Load(AssetHandle handle) override;
		[[nodiscard]] JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override;
		[[nodiscard]] AssetState GetState(AssetHandle handle) const override;
		// Paks carry no .meta files: always nullptr.
		[[nodiscard]] const AssetMetadata* GetMetadata(AssetHandle handle) const override;
		[[nodiscard]] AssetType GetAssetType(AssetHandle handle) const override;
		// Handles of pak entries and procedural built-ins; project paths, sub-asset paths and engine paths through the entries'
		// Path (FormatAssetReference).
		[[nodiscard]] std::optional<AssetHandle> Resolve(std::string_view reference) const override;
		[[nodiscard]] std::string GetReferencePath(AssetHandle handle) const override;
		void WaitIdle() override;
	private:
		// The specification, the paks, the entry index and the loaded assets with their states (RuntimeAssetManager.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
