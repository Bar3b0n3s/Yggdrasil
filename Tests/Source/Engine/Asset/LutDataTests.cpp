#include "TestsPCH.h"

#include "Engine/Asset/LutData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Colour-grading LUTs (M26, C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decision 15, stream X-B): the format version 1,
// the `.cube` parser, the built-in identity LUT and the fuzz target. The artifact wrappers, the loader's registration and the
// built-in names are the contract's (live); the rest is skipped until stream X-B implements it.

namespace Engine {

	namespace {

		// A 2^3 table that swaps red and blue, with a title, a comment and a blank line.
		constexpr std::string_view SwapCube = R"(TITLE "Swap red and blue"
# a 2 x 2 x 2 table, red fastest
LUT_3D_SIZE 2

0 0 0
0 0 1
0 1 0
0 1 1
1 0 0
1 0 1
1 1 0
1 1 1
)";

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("LutData: the artifact names its type and format version, its loader is registered and the identity LUT is built in")
		{
			CHECK(LutData::StaticType == AssetType::Lut);
			CHECK(LutData::FormatVersion == 1);
			CHECK(GetCookedFormatVersion(AssetType::Lut) == LutData::FormatVersion);
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			REQUIRE(loaders.Find(AssetType::Lut) != nullptr);
			const std::byte payload[1] = {};
			const Result<AssetRef<LutData>> newer = LoadCookedLut(WriteCookedArtifact(AssetType::Lut, LutData::FormatVersion + 1, 1, payload));
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
			// ADR 0028's phase-3 ledger: the built-in LUTs' block from 0x0281, the identity LUT first.
			CHECK(BuiltinLutToString(BuiltinLut::Identity) == "Identity");
			CHECK(BuiltinLutToString(static_cast<BuiltinLut>(200)) == "Unknown");
			CHECK(BuiltinAssetHandles::IdentityLut.GetValue() == 0x0281);
			CHECK(IsBuiltinAssetHandle(BuiltinAssetHandles::IdentityLut));
		}

		TEST_CASE("LutData: a .cube file parses into its table, red fastest, and round-trips as a payload" * doctest::skip(true))
		{
			const Result<LutData> lut = ParseCubeLut(SwapCube);
			REQUIRE_MESSAGE(lut.has_value(), lut.error().ToString());
			CHECK(lut->Size == 2);
			CHECK(lut->DomainMin == glm::vec3(0.0f));
			CHECK(lut->DomainMax == glm::vec3(1.0f));
			REQUIRE(lut->Values.size() == 8);
			// Entry (r, g, b) = (1, 0, 0) is Values[1], which the file maps to (0, 0, 1).
			CHECK(lut->Values[1] == glm::vec3(0.0f, 0.0f, 1.0f));
			CHECK(lut->Values[4] == glm::vec3(1.0f, 0.0f, 0.0f));
			const Buffer payload = SerializeLutPayload(*lut);
			REQUIRE(payload.size() == 4 + 12 + 12 + 8 * 12);
			const Result<LutData> read = DeserializeLutPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Values == lut->Values);
		}

		TEST_CASE("LutData: 1D tables, short tables, unknown keywords and malformed numbers are refused, naming the line" * doctest::skip(true))
		{
			CHECK(ParseCubeLut("LUT_1D_SIZE 4\n0 0 0\n").error().GetCode() == ErrorCode::Unsupported);
			const Result<LutData> shortTable = ParseCubeLut("LUT_3D_SIZE 2\n0 0 0\n");
			REQUIRE_FALSE(shortTable.has_value());
			CHECK(shortTable.error().GetCode() == ErrorCode::Parse);
			const Result<LutData> keyword = ParseCubeLut("LUT_3D_SIZE 2\nGAMMA 2.2\n");
			REQUIRE_FALSE(keyword.has_value());
			CHECK(keyword.error().GetCode() == ErrorCode::Parse);
			CHECK(keyword.error().GetLocation().Line == 2);
			CHECK(ParseCubeLut("LUT_3D_SIZE 65\n").error().GetCode() == ErrorCode::Parse);
			CHECK(ParseCubeLut("LUT_3D_SIZE 2\n0 0 zero\n").error().GetCode() == ErrorCode::Parse);
		}

		TEST_CASE("LutData: the identity LUT maps every entry to its own coordinates" * doctest::skip(true))
		{
			const LutData identity = GenerateBuiltinLut(BuiltinLut::Identity);
			REQUIRE(ValidateLutData(identity).has_value());
			CHECK(identity.Size == 33);
			REQUIRE(identity.Values.size() == 33u * 33u * 33u);
			CHECK(identity.Values[0] == glm::vec3(0.0f));
			CHECK(identity.Values[1] == glm::vec3(1.0f / 32.0f, 0.0f, 0.0f));
			CHECK(identity.Values[33] == glm::vec3(0.0f, 1.0f / 32.0f, 0.0f));
			CHECK(identity.Values.back() == glm::vec3(1.0f));
		}

		TEST_CASE("LutData: 10,000 seeded mutations of a .cube file and of a payload never crash" * doctest::test_suite(Test::FuzzSuite) * doctest::skip(true))
		{
			const Result<LutData> lut = ParseCubeLut(SwapCube);
			REQUIRE(lut.has_value());
			const Buffer payload = SerializeLutPayload(*lut);
			const std::string text(SwapCube);
			const Test::FuzzRun run = Test::RunFuzzCase(0x1D7, 10000, [&payload, &text](Random& random, uint64_t index)
			{
				// Even iterations mutate the text, odd ones the payload; whatever either reader accepts is a valid table.
				if (index % 2 == 0)
				{
					Buffer bytes(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size());
					Test::MutateBytes(bytes, random);
					const Result<LutData> parsed = ParseCubeLut(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
					if (!parsed.has_value())
						return false;
					CHECK(ValidateLutData(*parsed).has_value());
					return true;
				}
				Buffer mutated = payload;
				Test::MutateBytes(mutated, random);
				const Result<LutData> read = DeserializeLutPayload(mutated);
				if (!read.has_value())
					return false;
				CHECK(ValidateLutData(*read).has_value());
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}
	}

}
