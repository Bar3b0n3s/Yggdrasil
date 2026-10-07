#include "TestsPCH.h"

#include "Engine/Asset/MaterialData.h"

#include "Support/SceneTestFixture.h"

namespace Engine {

	namespace {

		// The §6.5 example with every field present (canonical order and spelling).
		constexpr std::string_view RedMaterial = R"({
	"Format": "Material",
	"Version": 1,
	"BaseColor": [0.9, 0.15, 0.15, 1],
	"BaseColorMap": null,
	"Metallic": 0,
	"Roughness": 0.45,
	"MetallicRoughnessMap": null,
	"NormalMap": null,
	"NormalScale": 1,
	"OcclusionMap": null,
	"OcclusionStrength": 1,
	"Emissive": [0, 0, 0],
	"EmissiveStrength": 1,
	"EmissiveMap": null,
	"AlphaMode": "Opaque",
	"AlphaCutoff": 0.5,
	"DoubleSided": false,
	"UVScale": [1, 1],
	"UVOffset": [0, 0]
}
)";

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("MaterialData: a .material file round-trips byte-identically" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			MaterialLoadReport report;
			Result<MaterialData> material = MaterialFromText(RedMaterial, *registry, report);
			REQUIRE_MESSAGE(material.has_value(), material.error().ToString());
			CHECK(report.FileVersion == 1);
			CHECK(report.Diagnostics.empty());
			CHECK(material->BaseColor == glm::vec4(0.9f, 0.15f, 0.15f, 1.0f));
			CHECK(material->Roughness == 0.45f);
			CHECK(material->AlphaMode == AlphaMode::Opaque);
			Result<std::string> text = MaterialToText(*material, *registry);
			REQUIRE(text.has_value());
			CHECK(*text == RedMaterial);

			Result<Buffer> cooked = CookMaterial(*material, *registry, 1);
			REQUIRE(cooked.has_value());
			Result<AssetRef<MaterialData>> loaded = LoadCookedMaterial(*cooked, *registry);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK(MaterialToText(**loaded, *registry).value_or(std::string()) == RedMaterial);
		}

		TEST_CASE("MaterialData: out-of-range values are located Validation errors" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			MaterialLoadReport report;
			Result<MaterialData> rough = MaterialFromText(R"({"Format": "Material", "Version": 1, "Roughness": 1.5})", *registry, report);
			REQUIRE_FALSE(rough.has_value());
			CHECK(rough.error().GetCode() == ErrorCode::Validation);
			REQUIRE(rough.error().GetLocation().JsonPointer.has_value());
			CHECK(*rough.error().GetLocation().JsonPointer == "/Roughness");

			Result<MaterialData> mode = MaterialFromText(R"({"Format": "Material", "Version": 1, "AlphaMode": "opaque"})", *registry, report);
			REQUIRE_FALSE(mode.has_value());
			CHECK(mode.error().GetCode() == ErrorCode::Validation);

			MaterialLoadReport unknownReport;
			Result<MaterialData> unknown = MaterialFromText(R"({"Format": "Material", "Version": 1, "Shininess": 3})", *registry, unknownReport);
			REQUIRE(unknown.has_value());
			CHECK(unknownReport.Diagnostics.size() == 1);
			CHECK_FALSE(MaterialFromText(R"({"Format": "Material", "Version": 1, "Shininess": 3})", *registry, unknownReport, true).has_value());

			Result<MaterialData> newer = MaterialFromText(R"({"Format": "Material", "Version": 2})", *registry, report);
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("MaterialData: the referenced textures are its dependencies" * doctest::skip(true))
		{
			MaterialData material;
			material.BaseColorMap.SetHandle(AssetHandle(30));
			material.NormalMap.SetHandle(AssetHandle(10));
			material.EmissiveMap.SetHandle(AssetHandle(30));
			CHECK(GetMaterialTextureHandles(material) == std::vector<AssetHandle>{ AssetHandle(10), AssetHandle(30) });
			CHECK(GetMaterialTextureHandles(MaterialData()).empty());
		}
	}

}
