#include "TestsPCH.h"

#include "Engine/Core/Hash.h"

namespace Engine {

	// The sanity buffer of the xxHash reference test suite: byte i is the top byte of 2654435761 * 11400714785074694797^i
	// (mod 2^64).
	static std::vector<std::byte> MakeSanityBuffer(size_t size)
	{
		constexpr uint64_t Prime32 = 2654435761ull;
		constexpr uint64_t Prime64 = 11400714785074694797ull;
		std::vector<std::byte> buffer(size);
		uint64_t generator = Prime32;
		for (std::byte& value : buffer)
		{
			value = static_cast<std::byte>(generator >> 56);
			generator *= Prime64;
		}
		return buffer;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Hash: XXH64 matches reference test vectors" * doctest::skip(true))
		{
			constexpr uint64_t Prime32 = 2654435761ull;
			const std::vector<std::byte> buffer = MakeSanityBuffer(2367);
			const std::span<const std::byte> bytes(buffer);

			CHECK(XXH64(bytes.first(0), 0) == 0xEF46DB3751D8E999ull);
			CHECK(XXH64(bytes.first(0), Prime32) == 0xAC75FDA2929B17EFull);
			CHECK(XXH64(bytes.first(1), 0) == 0xE934A84ADB052768ull);
			CHECK(XXH64(bytes.first(1), Prime32) == 0x5014607643A9B4C3ull);
			CHECK(XXH64(bytes.first(14), 0) == 0x8282DCC4994E35C8ull);
			CHECK(XXH64(bytes.first(14), Prime32) == 0xC3BD6BF63DEB6DF0ull);
			CHECK(XXH64(bytes.first(222), 0) == 0xB641AE8CB691C174ull);
			CHECK(XXH64(bytes.first(222), Prime32) == 0x20CB8AB7AE10C14Aull);
			CHECK(XXH64(bytes.first(100), 0) == 0x4BFE019CD91D9EA4ull);
			CHECK(XXH64(bytes, 0) == 0xA82418DDEC0EA581ull);

			CHECK(XXH64(std::string_view(""), 0) == 0xEF46DB3751D8E999ull);
			CHECK(XXH64(std::string_view("a"), 0) == 0xD24EC4F1A98C6E5Bull);
			CHECK(XXH64(std::string_view("abc"), 0) == 0x44BC2CF5AD770999ull);
			CHECK(XXH64(std::string_view("Hello, world!"), 0) == 0xF58336A78B6F9476ull);
		}

		TEST_CASE("Hash: streaming XXH64 equals one-shot XXH64 for every split" * doctest::skip(true))
		{
			const std::vector<std::byte> buffer = MakeSanityBuffer(222);
			const std::span<const std::byte> bytes(buffer);
			const uint64_t expected = XXH64(bytes, 2654435761ull);

			for (size_t split = 0; split <= bytes.size(); ++split)
			{
				XXH64Hasher hasher(2654435761ull);
				hasher.Update(bytes.first(split));
				hasher.Update(bytes.subspan(split));
				CHECK(hasher.Digest() == expected);
			}

			XXH64Hasher byteByByte(2654435761ull);
			for (const std::byte value : bytes)
				byteByByte.Update(std::span<const std::byte>(&value, 1));
			CHECK(byteByByte.Digest() == expected);
			CHECK(byteByByte.Digest() == expected); // Digest does not change the state

			byteByByte.Reset(0);
			CHECK(byteByByte.Digest() == 0xEF46DB3751D8E999ull);
		}

		TEST_CASE("Hash: Hash64 hashes the little-endian value or the key bytes with the seed" * doctest::skip(true))
		{
			CHECK(Hash64(1, 2) == 0x2EFA9E5D4E7FC483ull);
			CHECK(Hash64(0x1111, 0x2222) == 0x26BFDF9F2EA69EC9ull);
			CHECK(Hash64(0x3c9f2e7a11d04b88ull, std::string_view("mesh:0:Straight")) == 0x042C289A67ECF6E6ull);

			const std::array<std::byte, 8> littleEndian = {
				std::byte{ 0x22 },
				std::byte{ 0x22 },
				std::byte{ 0 },
				std::byte{ 0 },
				std::byte{ 0 },
				std::byte{ 0 },
				std::byte{ 0 },
				std::byte{ 0 },
			};
			CHECK(Hash64(0x1111, 0x2222) == XXH64(littleEndian, 0x1111));

			XXH64Hasher hasher(0x1111);
			hasher.UpdateU64(0x2222);
			CHECK(hasher.Digest() == Hash64(0x1111, 0x2222));
		}

		TEST_CASE("Hash: FNV-1a matches reference test vectors")
		{
			static_assert(FNV1a32("") == 0x811c9dc5u);
			static_assert(FNV1a32("a") == 0xe40c292cu);
			static_assert(FNV1a32("foobar") == 0xbf9cf968u);
			static_assert(FNV1a64("") == 0xcbf29ce484222325ull);
			static_assert(FNV1a64("a") == 0xaf63dc4c8601ec8cull);
			static_assert(FNV1a64("foobar") == 0x85944171f73967e8ull);

			const std::string runtime = "foobar";
			CHECK(FNV1a64(runtime) == 0x85944171f73967e8ull);
		}
	}

}
