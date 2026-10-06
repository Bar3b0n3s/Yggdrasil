#include "TestsPCH.h"

#include "Engine/Asset/TypedAssetHandle.h"

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("TypedAssetHandle: holds a handle tagged with its accepted type")
		{
			using MeshHandle = TypedAssetHandle<AssetType::Mesh>;
			static_assert(MeshHandle::AcceptedType == AssetType::Mesh);
			static_assert(std::is_trivially_copyable_v<MeshHandle>);
			static_assert(!std::is_convertible_v<TypedAssetHandle<AssetType::Material>, MeshHandle>);

			constexpr MeshHandle Empty;
			static_assert(!Empty.IsValid());

			MeshHandle mesh(AssetHandle(0x102));
			CHECK(mesh.IsValid());
			CHECK(mesh.GetHandle() == AssetHandle(0x102));
			CHECK(mesh == MeshHandle(AssetHandle(0x102)));
			CHECK(mesh < MeshHandle(AssetHandle(0x103)));

			mesh.SetHandle(AssetHandle());
			CHECK_FALSE(mesh.IsValid());
		}

		TEST_CASE("TypedAssetHandle: reflects as an AssetRef field filtered by its type")
		{
			using FontHandle = TypedAssetHandle<AssetType::Font>;
			static_assert(AssetFieldTraits<FontHandle>::IsAssetField);
			static_assert(AssetFieldTraits<FontHandle>::AssetTypeName == "Font");
			static_assert(AssetFieldTraits<TypedAssetHandle<AssetType::None>>::AssetTypeName.empty()); // any type
			static_assert(!AssetFieldTraits<UUID>::IsAssetField);
			static_assert(Detail::DeduceFieldType<FontHandle>() == FieldType::AssetRef);
			static_assert(Detail::DeduceFieldType<std::vector<FontHandle>>() == FieldType::Array);
			static_assert(Detail::DeduceFieldType<UUID>() == FieldType::EntityRef);

			FontHandle font;
			AssetFieldTraits<FontHandle>::SetHandle(font, AssetHandle(0x2a));
			CHECK(AssetFieldTraits<FontHandle>::GetHandle(font) == AssetHandle(0x2a));
		}
	}

}
