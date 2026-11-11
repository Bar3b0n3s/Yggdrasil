#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/HeightfieldImporter.h"

#include "Engine/Core/FileSystem.h"
#include "Support/TestData.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The heightfield importer and the heightmap conversion (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 20,
// stream T-A): ADR 0028's phase-3 ledger's "import of 16-bit PNG or RAW, at most 1025 squared samples". The extension is the
// contract's (live); the conversion is skipped until stream T-A implements it, with the fixture its generator writes
// (Tests/Data/Assets/Heightmaps/Ramp16.png, a 17 x 17 16-bit grey ramp, sample (x, z) = x * 4000 + z, at most 64,016 so
// every sample fits 16 bits, from Tests/Data/Generate/MakeHeightmapFixtures.py).

namespace Engine {

	namespace {

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

		TEST_CASE("HeightfieldImporter: a 16-bit PNG and a RAW heightmap convert to the same heightfield bit for bit" * doctest::skip(true))
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

		TEST_CASE("HeightfieldImporter: an 8-bit PNG, a non-square RAW and a heightmap over 1025 samples a side are refused" * doctest::skip(true))
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
	}

}
