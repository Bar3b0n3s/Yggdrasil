#include "TestsPCH.h"

#include "Engine/Asset/HeightfieldData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"

#include <cstddef>
#include <cstdint>
#include <span>

// Heightfields (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 20 and 21, stream T-A): the format version
// 1, its source file and the fuzz target.

namespace Engine {

	namespace {

		// A 5 x 5 ramp rising along +X: sample (x, z) = x * 16384 (the last column 65535).
		HeightfieldData MakeRamp()
		{
			HeightfieldData heightfield = MakeFlatHeightfield(5);
			for (uint32_t z = 0; z < 5; ++z)
			{
				for (uint32_t x = 0; x < 5; ++x)
					heightfield.Heights[x + 5 * z] = static_cast<uint16_t>(x == 4 ? MaxHeightfieldSample : x * 16384);
			}
			return heightfield;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("HeightfieldData: the artifact names its type and format version, its loader is registered and a flat one is level")
		{
			CHECK(HeightfieldData::StaticType == AssetType::Heightfield);
			CHECK(HeightfieldData::FormatVersion == 1);
			CHECK(GetCookedFormatVersion(AssetType::Heightfield) == HeightfieldData::FormatVersion);
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			REQUIRE(loaders.Find(AssetType::Heightfield) != nullptr);
			const std::byte payload[1] = {};
			const Result<AssetRef<HeightfieldData>> newer =
				LoadCookedHeightfield(WriteCookedArtifact(AssetType::Heightfield, HeightfieldData::FormatVersion + 1, 1, payload));
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
			// ADR 0028's phase-3 ledger: at most 1025 x 1025 samples.
			CHECK(MaxHeightfieldResolution == 1025);
			const HeightfieldData flat = MakeFlatHeightfield(129, 300);
			CHECK(flat.Resolution == 129);
			REQUIRE(flat.Heights.size() == 129u * 129u);
			CHECK(flat.Heights.front() == 300);
			CHECK(flat.Heights.back() == 300);
		}

		TEST_CASE("HeightfieldData: a payload and a source file round-trip the samples byte for byte")
		{
			const HeightfieldData ramp = MakeRamp();
			REQUIRE(ValidateHeightfieldData(ramp).has_value());
			const Buffer payload = SerializeHeightfieldPayload(ramp);
			REQUIRE(payload.size() == 4 + 25 * 2);
			// Little-endian: sample (1, 0) = 16384 = 0x4000 at bytes 6 and 7.
			CHECK(payload[6] == std::byte{ 0x00 });
			CHECK(payload[7] == std::byte{ 0x40 });
			const Result<HeightfieldData> read = DeserializeHeightfieldPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Heights == ramp.Heights);
			const Buffer source = WriteHeightfieldSource(ramp);
			REQUIRE(source.size() == 8 + payload.size());
			const Result<HeightfieldData> fromSource = ReadHeightfieldSource(source);
			REQUIRE_MESSAGE(fromSource.has_value(), fromSource.error().ToString());
			CHECK(fromSource->Heights == ramp.Heights);
			HeightfieldData tooLarge;
			tooLarge.Resolution = MaxHeightfieldResolution + 1;
			tooLarge.Heights.assign(static_cast<size_t>(tooLarge.Resolution) * tooLarge.Resolution, 0);
			CHECK(ValidateHeightfieldData(tooLarge).error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("HeightfieldData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			const Buffer payload = SerializeHeightfieldPayload(MakeRamp());
			REQUIRE(DeserializeHeightfieldPayload(payload).has_value());
			const Test::FuzzRun run = Test::RunFuzzCase(0x4EF1, 10000, [&payload](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = payload;
				Test::MutateBytes(mutated, random);
				const Result<HeightfieldData> read = DeserializeHeightfieldPayload(mutated);
				if (!read.has_value())
					return false;
				CHECK(ValidateHeightfieldData(*read).has_value());
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}

		// The readers never trust what they read: a source names its format and version first, and a payload holds exactly its
		// resolution's samples (decision 20).
		TEST_CASE("HeightfieldData: another magic or version, a resolution out of range, a truncated payload and trailing bytes are refused")
		{
			const Buffer payload = SerializeHeightfieldPayload(MakeRamp());
			const Buffer source = WriteHeightfieldSource(MakeRamp());
			REQUIRE(source.size() == 8 + payload.size());
			CHECK(source[0] == std::byte{ 'E' });
			CHECK(source[3] == std::byte{ 'D' });
			CHECK(source[4] == std::byte{ 1 });

			Buffer magic = source;
			magic[0] = std::byte{ 'X' };
			CHECK(ReadHeightfieldSource(magic).error().GetCode() == ErrorCode::Parse);
			CHECK(ReadHeightfieldSource(std::span(source).first(3)).error().GetCode() == ErrorCode::Parse);
			Buffer newer = source;
			newer[4] = std::byte{ 2 };
			CHECK(ReadHeightfieldSource(newer).error().GetCode() == ErrorCode::UnsupportedVersion);
			CHECK(ReadHeightfieldSource(std::span(source).first(6)).error().GetCode() == ErrorCode::Parse);

			Buffer truncated = payload;
			truncated.pop_back();
			CHECK(DeserializeHeightfieldPayload(truncated).error().GetCode() == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(DeserializeHeightfieldPayload(trailing).error().GetCode() == ErrorCode::Parse);
			for (const uint32_t resolution : { 0u, 2u, MaxHeightfieldResolution + 1, 0xFFFFFFFFu })
			{
				CAPTURE(resolution);
				Buffer sized = payload;
				for (size_t index = 0; index < 4; ++index)
					sized[index] = static_cast<std::byte>((resolution >> (8 * index)) & 0xFFU);
				CHECK(DeserializeHeightfieldPayload(sized).error().GetCode() == ErrorCode::Parse);
			}

			HeightfieldData short3 = MakeFlatHeightfield(3);
			short3.Heights.pop_back();
			CHECK(ValidateHeightfieldData(short3).error().GetCode() == ErrorCode::Validation);
			HeightfieldData small;
			small.Resolution = MinHeightfieldResolution - 1;
			small.Heights.assign(4, 0);
			CHECK(ValidateHeightfieldData(small).error().GetCode() == ErrorCode::Validation);
			CHECK(ValidateHeightfieldData(MakeFlatHeightfield(MaxHeightfieldResolution)).has_value());

			// A cooked artifact reads back to the same samples.
			const Result<AssetRef<HeightfieldData>> cooked = LoadCookedHeightfield(CookHeightfield(MakeRamp(), 1));
			REQUIRE_MESSAGE(cooked.has_value(), cooked.error().ToString());
			CHECK((*cooked)->Heights == MakeRamp().Heights);
		}
	}

}
