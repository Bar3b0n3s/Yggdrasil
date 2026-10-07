#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/MaterialImporter.h"

#include "Engine/Asset/MaterialData.h"
#include "Support/AssetTestFixture.h"

namespace Engine {

	namespace {

		Result<ImportResult> ImportMaterial(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x3a7e000000000001ull);
			metadata.Type = AssetType::Material;
			metadata.Importer = std::string(MaterialImporter::Id);
			metadata.ImporterVersion = MaterialImporter::Version;
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = bytes,
				.Settings = VariantValue(),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			return MaterialImporter().Import(context, metadata);
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("MaterialImporter: cooks the canonical document and lists its textures" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Wood.material", R"({"Format": "Material", "Version": 1, "Roughness": 0.8,
				"BaseColorMap": "00000000000000aa", "NormalMap": "0000000000000055"})");
			Result<ImportResult> imported = ImportMaterial(fixture, "Assets/Wood.material");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x55), AssetHandle(0xaa) });
			Result<AssetRef<MaterialData>> material = LoadCookedMaterial(imported->Artifacts.front().Cooked, fixture.GetRegistry());
			REQUIRE_MESSAGE(material.has_value(), material.error().ToString());
			CHECK((*material)->Roughness == 0.8f);
			CHECK((*material)->BaseColorMap.GetHandle() == AssetHandle(0xaa));
		}

		TEST_CASE("MaterialImporter: an unknown member or an invalid value fails the import" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Typo.material", R"({"Format": "Material", "Version": 1, "Rougness": 0.8})");
			Result<ImportResult> typo = ImportMaterial(fixture, "Assets/Typo.material");
			REQUIRE_FALSE(typo.has_value());
			CHECK(typo.error().ToString().find("Rougness") != std::string::npos);
			fixture.WriteProjectText("Assets/Bad.material", R"({"Format": "Material", "Version": 1, "Metallic": -1})");
			CHECK_FALSE(ImportMaterial(fixture, "Assets/Bad.material").has_value());
		}
	}

}
