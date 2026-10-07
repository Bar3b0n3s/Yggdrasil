#include "TestsPCH.h"

#include "Engine/Asset/MaterialData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <limits>

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
		TEST_CASE("MaterialData: a .material file round-trips byte-identically")
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

		TEST_CASE("MaterialData: out-of-range values are located Validation errors")
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

		TEST_CASE("MaterialData: defaults write every field after the header, and wrong headers or types fail")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			Result<Json> document = MaterialToJson(MaterialData(), *registry);
			REQUIRE(document.has_value());
			REQUIRE(document->is_object());
			CHECK(document->begin().key() == "Format");
			CHECK((*document)["Version"] == Json(1));
			CHECK(document->size() == 2 + 17);
			CHECK((*document)["BaseColorMap"].is_null());
			CHECK((*document)["AlphaMode"] == Json("Opaque"));

			MaterialLoadReport report;
			Result<MaterialData> defaults = MaterialFromText(R"({"Format": "Material", "Version": 1})", *registry, report);
			REQUIRE(defaults.has_value());
			CHECK(defaults->Roughness == 0.5f);
			CHECK(defaults->UVScale == glm::vec2(1.0f, 1.0f));

			Result<MaterialData> wrongFormat = MaterialFromText(R"({"Format": "Scene", "Version": 1})", *registry, report);
			REQUIRE_FALSE(wrongFormat.has_value());
			CHECK(wrongFormat.error().GetCode() == ErrorCode::Validation);
			Result<MaterialData> oldVersion = MaterialFromText(R"({"Format": "Material", "Version": 0})", *registry, report);
			REQUIRE_FALSE(oldVersion.has_value());
			CHECK(oldVersion.error().GetCode() == ErrorCode::Validation);
			Result<MaterialData> notJson = MaterialFromText(R"({"Format": )", *registry, report);
			REQUIRE_FALSE(notJson.has_value());
			CHECK(notJson.error().GetCode() == ErrorCode::Parse);
			Result<MaterialData> notObject = MaterialFromText("[1, 2]", *registry, report);
			REQUIRE_FALSE(notObject.has_value());
			CHECK(notObject.error().GetCode() == ErrorCode::Validation);
			Result<MaterialData> wrongType = MaterialFromText(R"({"Format": "Material", "Version": 1, "DoubleSided": 1})", *registry, report);
			REQUIRE_FALSE(wrongType.has_value());
			CHECK(wrongType.error().GetCode() == ErrorCode::Validation);
			Result<MaterialData> negative = MaterialFromText(R"({"Format": "Material", "Version": 1, "NormalScale": -0.5})", *registry, report);
			REQUIRE_FALSE(negative.has_value());
			CHECK(negative.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("MaterialData: a non-finite value cannot be written")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			MaterialData material;
			material.UVOffset.x = std::numeric_limits<float>::quiet_NaN();
			Result<std::string> text = MaterialToText(material, *registry);
			REQUIRE_FALSE(text.has_value());
			CHECK(text.error().GetCode() == ErrorCode::Validation);
			CHECK_FALSE(CookMaterial(material, *registry, 1).has_value());
		}

		TEST_CASE("MaterialData: the cooked material is the minified document and is read strictly")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			MaterialData material;
			material.AlphaMode = AlphaMode::Blend;
			material.NormalMap.SetHandle(AssetHandle(0x1234));
			Result<Buffer> cooked = CookMaterial(material, *registry, 3);
			REQUIRE(cooked.has_value());
			Result<CookedArtifactView> view = ReadCookedArtifact(*cooked, AssetType::Material, MaterialData::FormatVersion);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			CHECK(view->Header.ImporterVersion == 3);
			Result<std::string> minified = MaterialToText(material, *registry, JsonStyle::Minified);
			REQUIRE(minified.has_value());
			CHECK(AsStringView(view->Payload) == *minified);
			CHECK(minified->find('\n') == std::string::npos);

			Result<AssetRef<MaterialData>> loaded = LoadCookedMaterial(*cooked, *registry);
			REQUIRE(loaded.has_value());
			CHECK((*loaded)->AlphaMode == AlphaMode::Blend);
			CHECK((*loaded)->NormalMap.GetHandle() == AssetHandle(0x1234));

			// An unknown member in cooked data is an error: cooked materials are engine-written.
			const std::string extra = R"({"Format":"Material","Version":1,"Shininess":3})";
			const Buffer unknown = WriteCookedArtifact(AssetType::Material, MaterialData::FormatVersion, 1, AsBytes(extra));
			CHECK_FALSE(LoadCookedMaterial(unknown, *registry).has_value());
		}

		TEST_CASE("MaterialData: the referenced textures are its dependencies")
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
