#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// An asset manager over assets a test publishes in memory (moved from GpuResourceCacheTests.cpp by the M8 contract,
// Docs/Decisions/0013-m8-decisions.md decision 14, because the renderer's tests share it): Publish replaces an asset and
// bumps its version like a hot-reload swap, so GPU mirrors and renderers are tested with exact data (a constant
// environment for the furnace tests, a material with given factors) without importers, a project or a baker. The
// procedural built-ins come from the AssetManager base. Main thread; not copyable.

namespace Engine {

	namespace Test {

		class InMemoryAssetManager final : public AssetManager
		{
		public:
			InMemoryAssetManager();
			~InMemoryAssetManager() override;

			InMemoryAssetManager(const InMemoryAssetManager&) = delete;
			InMemoryAssetManager& operator=(const InMemoryAssetManager&) = delete;

			// Publishes `asset` as `handle` (replacing any earlier one) and bumps its version.
			// Optional readable path enables real path/require resolution in script tests. Omission retains the old path.
			void Publish(AssetHandle handle, AssetRef<Asset> asset, std::string path = {});

			// Removes `handle`: later loads fail with NotFound, as for a deleted asset.
			void Remove(AssetHandle handle);

			// Makes GetCookedHash of `handle` fail with `code`, as the editor's does for an asset whose import failed. Every
			// other handle keeps the base's Unsupported: this manager holds no cooked artifacts.
			void FailCookedHash(AssetHandle handle, ErrorCode code);

			// Load notifies the load observer (AssetManager::SetLoadObserver) for every published asset it returns, as the
			// editor and runtime managers do; procedural built-ins and failures never.

			[[nodiscard]] Result<AssetRef<Asset>> Load(AssetHandle handle) override;
			[[nodiscard]] JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override;
			[[nodiscard]] AssetState GetState(AssetHandle handle) const override;
			[[nodiscard]] const AssetMetadata* GetMetadata(AssetHandle handle) const override;
			[[nodiscard]] AssetType GetAssetType(AssetHandle handle) const override;
			[[nodiscard]] std::optional<AssetHandle> Resolve(std::string_view reference) const override;
			// The procedural built-in's engine path, or "Assets/Test/<handle>".
			[[nodiscard]] std::string GetReferencePath(AssetHandle handle) const override;
			void WaitIdle() override {}
			[[nodiscard]] Result<uint64_t> GetCookedHash(AssetHandle handle) override;
		private:
			MainThreadQueue m_MainThreadQueue;
			JobSystem m_Jobs; // inline
			std::map<AssetHandle, AssetRef<Asset>> m_Assets;
			std::map<AssetHandle, std::string> m_Paths;
			std::map<AssetHandle, ErrorCode> m_CookedHashFailures;
		};

	}

}
