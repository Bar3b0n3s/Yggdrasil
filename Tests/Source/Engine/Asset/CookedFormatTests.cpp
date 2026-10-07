#include "TestsPCH.h"

#include "Engine/Asset/CookedFormat.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"

namespace Engine {

	namespace {

		Buffer MakePayload(size_t size)
		{
			Buffer payload(size);
			for (size_t index = 0; index < size; ++index)
				payload[index] = static_cast<std::byte>((index * 31 + 7) & 0xFF);
			return payload;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("CookedFormat: the 32-byte header round-trips")
		{
			const Buffer payload = MakePayload(100);
			const Buffer artifact = WriteCookedArtifact(AssetType::Texture, 3, 7, payload);
			REQUIRE(artifact.size() == CookedHeader::Size + payload.size());
			CHECK(std::memcmp(artifact.data(), CookedHeader::Magic.data(), 4) == 0);
			Result<CookedArtifactView> view = ReadCookedArtifact(artifact);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			CHECK(view->Header.FormatVersion == 3);
			CHECK(view->Header.Type == AssetType::Texture);
			CHECK(view->Header.ImporterVersion == 7);
			CHECK(view->Header.Flags == 0);
			CHECK(view->Header.PayloadSize == payload.size());
			CHECK(view->Header.PayloadHash == XXH64(payload));
			CHECK(std::ranges::equal(view->Payload, payload));
			CHECK(WriteCookedArtifact(AssetType::Texture, 3, 7, payload) == artifact);

			Result<CookedArtifactView> typed = ReadCookedArtifact(artifact, AssetType::Texture, 3);
			CHECK(typed.has_value());
			Result<CookedArtifactView> wrongType = ReadCookedArtifact(artifact, AssetType::Mesh, 3);
			REQUIRE_FALSE(wrongType.has_value());
			CHECK(wrongType.error().GetCode() == ErrorCode::Validation);
			Result<CookedArtifactView> newer = ReadCookedArtifact(artifact, AssetType::Texture, 2);
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("CookedFormat: a flipped payload byte is a hash mismatch, a wrong size or magic is Parse")
		{
			Buffer artifact = WriteCookedArtifact(AssetType::Mesh, 1, 1, MakePayload(64));
			Buffer flipped = artifact;
			flipped[CookedHeader::Size + 10] ^= std::byte{ 0x01 };
			Result<CookedArtifactView> corrupted = ReadCookedArtifact(flipped);
			REQUIRE_FALSE(corrupted.has_value());
			CHECK(corrupted.error().GetCode() == ErrorCode::Validation);

			const auto checkParseError = [](std::span<const std::byte> bytes)
			{
				const Result<CookedArtifactView> read = ReadCookedArtifact(bytes);
				REQUIRE_FALSE(read.has_value());
				CHECK(read.error().GetCode() == ErrorCode::Parse);
			};
			Buffer longer = artifact;
			longer.push_back(std::byte{ 0 });
			checkParseError(longer);
			Buffer badMagic = artifact;
			badMagic[0] = std::byte{ 'X' };
			checkParseError(badMagic);
			checkParseError(std::span<const std::byte>(artifact).first(31));
		}

		TEST_CASE("CookedFormat: 10,000 seeded mutations never crash and always return Result")
		{
			const Buffer original = WriteCookedArtifact(AssetType::Font, 1, 1, MakePayload(256));
			Random random(0xC00CED);
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				Buffer mutated = original;
				const int64_t flips = random.RangeInt(1, 8);
				for (int64_t flip = 0; flip < flips; ++flip)
				{
					const size_t index = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1));
					mutated[index] = static_cast<std::byte>(random.NextU32() & 0xFF);
				}
				if (random.NextBool(0.25))
					mutated.resize(static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()))));
				// Any outcome is fine as long as it is a value: a valid view only when the bytes are still consistent.
				const Result<CookedArtifactView> read = ReadCookedArtifact(mutated);
				if (read.has_value())
					CHECK(read->Header.PayloadHash == XXH64(read->Payload));
			}
		}
	}

}
