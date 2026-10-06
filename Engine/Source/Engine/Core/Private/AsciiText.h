#pragma once

#include "Engine/Core/Base.h"

#include <string_view>

// ASCII-only text operations shared by Core: case-insensitive matching of names (the case policy of paths, and the log
// level, log channel and engine event names that automation accepts in any case). Only 'A'-'Z' fold; every other byte,
// UTF-8 sequences included, must match exactly.

namespace Engine {

	namespace Utils {

		// 'A'-'Z' becomes 'a'-'z'; every other byte is returned unchanged.
		[[nodiscard]] char ToAsciiLower(char character);

		// True when both have the same length and equal bytes after ASCII case folding.
		[[nodiscard]] bool EqualsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs);

	}

}
