#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/TypeInfo.h"

#include <compare>
#include <string_view>

namespace Engine {

	// The storage of an asset-reference field (Architecture §5.3 "AssetRef<T> fields hold an AssetHandle"): an AssetHandle
	// tagged at compile time with the AssetType the field accepts, so a mesh slot cannot be assigned a material handle
	// by C++ code, and the reflected field carries its AssetFilter automatically (FieldType::AssetRef, FieldMeta::AssetFilter
	// = AssetTypeToString(Type), or empty, meaning any type, for AssetType::None). The null handle means "no asset" (the
	// documented default per field, e.g. a null Font is the default font) and is written as JSON null.
	//
	// It is not the loaded-asset type: §4.7 and §7.2 name that AssetRef<T> = Ref<const T>, which the M6 Asset module adds
	// next to this one (Docs/Decisions/0006-m3-decisions.md, decision 2). A handle says which asset; whether its type
	// matches at run time is checked by the asset manager and the validator, never assumed. A trivially copyable value
	// type; thread-compatible.
	template<AssetType Type>
	class TypedAssetHandle
	{
	public:
		static constexpr AssetType AcceptedType = Type;

		constexpr TypedAssetHandle() = default;
		constexpr explicit TypedAssetHandle(AssetHandle handle)
			: m_Handle(handle)
		{
		}

		[[nodiscard]] constexpr AssetHandle GetHandle() const { return m_Handle; }
		constexpr void SetHandle(AssetHandle handle) { m_Handle = handle; }
		[[nodiscard]] constexpr bool IsValid() const { return m_Handle.IsValid(); }

		constexpr std::strong_ordering operator<=>(const TypedAssetHandle&) const = default;
	private:
		AssetHandle m_Handle;
	};

	template<AssetType Type>
	struct AssetFieldTraits<TypedAssetHandle<Type>>
	{
		static constexpr bool IsAssetField = true;
		// The FieldMeta::AssetFilter spelling: empty ("any type") for AssetType::None, the AssetType name otherwise.
		static constexpr std::string_view AssetTypeName = Type == AssetType::None ? std::string_view() : AssetTypeToString(Type);

		[[nodiscard]] static UUID GetHandle(const TypedAssetHandle<Type>& value) { return value.GetHandle(); }
		static void SetHandle(TypedAssetHandle<Type>& value, UUID handle) { value.SetHandle(handle); }
	};

}
