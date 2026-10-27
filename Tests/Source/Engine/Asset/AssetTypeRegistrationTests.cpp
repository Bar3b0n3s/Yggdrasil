#include "TestsPCH.h"

#include "Engine/Asset/AssetTypeRegistration.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
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

		// M18 (Docs/Decisions/0029-m18-contract.md decisions 14 and 15): the sheet's structs follow their C++ members, with the
		// ranges of SpriteSheetData.h, and the rules across fields run as Validate hooks with Generate hooks beside them.
		TEST_CASE("AssetTypeRegistration: the sprite sheet structs follow their members with their ranges and hooks")
		{
			TypeRegistry registry;
			RegisterAssetTypes(registry);
			registry.Freeze();
			const auto names = [](const StructInfo& type)
			{
				std::vector<std::string> fields;
				for (const Scope<FieldInfo>& field : type.GetFields())
					fields.push_back(field->GetName());
				return fields;
			};
			const StructInfo* sheet = registry.FindStruct<SpriteSheetData>();
			REQUIRE(sheet != nullptr);
			CHECK(sheet == registry.FindStruct("SpriteSheet"));
			CHECK(names(*sheet) == std::vector<std::string>{ "Texture", "Frames", "Clips" });
			CHECK(sheet->FindField("Texture")->GetMeta().AssetFilter == "Texture");
			const StructInfo* frame = registry.FindStruct<SpriteFrame>();
			REQUIRE(frame != nullptr);
			CHECK(names(*frame) == std::vector<std::string>{ "Name", "Rect", "Pivot", "Borders" });
			CHECK(frame->FindField("Pivot")->GetMeta().Min == 0.0);
			CHECK(frame->FindField("Pivot")->GetMeta().Max == 1.0);
			const StructInfo* rect = registry.FindStruct<SpriteRect>();
			REQUIRE(rect != nullptr);
			CHECK(names(*rect) == std::vector<std::string>{ "X", "Y", "Width", "Height" });
			CHECK(rect->FindField("Width")->GetMeta().Min == 1.0);
			CHECK(rect->FindField("Width")->GetMeta().Max == static_cast<double>(MaxTextureDimension));
			const StructInfo* borders = registry.FindStruct<SpriteBorders>();
			REQUIRE(borders != nullptr);
			CHECK(names(*borders) == std::vector<std::string>{ "Left", "Top", "Right", "Bottom" });
			const StructInfo* clip = registry.FindStruct<SpriteClip>();
			REQUIRE(clip != nullptr);
			CHECK(names(*clip) == std::vector<std::string>{ "Name", "Frames", "Loop" });
			const StructInfo* step = registry.FindStruct<SpriteClipFrame>();
			REQUIRE(step != nullptr);
			CHECK(names(*step) == std::vector<std::string>{ "Frame", "Ticks" });
			CHECK(step->FindField("Ticks")->GetMeta().Min == 1.0);
			CHECK(step->FindField("Ticks")->GetMeta().Max == static_cast<double>(MaxSpriteFrameTicks));
			for (const StructInfo* hooked : { sheet, frame, clip })
			{
				CAPTURE(hooked->GetName());
				CHECK(hooked->HasValidators());
				CHECK(hooked->HasGenerators());
			}
		}
	}

}
