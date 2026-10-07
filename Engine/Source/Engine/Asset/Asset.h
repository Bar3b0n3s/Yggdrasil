#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"

#include <concepts>
#include <memory>

// Loaded assets (Architecture §4.7, §7.2): immutable CPU data shared across threads through AssetRef<T>.

namespace Engine {

	// The common base of every loaded asset type (MeshData, TextureData, MaterialData, FontData, SceneData, PrefabData; the
	// environment, audio clip, script and replay types join with M8, M12 and M13). It records only the asset's AssetType, so
	// AssetCast can tell the concrete type without RTTI (first-party code never uses dynamic_cast or typeid, §3 rule 5).
	//
	// Each derived type declares `static constexpr AssetType StaticType` and passes it to this constructor. The destructor
	// is protected and not virtual: an asset is only ever owned through AssetRef (std::shared_ptr), whose control block
	// remembers the concrete type it was created with (CreateRef<MeshData>), so deleting through Asset* never happens.
	// Copyable, so the reflected data types (MaterialData) keep value semantics. Thread-compatible; loaded assets are const
	// and therefore safe to read from any thread.
	class Asset
	{
	public:
		[[nodiscard]] AssetType GetAssetType() const { return m_Type; }
	protected:
		explicit Asset(AssetType type)
			: m_Type(type)
		{
		}

		Asset(const Asset&) = default;
		Asset(Asset&&) = default;
		Asset& operator=(const Asset&) = default;
		Asset& operator=(Asset&&) = default;
		~Asset() = default;
	private:
		AssetType m_Type = AssetType::None;
	};

	// A loaded asset (§4.7, §7.2): shared, immutable, never null where the AssetManager hands one out (placeholders stand in
	// for missing or failed assets). Old references stay valid after a hot reload replaced the registry entry.
	template<typename T>
	using AssetRef = Ref<const T>;

	// A concrete asset type: derived from Asset with its AssetType as T::StaticType.
	template<typename T>
	concept AssetDataType = std::derived_from<T, Asset> && requires {
		{ T::StaticType } -> std::convertible_to<AssetType>;
	};

	// `asset` as a T when it holds one (its AssetType is T::StaticType), else null; null stays null. The result shares
	// ownership with `asset`. Pure and thread-safe.
	template<AssetDataType T>
	[[nodiscard]] AssetRef<T> AssetCast(const AssetRef<Asset>& asset)
	{
		if (asset == nullptr || asset->GetAssetType() != T::StaticType)
			return nullptr;
		return std::static_pointer_cast<const T>(asset);
	}

}
