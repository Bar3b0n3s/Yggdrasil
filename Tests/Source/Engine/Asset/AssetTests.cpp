#include "TestsPCH.h"

#include "Engine/Asset/Asset.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("Asset: AssetCast returns the asset for its own type and null otherwise")
		{
			const AssetRef<Asset> mesh = CreateRef<MeshData>();
			CHECK(mesh->GetAssetType() == AssetType::Mesh);
			const AssetRef<MeshData> asMesh = AssetCast<MeshData>(mesh);
			REQUIRE(asMesh != nullptr);
			CHECK(asMesh.get() == mesh.get());
			CHECK(AssetCast<TextureData>(mesh) == nullptr);
			CHECK(AssetCast<MaterialData>(nullptr) == nullptr);
		}

		TEST_CASE("Asset: every data type carries its AssetType")
		{
			CHECK(MeshData().GetAssetType() == MeshData::StaticType);
			CHECK(TextureData().GetAssetType() == AssetType::Texture);
			CHECK(MaterialData().GetAssetType() == AssetType::Material);
			MaterialData copy;
			copy.Roughness = 0.25f;
			const MaterialData assigned = copy;
			CHECK(assigned.GetAssetType() == AssetType::Material);
			CHECK(assigned.Roughness == 0.25f);
		}
	}

}
