#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/LutImporter.h"

#include "Engine/Asset/LutData.h"
#include "Engine/Core/FileSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <string>
#include <string_view>

// LutImporter (M26, C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decision 15, stream X-B): `.cube` files into Lut
// assets, with no settings and no dependencies (ADR 0028's phase-3 ledger's importer rule).

namespace Engine {

	namespace {

		Result<ImportResult> ImportLut(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x1D70000000000001ull);
			metadata.Type = AssetType::Lut;
			metadata.Importer = std::string(LutImporter::Id);
			metadata.ImporterVersion = LutImporter::Version;
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
			Result<ImportResult> result = LutImporter().Import(context, metadata);
			// The importer reads nothing but its source.
			CHECK(context.GetDependencyReads().empty());
			return result;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("LutImporter: a .cube file imports as a Lut asset with no dependencies, identically every time")
		{
			const LutImporter importer;
			CHECK(importer.GetId() == "Lut");
			CHECK(importer.GetVersion() == 1);
			CHECK(importer.GetMainType() == AssetType::Lut);
			CHECK(importer.CanImport(".cube"));
			CHECK(importer.CanImport(".CUBE"));
			CHECK(importer.GetSettingsTypeName().empty());

			const Result<std::string> warm = FileSystem::ReadText(Test::GetTestDataPath("Assets/Luts/Warm.cube"));
			REQUIRE_MESSAGE(warm.has_value(), warm.error().ToString());
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Luts/Warm.cube", *warm);
			const Result<ImportResult> imported = ImportLut(fixture, "Assets/Luts/Warm.cube");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::Lut);
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x1D70000000000001ull));
			CHECK(imported->Artifacts.front().SubAssetKey.empty());
			// A LUT references no asset (ADR 0028's phase-3 ledger: every new importer fills Import.Dependencies).
			CHECK(imported->Dependencies.empty());
			CHECK(imported->Diagnostics.empty());
			const Result<AssetRef<LutData>> lut = LoadCookedLut(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(lut.has_value(), lut.error().ToString());
			const Result<LutData> parsed = ParseCubeLut(*warm);
			REQUIRE(parsed.has_value());
			CHECK((*lut)->Size == 9);
			CHECK((*lut)->Values == parsed->Values);

			// A pure function of the source and the version: a second import gives identical bytes.
			const Result<ImportResult> again = ImportLut(fixture, "Assets/Luts/Warm.cube");
			REQUIRE(again.has_value());
			CHECK(again->Artifacts.front().Cooked == imported->Artifacts.front().Cooked);
		}

		TEST_CASE("LutImporter: a malformed or 1D .cube file fails the import, located at its line in its file")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Luts/Typo.cube", "LUT_3D_SIZE 2\nGAMMA 2.2\n");
			const Result<ImportResult> typo = ImportLut(fixture, "Assets/Luts/Typo.cube");
			REQUIRE_FALSE(typo.has_value());
			CHECK(typo.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(typo.error().GetLocation().File.find("Typo.cube") != std::string::npos);
			CHECK(typo.error().GetLocation().Line == 2);

			fixture.WriteProjectText("Assets/Luts/Curve.cube", "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
			const Result<ImportResult> curve = ImportLut(fixture, "Assets/Luts/Curve.cube");
			REQUIRE_FALSE(curve.has_value());
			CHECK(curve.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(curve.error().GetLocation().Line == 1);
		}
	}

}
