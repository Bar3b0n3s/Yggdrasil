#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/MaterialImporter.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/MaterialData.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

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

		// Copies Tests/Data/Assets/Materials/<name> into the fixture's project as Assets/<name>.
		void CopyMaterialFixture(Test::AssetTestFixture& fixture, std::string_view name)
		{
			Result<std::string> text = Test::ReadTestDataText(std::string("Assets/Materials/") + std::string(name));
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			fixture.WriteProjectText(std::string("Assets/") + std::string(name), *text);
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("MaterialImporter: cooks the canonical document and lists its textures")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Wood.material", R"({"Format": "Material", "Version": 1, "Roughness": 0.8,
				"BaseColorMap": "00000000000000aa", "NormalMap": "0000000000000055"})");
			Result<ImportResult> imported = ImportMaterial(fixture, "Assets/Wood.material");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::Material);
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x3a7e000000000001ull));
			CHECK(imported->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x55), AssetHandle(0xaa) });
			Result<AssetRef<MaterialData>> material = LoadCookedMaterial(imported->Artifacts.front().Cooked, fixture.GetRegistry());
			REQUIRE_MESSAGE(material.has_value(), material.error().ToString());
			CHECK((*material)->Roughness == 0.8f);
			CHECK((*material)->BaseColorMap.GetHandle() == AssetHandle(0xaa));
		}

		TEST_CASE("MaterialImporter: an unknown member or an invalid value fails the import")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Typo.material", R"({"Format": "Material", "Version": 1, "Rougness": 0.8})");
			Result<ImportResult> typo = ImportMaterial(fixture, "Assets/Typo.material");
			REQUIRE_FALSE(typo.has_value());
			CHECK(typo.error().ToString().find("Rougness") != std::string::npos);
			// Located in the source file.
			CHECK(typo.error().GetLocation().File.find("Typo.material") != std::string::npos);
			fixture.WriteProjectText("Assets/Bad.material", R"({"Format": "Material", "Version": 1, "Metallic": -1})");
			Result<ImportResult> bad = ImportMaterial(fixture, "Assets/Bad.material");
			REQUIRE_FALSE(bad.has_value());
			CHECK(bad.error().GetCode() == ErrorCode::Validation);
			REQUIRE(bad.error().GetLocation().JsonPointer.has_value());
			CHECK(*bad.error().GetLocation().JsonPointer == "/Metallic");
			fixture.WriteProjectText("Assets/Newer.material", R"({"Format": "Material", "Version": 9})");
			Result<ImportResult> newer = ImportMaterial(fixture, "Assets/Newer.material");
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("MaterialImporter: the material fixtures import identically twice")
		{
			Test::AssetTestFixture fixture;
			CopyMaterialFixture(fixture, "Red.material");
			CopyMaterialFixture(fixture, "Textured.material");

			Result<ImportResult> red = ImportMaterial(fixture, "Assets/Red.material");
			REQUIRE_MESSAGE(red.has_value(), red.error().ToString());
			CHECK(red->Dependencies.empty());
			Result<ImportResult> textured = ImportMaterial(fixture, "Assets/Textured.material");
			REQUIRE_MESSAGE(textured.has_value(), textured.error().ToString());
			CHECK(textured->Dependencies
				== std::vector<AssetHandle>{ AssetHandle(0x7e57000000000001ull), AssetHandle(0x7e57000000000002ull), AssetHandle(0x7e57000000000003ull) });
			Result<AssetRef<MaterialData>> material = LoadCookedMaterial(textured->Artifacts.front().Cooked, fixture.GetRegistry());
			REQUIRE(material.has_value());
			CHECK((*material)->AlphaMode == AlphaMode::Mask);
			CHECK((*material)->UVScale == glm::vec2(2.0f, 2.0f));

			Result<ImportResult> again = ImportMaterial(fixture, "Assets/Textured.material");
			REQUIRE(again.has_value());
			CHECK(again->Artifacts.front().Cooked == textured->Artifacts.front().Cooked);
			CHECK(again->Dependencies == textured->Dependencies);
		}

		// M23 (Material version 2; Docs/Decisions/0034-m23-contract.md decision 10): importer version 2 cooks Material version 2
		// from a version 1 file (every UV set UV0) and from a version 2 file (its UV sets), and lists the textures of both as
		// its dependencies (the importer-dependency rule).
		TEST_CASE("MaterialImporter: version 2 cooks Material version 2 with each map's UV set from version 1 and 2 files")
		{
			CHECK(MaterialImporter::Version == 2);
			Test::AssetTestFixture fixture;
			const Result<std::string> legacy = Test::ReadTestDataText("Formats/Material/MaterialV1.material");
			REQUIRE(legacy.has_value());
			fixture.WriteProjectText("Assets/Legacy.material", *legacy);
			fixture.WriteProjectText("Assets/Layered.material", R"({"Format": "Material", "Version": 2, "BaseColorMap": "00000000000000aa",
				"OcclusionMap": "0000000000000055", "OcclusionUVSet": "UV1", "EmissiveUVSet": "UV1"})");

			Result<ImportResult> fromLegacy = ImportMaterial(fixture, "Assets/Legacy.material");
			REQUIRE_MESSAGE(fromLegacy.has_value(), fromLegacy.error().ToString());
			const Result<CookedArtifactView> legacyView = ReadCookedArtifact(fromLegacy->Artifacts.front().Cooked);
			REQUIRE(legacyView.has_value());
			CHECK(legacyView->Header.FormatVersion == 2);
			CHECK(legacyView->Header.ImporterVersion == 2);
			CHECK(AsStringView(legacyView->Payload).find(R"("Version":2)") != std::string_view::npos);
			Result<AssetRef<MaterialData>> legacyMaterial = LoadCookedMaterial(fromLegacy->Artifacts.front().Cooked, fixture.GetRegistry());
			REQUIRE(legacyMaterial.has_value());
			CHECK((*legacyMaterial)->OcclusionUVSet == TextureCoordinateSet::UV0);
			CHECK(fromLegacy->Dependencies
				== std::vector<AssetHandle>{ AssetHandle(0x7e57000000000001ull), AssetHandle(0x7e57000000000002ull), AssetHandle(0x7e57000000000003ull) });

			Result<ImportResult> layered = ImportMaterial(fixture, "Assets/Layered.material");
			REQUIRE_MESSAGE(layered.has_value(), layered.error().ToString());
			Result<AssetRef<MaterialData>> material = LoadCookedMaterial(layered->Artifacts.front().Cooked, fixture.GetRegistry());
			REQUIRE(material.has_value());
			CHECK((*material)->BaseColorUVSet == TextureCoordinateSet::UV0);
			CHECK((*material)->OcclusionUVSet == TextureCoordinateSet::UV1);
			CHECK((*material)->EmissiveUVSet == TextureCoordinateSet::UV1);
			CHECK(layered->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x55), AssetHandle(0xaa) });
		}
	}

}
