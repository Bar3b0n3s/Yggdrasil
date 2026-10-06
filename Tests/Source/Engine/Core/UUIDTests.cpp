#include "TestsPCH.h"

#include "Engine/Core/UUID.h"

#include <set>
#include <unordered_set>

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("UUID: hex round trip and 6-digit prefix matching")
		{
			const UUID uuid(0x5d1c9a7e33b04f12);
			CHECK(uuid.ToString() == "5d1c9a7e33b04f12");
			CHECK(UUID::FromString("5d1c9a7e33b04f12") == uuid);
			CHECK(UUID::FromString("5D1C9A7E33B04F12") == uuid);
			CHECK(UUID::FromString(UUID(0x3ff).ToString()) == UUID(0x3ff));
			CHECK(UUID(0x3ff).ToString() == "00000000000003ff");

			CHECK(UUID::IsValidPrefix("5d1c9a"));
			CHECK(UUID::IsValidPrefix("5D1C9A7E33B04F12"));
			CHECK_FALSE(UUID::IsValidPrefix("5d1c9"));             // shorter than 6 digits
			CHECK_FALSE(UUID::IsValidPrefix("5d1c9a7e33b04f123")); // longer than 16 digits
			CHECK_FALSE(UUID::IsValidPrefix("5d1c9g"));

			CHECK(uuid.MatchesPrefix("5d1c9a"));
			CHECK(uuid.MatchesPrefix("5D1C9A7E"));
			CHECK(uuid.MatchesPrefix("5d1c9a7e33b04f12"));
			CHECK_FALSE(uuid.MatchesPrefix("5d1c9b"));
			CHECK_FALSE(uuid.MatchesPrefix("5d1c9")); // too short to be a prefix
			CHECK_FALSE(uuid.MatchesPrefix("d1c9a7"));
		}

		TEST_CASE("UUID: the invalid UUID is zero and formats as sixteen zeros")
		{
			constexpr UUID Invalid;
			static_assert(!Invalid.IsValid());
			static_assert(UUID(1).IsValid());
			CHECK(Invalid.GetValue() == 0);
			CHECK(Invalid.ToString() == "0000000000000000");

			const std::optional<UUID> parsed = UUID::FromString("0000000000000000");
			REQUIRE(parsed.has_value());
			CHECK_FALSE(parsed->IsValid());
		}

		TEST_CASE("UUID: FromString accepts exactly sixteen hex digits")
		{
			CHECK_FALSE(UUID::FromString("").has_value());
			CHECK_FALSE(UUID::FromString("5d1c9a7e33b04f1").has_value());   // 15 digits
			CHECK_FALSE(UUID::FromString("5d1c9a7e33b04f120").has_value()); // 17 digits
			CHECK_FALSE(UUID::FromString("0x5d1c9a7e33b04f").has_value());
			CHECK_FALSE(UUID::FromString(" 5d1c9a7e33b04f1").has_value());
			CHECK_FALSE(UUID::FromString("5d1c9a7e33b04f1z").has_value());
			CHECK_FALSE(UUID::FromString("-d1c9a7e33b04f12").has_value());
			CHECK_FALSE(UUID::FromString("5d1c9a7e33b04f\xc3\xa9").has_value()); // 16 bytes, the last two a UTF-8 letter
			std::string withNul = "5d1c9a7e33b04f12";
			withNul[8] = '\0';
			CHECK_FALSE(UUID::FromString(withNul).has_value());
			CHECK(UUID::FromString("ffffffffffffffff") == UUID(0xffffffffffffffff));
			CHECK(UUID::FromString("0123456789ABCdef") == UUID(0x0123456789abcdef));
			CHECK_FALSE(UUID::IsValidPrefix("5d1c9\xc3\xa9"));
			CHECK_FALSE(UUID::IsValidPrefix(""));
			CHECK_FALSE(UUID(0x5d1c9a7e33b04f12).MatchesPrefix("5d1c9 "));
		}

		TEST_CASE("UUID: ordering and hashing follow the numeric value")
		{
			const std::set<UUID> sorted = { UUID(0x30), UUID(0x10), UUID(0x20) };
			std::vector<uint64_t> values;
			for (const UUID uuid : sorted)
				values.push_back(uuid.GetValue());
			CHECK(values == std::vector<uint64_t>{ 0x10, 0x20, 0x30 });

			const std::unordered_set<UUID> set = { UUID(1), UUID(2), UUID(1) };
			CHECK(set.size() == 2);
			CHECK(std::hash<UUID>()(UUID(99)) == std::hash<UUID>()(UUID(99)));
		}

		TEST_CASE("UUID: std::format writes the sixteen-digit text form")
		{
			CHECK(std::format("{}", UUID(0xabc)) == "0000000000000abc");
			CHECK(std::format("entity {}", UUID(0x5d1c9a7e33b04f12)) == "entity 5d1c9a7e33b04f12");
		}
	}

}
