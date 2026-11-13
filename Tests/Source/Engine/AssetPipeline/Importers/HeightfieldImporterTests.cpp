#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/HeightfieldImporter.h"

#include "Engine/Core/FileSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The heightfield importer and the heightmap conversion (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 20,
// stream T-A): ADR 0028's phase-3 ledger's "import of 16-bit PNG or RAW, at most 1025 squared samples", with the fixture its
// generator writes (Tests/Data/Assets/Heightmaps/Ramp16.png, a 17 x 17 16-bit grey ramp, sample (x, z) = x * 4000 + z, at most
// 64,016 so every sample fits 16 bits, from Tests/Data/Generate/MakeHeightmapFixtures.py).

namespace Engine {

	namespace {

		Result<ImportResult> ImportHeightfield(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x4EF1000000000001ull);
			metadata.Type = AssetType::Heightfield;
			metadata.Importer = std::string(HeightfieldImporter::Id);
			metadata.ImporterVersion = HeightfieldImporter::Version;
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
			Result<ImportResult> result = HeightfieldImporter().Import(context, metadata);
			// The importer reads nothing but its source.
			CHECK(context.GetDependencyReads().empty());
			return result;
		}

		// The fixture ramp as RAW bytes: little-endian uint16, row 0 first.
		Buffer MakeRampRaw()
		{
			Buffer raw;
			for (uint32_t z = 0; z < 17; ++z)
			{
				for (uint32_t x = 0; x < 17; ++x)
				{
					const uint32_t sample = x * 4000 + z;
					raw.push_back(static_cast<std::byte>(sample & 0xFFU));
					raw.push_back(static_cast<std::byte>(sample >> 8U));
				}
			}
			return raw;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("HeightfieldImporter: the importer reads .heightfield sources into Heightfield assets")
		{
			const HeightfieldImporter importer;
			CHECK(importer.GetId() == "Heightfield");
			CHECK(importer.GetVersion() == 1);
			CHECK(importer.GetMainType() == AssetType::Heightfield);
			REQUIRE(importer.GetExtensions().size() == 1);
			CHECK(importer.GetExtensions()[0] == ".heightfield");
		}

		TEST_CASE("HeightfieldImporter: a 16-bit PNG and a RAW heightmap convert to the same heightfield bit for bit")
		{
			const Result<Buffer> png = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Heightmaps/Ramp16.png"));
			REQUIRE_MESSAGE(png.has_value(), png.error().ToString());
			const Result<HeightfieldData> fromPng = ConvertHeightmap(*png, HeightmapSourceKind::Png16);
			REQUIRE_MESSAGE(fromPng.has_value(), fromPng.error().ToString());
			const Result<HeightfieldData> fromRaw = ConvertHeightmap(MakeRampRaw(), HeightmapSourceKind::Raw16);
			REQUIRE_MESSAGE(fromRaw.has_value(), fromRaw.error().ToString());
			CHECK(fromPng->Resolution == 17);
			CHECK(fromRaw->Resolution == 17);
			CHECK(fromPng->Heights == fromRaw->Heights);
			CHECK(fromRaw->Heights[3 + 17 * 2] == 3 * 4000 + 2);
		}

		TEST_CASE("HeightfieldImporter: an 8-bit PNG, a non-square RAW and a heightmap over 1025 samples a side are refused")
		{
			Buffer raw = MakeRampRaw();
			raw.pop_back();
			CHECK(ConvertHeightmap(raw, HeightmapSourceKind::Raw16).error().GetCode() == ErrorCode::InvalidArgument);
			const Buffer large(static_cast<size_t>(1026) * 1026 * 2, std::byte{ 0 });
			CHECK(ConvertHeightmap(large, HeightmapSourceKind::Raw16).error().GetCode() == ErrorCode::InvalidArgument);
			const Buffer tiny(static_cast<size_t>(2) * 2 * 2, std::byte{ 0 });
			CHECK(ConvertHeightmap(tiny, HeightmapSourceKind::Raw16).error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Buffer> eightBit = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Textures/Rgba.png"));
			REQUIRE(eightBit.has_value());
			CHECK(ConvertHeightmap(*eightBit, HeightmapSourceKind::Png16).error().GetCode() == ErrorCode::InvalidArgument);
		}

		// The rest of the PNG path: a file that is not a PNG or that stb_image cannot read is Parse, a non-square 16-bit PNG
		// InvalidArgument; a RAW at the largest and the smallest side converts.
		TEST_CASE("HeightfieldImporter: a heightmap that is not a PNG or not square is refused, and RAWs of 3 and 1025 samples a side convert")
		{
			CHECK(ConvertHeightmap(MakeRampRaw(), HeightmapSourceKind::Png16).error().GetCode() == ErrorCode::Parse);
			const Result<Buffer> png = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Heightmaps/Ramp16.png"));
			REQUIRE(png.has_value());
			const Buffer truncated(png->begin(), png->begin() + 40);
			CHECK(ConvertHeightmap(truncated, HeightmapSourceKind::Png16).error().GetCode() == ErrorCode::Parse);
			// The IHDR's height (bytes 20 to 23, big-endian) set to 16 makes a 17 x 16 image: not square.
			Buffer wide = *png;
			wide[23] = std::byte{ 16 };
			const Result<HeightfieldData> notSquare = ConvertHeightmap(wide, HeightmapSourceKind::Png16);
			REQUIRE_FALSE(notSquare.has_value());
			CHECK(notSquare.error().GetCode() == ErrorCode::InvalidArgument);

			const Result<HeightfieldData> smallest = ConvertHeightmap(Buffer(static_cast<size_t>(3) * 3 * 2, std::byte{ 0xFF }), HeightmapSourceKind::Raw16);
			REQUIRE_MESSAGE(smallest.has_value(), smallest.error().ToString());
			CHECK(smallest->Resolution == 3);
			CHECK(smallest->Heights.front() == MaxHeightfieldSample);
			const Result<HeightfieldData> largest = ConvertHeightmap(Buffer(static_cast<size_t>(1025) * 1025 * 2, std::byte{ 0 }), HeightmapSourceKind::Raw16);
			REQUIRE_MESSAGE(largest.has_value(), largest.error().ToString());
			CHECK(largest->Resolution == MaxHeightfieldResolution);
			CHECK(ValidateHeightfieldData(*largest).has_value());
		}

		// Decision 20: the importer reads a .heightfield source (ReadHeightfieldSource) into one Heightfield artifact, cooked
		// unchanged, with no dependency; a source that fails the reader fails the import, located in its file.
		TEST_CASE("HeightfieldImporter: a source imports as one Heightfield with no dependency, and a bad source fails the import")
		{
			Test::AssetTestFixture fixture;
			const Result<HeightfieldData> ramp = ConvertHeightmap(MakeRampRaw(), HeightmapSourceKind::Raw16);
			REQUIRE_MESSAGE(ramp.has_value(), ramp.error().ToString());
			const Buffer source = WriteHeightfieldSource(*ramp);
			fixture.WriteProjectFile("Assets/Ramp.heightfield", source);
			const Result<ImportResult> imported = ImportHeightfield(fixture, "Assets/Ramp.heightfield");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::Heightfield);
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x4EF1000000000001ull));
			CHECK(imported->Dependencies.empty());
			CHECK(imported->Diagnostics.empty());
			const Result<AssetRef<HeightfieldData>> heightfield = LoadCookedHeightfield(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(heightfield.has_value(), heightfield.error().ToString());
			CHECK((*heightfield)->Resolution == 17);
			CHECK((*heightfield)->Heights[3 + 17 * 2] == 3 * 4000 + 2);
			// A pure function of the source and the version: a second import gives identical bytes.
			const Result<ImportResult> again = ImportHeightfield(fixture, "Assets/Ramp.heightfield");
			REQUIRE(again.has_value());
			CHECK(again->Artifacts.front().Cooked == imported->Artifacts.front().Cooked);

			Buffer newer = source;
			newer[4] = std::byte{ 2 };
			fixture.WriteProjectFile("Assets/Newer.heightfield", newer);
			const Result<ImportResult> refused = ImportHeightfield(fixture, "Assets/Newer.heightfield");
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(refused.error().GetLocation().File.find("Newer.heightfield") != std::string::npos);
		}
	}

}
