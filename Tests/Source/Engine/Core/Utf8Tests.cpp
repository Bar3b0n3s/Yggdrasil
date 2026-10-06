#include "TestsPCH.h"

#include "Engine/Core/Utf8.h"

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("Utf8: accepts well-formed sequences of every length" * doctest::skip(true))
		{
			using namespace std::string_view_literals;
			CHECK(IsValidUtf8(""));
			CHECK(IsValidUtf8("plain ASCII"));
			CHECK(IsValidUtf8("\xc3\xa9"));         // U+00E9
			CHECK(IsValidUtf8("\xe2\x82\xac"));     // U+20AC
			CHECK(IsValidUtf8("\xf0\x9f\x98\x80")); // U+1F600
			CHECK(IsValidUtf8("\xf4\x8f\xbf\xbf")); // U+10FFFF
			const std::string withByteOrderMark = std::string("\xef\xbb\xbf") + "BOM";
			CHECK(IsValidUtf8(withByteOrderMark)); // a BOM is a valid code point
			CHECK(IsValidUtf8("embedded\0NUL"sv));
			CHECK(FindInvalidUtf8("\xe2\x82\xac") == 3);
		}

		TEST_CASE("Utf8: rejects overlong, surrogate, out-of-range and truncated sequences" * doctest::skip(true))
		{
			CHECK_FALSE(IsValidUtf8("\xc0\xaf"));             // overlong '/'
			CHECK_FALSE(IsValidUtf8("\xe0\x80\xaf"));         // overlong '/'
			CHECK_FALSE(IsValidUtf8("\xed\xa0\x80"));         // U+D800 surrogate
			CHECK_FALSE(IsValidUtf8("\xf4\x90\x80\x80"));     // above U+10FFFF
			CHECK_FALSE(IsValidUtf8("\xf8\x88\x80\x80\x80")); // 5-byte form
			CHECK_FALSE(IsValidUtf8("\xff"));
			CHECK_FALSE(IsValidUtf8("\x80"));     // lone continuation byte
			CHECK_FALSE(IsValidUtf8("\xe2\x82")); // truncated

			CHECK(FindInvalidUtf8("ok \xc3\x28") == 3);
			CHECK(FindInvalidUtf8("abc\xe2\x82") == 3);
		}
	}

}
