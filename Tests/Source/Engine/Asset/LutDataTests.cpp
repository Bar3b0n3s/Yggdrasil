#include "TestsPCH.h"

#include "Engine/Asset/LutData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"
#include "Support/TestData.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

// Colour-grading LUTs (M26, C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decision 15, stream X-B): the format version 1,
// the `.cube` parser, the built-in identity LUT and the fuzz target. The artifact wrappers, the loader's registration and the
// built-in names are the contract's.

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

		// The little-endian bytes of `value`.
		void AppendU32(Buffer& bytes, uint32_t value)
		{
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
		}

		void AppendF32(Buffer& bytes, float value)
		{
			AppendU32(bytes, std::bit_cast<uint32_t>(value));
		}

		// A valid table of `size` whose entry (r, g, b) is (r, g, b) / (size - 1) scaled by `gain`.
		LutData MakeRamp(uint32_t size, float gain)
		{
			LutData lut;
			lut.Size = size;
			for (uint32_t blue = 0; blue < size; ++blue)
			{
				for (uint32_t green = 0; green < size; ++green)
				{
					for (uint32_t red = 0; red < size; ++red)
						lut.Values.push_back(glm::vec3(static_cast<float>(red), static_cast<float>(green), static_cast<float>(blue)) * gain / static_cast<float>(size - 1));
				}
			}
			return lut;
		}

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

		TEST_CASE("LutData: a .cube file parses into its table, red fastest, and round-trips as a payload")
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

		TEST_CASE("LutData: a .cube file's domain, keywords in any order, comments, CRLF lines and a byte order mark are read")
		{
			// Keywords in any order before the data, comments and blank lines among the data, Windows line ends, a UTF-8 byte
			// order mark, tabs and leading spaces, exponents and negative values (graded outputs may leave [0, 1]).
			const std::string text = "\xEF\xBB\xBF# graded\r\nDOMAIN_MAX 2 2.5 4\r\nLUT_3D_SIZE 2\r\n  DOMAIN_MIN -1 0 0.5\r\nTITLE \"Odd\"\r\n"
									 "0 0 0\r\n1e-1\t0\t0\r\n\r\n# halfway\r\n0 0.5 0\r\n1 1 0\r\n0 0 1\r\n1 0 1\r\n-0.25 1 1\r\n1 1 1.5";
			const Result<LutData> lut = ParseCubeLut(text);
			REQUIRE_MESSAGE(lut.has_value(), lut.error().ToString());
			CHECK(lut->Size == 2);
			CHECK(lut->DomainMin == glm::vec3(-1.0f, 0.0f, 0.5f));
			CHECK(lut->DomainMax == glm::vec3(2.0f, 2.5f, 4.0f));
			REQUIRE(lut->Values.size() == 8);
			CHECK(lut->Values[1] == glm::vec3(0.1f, 0.0f, 0.0f));
			CHECK(lut->Values[6] == glm::vec3(-0.25f, 1.0f, 1.0f));
			CHECK(lut->Values[7] == glm::vec3(1.0f, 1.0f, 1.5f));
			CHECK(ValidateLutData(*lut).has_value());
		}

		// The W26.1 integration's addition (Docs/Decisions/0037-m26-contract.md, the lead's decision on X-B's open point): DaVinci
		// Resolve writes the domain as LUT_3D_INPUT_RANGE min max, the uniform form of DOMAIN_MIN and DOMAIN_MAX, so its exports
		// import; a file holds either form, never both.
		TEST_CASE("LutData: LUT_3D_INPUT_RANGE sets a uniform domain, alone and never beside DOMAIN_MIN or DOMAIN_MAX")
		{
			const std::string resolve = "TITLE \"Resolve\"\nLUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE -0.5 2\n" + std::string(SwapCube.substr(SwapCube.find("0 0 0")));
			const Result<LutData> lut = ParseCubeLut(resolve);
			REQUIRE_MESSAGE(lut.has_value(), lut.error().ToString());
			CHECK(lut->DomainMin == glm::vec3(-0.5f));
			CHECK(lut->DomainMax == glm::vec3(2.0f));
			REQUIRE(lut->Values.size() == 8);
			CHECK(lut->Values[1] == glm::vec3(0.0f, 0.0f, 1.0f));

			struct Bad
			{
				std::string_view Text{};
				uint32_t Line = 0;
			};
			const std::array<Bad, 6> cases = { {
				{ "LUT_3D_INPUT_RANGE 0 1\nDOMAIN_MIN 0 0 0\n", 2 }, // the domain set twice, either way round
				{ "DOMAIN_MAX 1 1 1\nLUT_3D_INPUT_RANGE 0 1\n", 2 },
				{ "LUT_3D_INPUT_RANGE 0 1\nLUT_3D_INPUT_RANGE 0 1\n", 2 }, // a repeated keyword
				{ "LUT_3D_INPUT_RANGE 0 1 1\n", 1 },                       // three values where two belong
				{ "LUT_3D_INPUT_RANGE 0\n", 1 },                           // one value
				{ "LUT_3D_INPUT_RANGE 0 nan\n", 1 },                       // not a finite number
			} };
			for (const Bad& bad : cases)
			{
				CAPTURE(std::string(bad.Text));
				const Result<LutData> refused = ParseCubeLut(bad.Text);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Parse);
				CHECK(refused.error().GetLocation().Line == bad.Line);
			}
			// An inverted range parses but is not a valid table, as an inverted DOMAIN_MIN and DOMAIN_MAX.
			const std::string inverted = "LUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE 1 0\n" + std::string(SwapCube.substr(SwapCube.find("0 0 0")));
			CHECK(ParseCubeLut(inverted).error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("LutData: 1D tables, short tables, unknown keywords and malformed numbers are refused, naming the line")
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

		TEST_CASE("LutData: a .cube file's structure is checked line by line, each refusal naming its line")
		{
			struct Bad
			{
				std::string_view Text{};
				ErrorCode Code = ErrorCode::Parse;
				uint32_t Line = 0;
			};
			const std::string nineEntries = std::string(SwapCube) + "1 1 1\n";
			const std::array<Bad, 14> cases = { {
				{ "", ErrorCode::Parse, 1 },                                         // no LUT_3D_SIZE at all
				{ "# only a comment\n", ErrorCode::Parse, 1 },                       // nor here
				{ "0 0 0\nLUT_3D_SIZE 2\n", ErrorCode::Parse, 1 },                   // a data line before the size
				{ "LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n", ErrorCode::Parse, 2 },           // a repeated keyword
				{ "TITLE \"a\"\nTITLE \"b\"\n", ErrorCode::Parse, 2 },               // a second title
				{ "LUT_3D_SIZE 1\n", ErrorCode::Parse, 1 },                          // below the smallest size
				{ "LUT_3D_SIZE 2.5\n", ErrorCode::Parse, 1 },                        // not a whole number
				{ "LUT_3D_SIZE\n", ErrorCode::Parse, 1 },                            // no value
				{ "DOMAIN_MIN 0 0\nLUT_3D_SIZE 2\n", ErrorCode::Parse, 1 },          // two values where three belong
				{ "LUT_3D_SIZE 2\n0 0 0 0\n", ErrorCode::Parse, 2 },                 // four values on a data line
				{ "LUT_3D_SIZE 2\n0 0 inf\n", ErrorCode::Parse, 2 },                 // not a finite number
				{ "LUT_3D_SIZE 2\n0 0 1e999\n", ErrorCode::Parse, 2 },               // out of range
				{ "LUT_3D_SIZE 2\n0 0 0\nDOMAIN_MIN 0 0 0\n", ErrorCode::Parse, 3 }, // a keyword after the data
				{ "LUT_3D_SIZE 2\nLUT_1D_SIZE 4\n", ErrorCode::Unsupported, 2 },     // a 1D table, wherever it is declared
			} };
			for (const Bad& bad : cases)
			{
				CAPTURE(std::string(bad.Text));
				const Result<LutData> lut = ParseCubeLut(bad.Text);
				REQUIRE_FALSE(lut.has_value());
				CHECK(lut.error().GetCode() == bad.Code);
				CHECK(lut.error().GetLocation().Line == bad.Line);
			}
			// A ninth entry in a table of eight is refused at its line: SwapCube's twelve lines, then the extra one.
			const Result<LutData> extra = ParseCubeLut(nineEntries);
			REQUIRE_FALSE(extra.has_value());
			CHECK(extra.error().GetCode() == ErrorCode::Parse);
			CHECK(extra.error().GetLocation().Line == 13);
			// A domain whose minimum is not below its maximum parses but is not a valid table.
			CHECK(ParseCubeLut("DOMAIN_MIN 0 1 0\nDOMAIN_MAX 1 1 1\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n").error().GetCode()
				== ErrorCode::Validation);
			// A file over the limit is refused before it is read.
			const std::string huge(MaxCubeLutBytes + 1, '#');
			CHECK(ParseCubeLut(huge).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("LutData: the payload is the documented little-endian layout")
		{
			LutData lut = MakeRamp(2, 1.0f);
			lut.DomainMin = glm::vec3(-0.5f, 0.0f, 0.25f);
			lut.DomainMax = glm::vec3(1.0f, 2.0f, 3.0f);
			lut.Values[3] = glm::vec3(0.125f, -2.0f, 7.5f);
			Buffer expected;
			AppendU32(expected, 2);
			for (const float value : { -0.5f, 0.0f, 0.25f, 1.0f, 2.0f, 3.0f })
				AppendF32(expected, value);
			for (const glm::vec3& value : lut.Values)
			{
				AppendF32(expected, value.x);
				AppendF32(expected, value.y);
				AppendF32(expected, value.z);
			}
			CHECK(SerializeLutPayload(lut) == expected);
			const Result<AssetRef<LutData>> loaded = LoadCookedLut(CookLut(lut, 1));
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->DomainMin == lut.DomainMin);
			CHECK((*loaded)->DomainMax == lut.DomainMax);
			CHECK((*loaded)->Values == lut.Values);
		}

		TEST_CASE("LutData: sizes, domains and entries outside the format's rules are refused")
		{
			CHECK(ValidateLutData(MakeRamp(2, 1.0f)).has_value());
			CHECK(ValidateLutData(MakeRamp(MaxLutSize, 1.0f)).has_value());
			LutData small = MakeRamp(2, 1.0f);
			small.Size = 1;
			small.Values.resize(1);
			CHECK(ValidateLutData(small).error().GetCode() == ErrorCode::Validation);
			LutData mismatched = MakeRamp(3, 1.0f);
			mismatched.Values.pop_back();
			CHECK(ValidateLutData(mismatched).error().GetCode() == ErrorCode::Validation);
			LutData flat = MakeRamp(2, 1.0f);
			flat.DomainMax.y = flat.DomainMin.y;
			CHECK(ValidateLutData(flat).error().GetCode() == ErrorCode::Validation);
			LutData infiniteDomain = MakeRamp(2, 1.0f);
			infiniteDomain.DomainMax.z = std::numeric_limits<float>::infinity();
			CHECK(ValidateLutData(infiniteDomain).error().GetCode() == ErrorCode::Validation);
			LutData notANumber = MakeRamp(2, 1.0f);
			notANumber.Values[5].y = std::numeric_limits<float>::quiet_NaN();
			CHECK(ValidateLutData(notANumber).error().GetCode() == ErrorCode::Validation);

			// The reader checks the declared size before it allocates, the bytes left, and that nothing trails the table.
			const Buffer payload = SerializeLutPayload(MakeRamp(2, 1.0f));
			Buffer oversized = payload;
			oversized[0] = static_cast<std::byte>(MaxLutSize + 1);
			CHECK(DeserializeLutPayload(oversized).error().GetCode() == ErrorCode::Parse);
			Buffer giant = payload;
			std::memset(giant.data(), 0xFF, 4);
			CHECK(DeserializeLutPayload(giant).error().GetCode() == ErrorCode::Parse);
			Buffer truncated = payload;
			truncated.pop_back();
			CHECK(DeserializeLutPayload(truncated).error().GetCode() == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(DeserializeLutPayload(trailing).error().GetCode() == ErrorCode::Parse);
			// The first entry's red, after the size and the domain, as a NaN.
			Buffer invalid = payload;
			const float nan = std::numeric_limits<float>::quiet_NaN();
			std::memcpy(invalid.data() + 4 + 24, &nan, sizeof(nan));
			CHECK(DeserializeLutPayload(invalid).error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("LutData: the identity LUT maps every entry to its own coordinates")
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

		TEST_CASE("LutData: the built-in identity LUT is a procedural asset, identical every time")
		{
			const Result<AssetRef<Asset>> created = CreateProceduralBuiltinAsset(BuiltinAssetHandles::IdentityLut);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			const AssetRef<LutData> lut = AssetCast<LutData>(*created);
			REQUIRE(lut != nullptr);
			CHECK(lut->Values == GenerateBuiltinLut(BuiltinLut::Identity).Values);
			// Every entry is the exact quotient of its coordinates: (r, g, b) / 32.
			for (uint32_t index = 0; index < lut->Values.size(); index += 997)
			{
				CAPTURE(index);
				const glm::vec3 coordinates(static_cast<float>(index % 33), static_cast<float>((index / 33) % 33), static_cast<float>(index / (33 * 33)));
				CHECK(lut->Values[index] == coordinates / 32.0f);
			}
			CHECK(SerializeLutPayload(*lut) == SerializeLutPayload(GenerateBuiltinLut(BuiltinLut::Identity)));
		}

		TEST_CASE("LutData: the committed .cube fixtures parse")
		{
			// Tests/Data/Assets/Luts/ (MakeLutFixtures.py): the FormatFixtures case's .cube fixture.
			const Result<std::string> text = FileSystem::ReadText(Test::GetTestDataPath("Assets/Luts/Warm.cube"));
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const Result<LutData> lut = ParseCubeLut(*text);
			REQUIRE_MESSAGE(lut.has_value(), lut.error().ToString());
			CHECK(lut->Size == 9);
			CHECK(lut->Values.size() == 9u * 9u * 9u);
			// Red lifted (0.1 + 0.9 r), green kept, blue lowered (0.85 b): entry (8, 4, 8) is (1, 0.5, 0.85).
			CHECK(lut->Values[8 + 9 * (4 + 9 * 8)] == glm::vec3(1.0f, 0.5f, 0.85f));
			CHECK(lut->Values[0] == glm::vec3(0.1f, 0.0f, 0.0f));
		}

		TEST_CASE("LutData: 10,000 seeded mutations of a .cube file and of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			const Result<LutData> lut = ParseCubeLut(SwapCube);
			REQUIRE(lut.has_value());
			const Buffer payload = SerializeLutPayload(*lut);
			const std::string text(SwapCube);
			const Test::FuzzRun run = Test::RunFuzzCase(0x1D7, 10000, [&payload, &text](Random& random, uint64_t index)
			{
				// Even iterations mutate the text (token-level edits and byte mutations), odd ones the payload; whatever either
				// reader accepts is a valid table.
				if (index % 2 == 0)
				{
					std::string mutated = text;
					Test::MutateText(mutated, random);
					const Result<LutData> parsed = ParseCubeLut(mutated);
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
