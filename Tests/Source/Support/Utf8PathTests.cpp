#include "TestsPCH.h"

#include "Support/Utf8Path.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("Utf8Path: a non-ASCII path round-trips through UTF-8")
		{
			const std::string text = "Data/\xC3\xA9t\xC3\xA9.txt"; // "Data/" followed by "ete.txt" with two e-acute, in UTF-8
			const std::filesystem::path path = Test::PathFromUtf8(text);
			CHECK(path.filename().u8string() == std::u8string(u8"été.txt"));
			CHECK(path.parent_path().u8string() == std::u8string(u8"Data"));
			CHECK(Test::PathToUtf8(path) == text);
			CHECK(Test::PathToUtf8(std::filesystem::path(u8"aé")) == "a\xC3\xA9");
			CHECK(Test::PathToUtf8(Test::PathFromUtf8("")).empty());
		}
	}

}
