#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/IrradianceVolumeImporter.h"

#include "Engine/Asset/IrradianceVolumeData.h"
#include "Support/AssetTestFixture.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// IrradianceVolumeImporter (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8): the .irradiance files
// lighting.bake writes, cooked unchanged.

namespace Engine {

	namespace {

		// A 2 x 3 x 2 volume whose probe p's coefficient k is (p, k, -p) / 8.
		IrradianceVolumeData MakeImportedVolume()
		{
			IrradianceVolumeData volume;
			volume.CountX = 2;
			volume.CountY = 3;
			volume.CountZ = 2;
			volume.Size = glm::vec3(3.0f, 2.5f, 8.0f);
			volume.Probes.resize(12);
			for (size_t probe = 0; probe < volume.Probes.size(); ++probe)
			{
				for (size_t k = 0; k < 9; ++k)
					volume.Probes[probe].SH9[k] = glm::vec3(static_cast<float>(probe), static_cast<float>(k), -static_cast<float>(probe)) / 8.0f;
			}
			return volume;
		}

		Result<ImportResult> ImportVolume(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x1BB0000000000001ull);
			metadata.Type = AssetType::IrradianceVolume;
			metadata.Importer = std::string(IrradianceVolumeImporter::Id);
			metadata.ImporterVersion = IrradianceVolumeImporter::Version;
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
			Result<ImportResult> result = IrradianceVolumeImporter().Import(context, metadata);
			// The importer reads nothing but its source.
			CHECK(context.GetDependencyReads().empty());
			return result;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("IrradianceVolumeImporter: a baked .irradiance imports its probes unchanged, with no settings or dependencies")
		{
			const IrradianceVolumeImporter importer;
			CHECK(importer.GetId() == "IrradianceVolume");
			CHECK(importer.GetVersion() == 1);
			CHECK(importer.GetMainType() == AssetType::IrradianceVolume);
			CHECK(importer.GetSettingsTypeName().empty());
			CHECK(importer.CanImport(".Irradiance"));
			Test::AssetTestFixture fixture;
			const IrradianceVolumeData volume = MakeImportedVolume();
			fixture.WriteProjectFile("Assets/Lighting/Room/Volume.irradiance", WriteIrradianceVolumeSource(volume));
			const Result<ImportResult> imported = ImportVolume(fixture, "Assets/Lighting/Room/Volume.irradiance");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::IrradianceVolume);
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x1BB0000000000001ull));
			// A volume references no asset (ADR 0028's phase-3 ledger's importer rule).
			CHECK(imported->Dependencies.empty());
			CHECK(imported->Diagnostics.empty());
			const Result<AssetRef<IrradianceVolumeData>> loaded = LoadCookedIrradianceVolume(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->CountY == 3);
			CHECK((*loaded)->Size == volume.Size);
			CHECK((*loaded)->Probes == volume.Probes);
			// The cooked payload is the source's payload, byte for byte.
			CHECK(imported->Artifacts.front().Cooked == CookIrradianceVolume(volume, IrradianceVolumeImporter::Version));
		}

		TEST_CASE("IrradianceVolumeImporter: a source that is not a version 1 volume fails the import")
		{
			Test::AssetTestFixture fixture;
			Buffer source = WriteIrradianceVolumeSource(MakeImportedVolume());
			fixture.WriteProjectFile("Assets/Truncated.irradiance", std::span(source).first(source.size() - 4));
			const Result<ImportResult> truncated = ImportVolume(fixture, "Assets/Truncated.irradiance");
			REQUIRE_FALSE(truncated.has_value());
			CHECK(truncated.error().GetCode() == ErrorCode::ImportFailed);

			source[4] = std::byte{ 2 };
			fixture.WriteProjectFile("Assets/Newer.irradiance", source);
			const Result<ImportResult> newer = ImportVolume(fixture, "Assets/Newer.irradiance");
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::ImportFailed);

			fixture.WriteProjectText("Assets/Text.irradiance", "not a volume");
			const Result<ImportResult> text = ImportVolume(fixture, "Assets/Text.irradiance");
			REQUIRE_FALSE(text.has_value());
			CHECK(text.error().GetCode() == ErrorCode::ImportFailed);
		}
	}

}
