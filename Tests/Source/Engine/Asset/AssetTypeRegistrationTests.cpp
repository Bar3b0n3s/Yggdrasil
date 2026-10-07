#include "TestsPCH.h"

#include "Engine/Asset/AssetTypeRegistration.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Reflection/TypeRegistry.h"

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetTypeRegistration: registers the Material struct and the AlphaMode enum" * doctest::skip(true))
		{
			TypeRegistry registry;
			RegisterAssetTypes(registry);
			registry.Freeze();
			const StructInfo* material = registry.FindStruct("Material");
			REQUIRE(material != nullptr);
			CHECK(registry.FindStruct<MaterialData>() == material);
			CHECK(material->FindField("BaseColorMap") != nullptr);
			CHECK(material->FindField("AlphaMode") != nullptr);
			REQUIRE(registry.FindEnum("AlphaMode") != nullptr);
			CHECK(registry.FindEnum<AlphaMode>() == registry.FindEnum("AlphaMode"));
		}
	}

}
