#include "TestsPCH.h"
#include "Support/InMemoryAssetManager.h"

#include "Engine/Asset/BuiltinAssets.h"

#include <algorithm>
#include <format>
#include <span>
#include <utility>

namespace Engine {

	namespace Test {

		InMemoryAssetManager::InMemoryAssetManager()
			: m_Jobs(0, m_MainThreadQueue)
		{
		}

		InMemoryAssetManager::~InMemoryAssetManager() = default;

		void InMemoryAssetManager::Publish(AssetHandle handle, AssetRef<Asset> asset)
		{
			m_Assets.insert_or_assign(handle, std::move(asset));
			BumpVersion(handle);
		}

		void InMemoryAssetManager::Remove(AssetHandle handle)
		{
			m_Assets.erase(handle);
			BumpVersion(handle);
		}

		Result<AssetRef<Asset>> InMemoryAssetManager::Load(AssetHandle handle)
		{
			if (IsBuiltinAssetHandle(handle) && !m_Assets.contains(handle))
				return GetProceduralBuiltin(handle);
			const auto found = m_Assets.find(handle);
			if (found == m_Assets.end())
				return MakeError(ErrorCode::NotFound, "no asset {}", handle);
			return found->second;
		}

		JobHandle<AssetRef<Asset>> InMemoryAssetManager::LoadAsync(AssetHandle handle)
		{
			return m_Jobs.Submit([result = Load(handle)]()
			{
				return result;
			});
		}

		AssetState InMemoryAssetManager::GetState(AssetHandle handle) const
		{
			return m_Assets.contains(handle) ? AssetState::Loaded : AssetState::Unloaded;
		}

		const AssetMetadata* InMemoryAssetManager::GetMetadata(AssetHandle /*handle*/) const
		{
			return nullptr;
		}

		AssetType InMemoryAssetManager::GetAssetType(AssetHandle handle) const
		{
			const auto found = m_Assets.find(handle);
			return found != m_Assets.end() ? found->second->GetAssetType() : AssetType::None;
		}

		std::optional<AssetHandle> InMemoryAssetManager::Resolve(std::string_view /*reference*/) const
		{
			return std::nullopt;
		}

		std::string InMemoryAssetManager::GetReferencePath(AssetHandle handle) const
		{
			const std::span<const BuiltinAssetEntry> builtins = GetProceduralBuiltinEntries();
			const auto builtin = std::ranges::find(builtins, handle, &BuiltinAssetEntry::Handle);
			return builtin != builtins.end() ? builtin->Path : std::format("Assets/Test/{}", handle);
		}

	}

}
