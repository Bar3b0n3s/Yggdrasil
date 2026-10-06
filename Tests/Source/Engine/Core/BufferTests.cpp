#include "TestsPCH.h"

#include "Engine/Core/Buffer.h"

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("Buffer: AsBytes and AsStringView view the same storage")
		{
			const std::string_view text = "bytes";
			const std::span<const std::byte> bytes = AsBytes(text);
			CHECK(bytes.size() == text.size());
			CHECK(static_cast<const void*>(bytes.data()) == static_cast<const void*>(text.data()));
			CHECK(std::to_integer<char>(bytes[0]) == 'b');

			const std::string_view back = AsStringView(bytes);
			CHECK(back == text);
			CHECK(back.data() == text.data());

			const Buffer owned(bytes.begin(), bytes.end());
			CHECK(AsStringView(owned) == "bytes");
		}
	}

}
