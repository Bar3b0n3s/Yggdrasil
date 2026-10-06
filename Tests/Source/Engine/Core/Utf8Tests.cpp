#include "TestsPCH.h"

#include "Engine/Core/Utf8.h"

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("Utf8: accepts well-formed sequences of every length")
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

		TEST_CASE("Utf8: rejects overlong, surrogate, out-of-range and truncated sequences")
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

		TEST_CASE("Utf8: the boundaries of every lead byte range are exact")
		{
			CHECK(IsValidUtf8("\xc2\x80"));               // U+0080, the first two-byte code point
			CHECK(IsValidUtf8("\xed\x9f\xbf"));           // U+D7FF, just below the surrogates
			CHECK(IsValidUtf8("\xee\x80\x80"));           // U+E000, just above the surrogates
			CHECK(IsValidUtf8("\xef\xbf\xbf"));           // U+FFFF
			CHECK(IsValidUtf8("\xf0\x90\x80\x80"));       // U+10000, the first four-byte code point
			CHECK_FALSE(IsValidUtf8("\xc1\xbf"));         // overlong U+007F
			CHECK_FALSE(IsValidUtf8("\xe0\x9f\xbf"));     // overlong U+07FF
			CHECK_FALSE(IsValidUtf8("\xed\xbf\xbf"));     // U+DFFF surrogate
			CHECK_FALSE(IsValidUtf8("\xf0\x8f\xbf\xbf")); // overlong U+FFFF
			CHECK_FALSE(IsValidUtf8("\xf5\x80\x80\x80")); // lead byte above F4
			CHECK_FALSE(IsValidUtf8("\xe2\x28\xac"));     // bad continuation byte
			CHECK(FindInvalidUtf8("\xc3\xa9\xc3\xa9\xff") == 4);
		}
	}

}
