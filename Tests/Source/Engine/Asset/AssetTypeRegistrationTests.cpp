#include "TestsPCH.h"

#include "Engine/Asset/AssetTypeRegistration.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Reflection/TypeRegistry.h"

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetTypeRegistration: registers the Material struct and the AlphaMode enum")
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

		TEST_CASE("AssetTypeRegistration: Material fields follow MaterialData in member order with their bounds")
		{
			TypeRegistry registry;
			RegisterAssetTypes(registry);
			registry.Freeze();
			const StructInfo* material = registry.FindStruct<MaterialData>();
			REQUIRE(material != nullptr);
			const std::vector<std::string> expected = { "BaseColor", "BaseColorMap", "Metallic", "Roughness", "MetallicRoughnessMap", "NormalMap",
				"NormalScale", "OcclusionMap", "OcclusionStrength", "Emissive", "EmissiveStrength", "EmissiveMap", "AlphaMode", "AlphaCutoff",
				"DoubleSided", "UVScale", "UVOffset" };
			std::vector<std::string> names;
			for (const Scope<FieldInfo>& field : material->GetFields())
				names.push_back(field->GetName());
			CHECK(names == expected);

			CHECK(material->FindField("BaseColor")->GetKind() == FieldType::Color4);
			CHECK(material->FindField("Emissive")->GetKind() == FieldType::Color3);
			CHECK(material->FindField("NormalMap")->GetMeta().AssetFilter == "Texture");
			CHECK(material->FindField("Roughness")->GetMeta().Max == 1.0);
			CHECK(material->FindField("EmissiveStrength")->GetMeta().Min == 0.0);
			CHECK_FALSE(material->FindField("EmissiveStrength")->GetMeta().Max.has_value());
			const EnumInfo* alphaMode = registry.FindEnum<AlphaMode>();
			REQUIRE(alphaMode != nullptr);
			CHECK(alphaMode->GetEntries().size() == 3);
		}
	}

}
