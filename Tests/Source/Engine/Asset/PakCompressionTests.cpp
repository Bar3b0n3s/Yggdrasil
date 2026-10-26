#include "TestsPCH.h"

#include "Engine/Asset/PakCompression.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

// The entry codec of pak version 2 (Docs/Decisions/0030-m19-contract.md decision 17, B-PAKZ, stream G).

namespace Engine {

	namespace {

		// A payload deflate shrinks well: eight copies of the cooked cube mesh.
		Buffer MakeCompressiblePayload()
		{
			const Buffer cube = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Cube), 1);
			Buffer payload;
			for (int copy = 0; copy < 8; ++copy)
				payload.insert(payload.end(), cube.begin(), cube.end());
			return payload;
		}

		void CheckRefused(const Result<Buffer>& inflated)
		{
			REQUIRE_FALSE(inflated.has_value());
			CHECK(inflated.error().GetCode() == ErrorCode::Validation);
		}

	}

	TEST_SUITE("Asset")
	{
		// Decision 17: an entry deflates to a zlib stream, the same bytes every time, which inflates back to exactly the
		// entry's bytes; a stream that does not inflate to exactly the size asked for is Validation, never a crash.
		TEST_CASE("PakCompression: deflate then inflate restores the bytes, and the stream is deterministic")
		{
			const Buffer payload = MakeCompressiblePayload();
			const Result<Buffer> deflated = DeflatePakEntry(payload);
			REQUIRE_MESSAGE(deflated.has_value(), deflated.error().ToString());
			CHECK(deflated->size() < payload.size());
			// A zlib stream (RFC 1950): deflate with a 32 KiB window, header byte 0x78.
			REQUIRE_FALSE(deflated->empty());
			CHECK(deflated->front() == std::byte{ 0x78 });
			const Result<Buffer> again = DeflatePakEntry(payload);
			REQUIRE(again.has_value());
			CHECK(*again == *deflated);

			const Result<Buffer> inflated = InflatePakEntry(*deflated, payload.size());
			REQUIRE_MESSAGE(inflated.has_value(), inflated.error().ToString());
			CHECK(*inflated == payload);

			// One byte fewer or more than the stream holds; half the stream; bytes that are no zlib stream; a size above
			// what stb's int lengths take.
			CheckRefused(InflatePakEntry(*deflated, payload.size() - 1));
			CheckRefused(InflatePakEntry(*deflated, payload.size() + 1));
			CheckRefused(InflatePakEntry(std::span<const std::byte>(*deflated).first(deflated->size() / 2), payload.size()));
			CheckRefused(InflatePakEntry(AsBytes("not a zlib stream"), 16));
			CheckRefused(InflatePakEntry(*deflated, MaxPakCompressedEntrySize + 1));
			// More bytes than any stream of its length holds (1032 per stream byte), refused before the output is allocated;
			// an empty stream.
			CheckRefused(InflatePakEntry(*deflated, deflated->size() * 1032 + 1));
			CheckRefused(InflatePakEntry({}, 0));

			// An empty entry deflates to a stream that inflates to nothing, and to nothing else.
			const Result<Buffer> empty = DeflatePakEntry({});
			REQUIRE(empty.has_value());
			const Result<Buffer> nothing = InflatePakEntry(*empty, 0);
			REQUIRE_MESSAGE(nothing.has_value(), nothing.error().ToString());
			CHECK(nothing->empty());
			CheckRefused(InflatePakEntry(*empty, 1));

			// Bytes deflate cannot shrink (seeded noise) still give a stream that inflates back to them.
			Buffer noise(4096);
			Random random(0x5EED);
			for (std::byte& value : noise)
				value = static_cast<std::byte>(random.NextU32() & 0xffu);
			const Result<Buffer> stored = DeflatePakEntry(noise);
			REQUIRE(stored.has_value());
			CHECK(stored->size() >= noise.size());
			const Result<Buffer> restored = InflatePakEntry(*stored, noise.size());
			REQUIRE_MESSAGE(restored.has_value(), restored.error().ToString());
			CHECK(*restored == noise);
		}

		// Decision 17's fuzz target (Tests/Data/Fuzz/Targets.json, reader PakInflate): the Runtime inflates entries of a pak
		// it did not write, so any byte sequence, claimed to inflate to any size, is a value or a Validation error.
		TEST_CASE("PakCompression: 10,000 seeded mutations of a deflate stream never crash and always return Result" * doctest::test_suite(Test::FuzzSuite))
		{
			const Buffer payload = MakeCompressiblePayload();
			const Result<Buffer> deflated = DeflatePakEntry(payload);
			REQUIRE(deflated.has_value());
			const Test::FuzzRun run = Test::RunFuzzCase(0x1F1A7, 10000, [&payload, &deflated](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = *deflated;
				Test::MutateBytes(mutated, random);
				// The size the TOC claims: the real one half of the time, any other the rest.
				const uint64_t size = random.NextBool() ? payload.size() : static_cast<uint64_t>(random.RangeInt(0, static_cast<int64_t>(payload.size()) * 2));
				const Result<Buffer> inflated = InflatePakEntry(mutated, size);
				if (!inflated.has_value())
				{
					CHECK(inflated.error().GetCode() == ErrorCode::Validation);
					return false;
				}
				CHECK(inflated->size() == size);
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}
	}

}
