#pragma once

#include "Engine/Core/Base.h"

#include <string>
#include <string_view>

// Text operations on relative paths with forward slashes ("Assets/Scenes/Level1.scene"), shared by FileSystem and the
// mounts for the case policy (Architecture §4.10), which compares names with EqualsIgnoreAsciiCase (AsciiText.h).

namespace Engine {

	namespace Utils {

		// The part before the last '/', or "" when there is none (the mount root).
		[[nodiscard]] std::string_view ParentPathOf(std::string_view relativePath);

		// The part after the last '/', or the whole path when there is none.
		[[nodiscard]] std::string_view FileNameOf(std::string_view relativePath);

		// `directory` followed by '/' and `name`, or `name` alone when `directory` is the root ("").
		[[nodiscard]] std::string JoinRelative(std::string_view directory, std::string_view name);

		// True when `path` lies strictly below `directory`, segment-wise ("A/B" is below "A", "AB" is not, "A" is not
		// below itself). Every non-empty path is below the root "".
		[[nodiscard]] bool IsStrictlyUnder(std::string_view path, std::string_view directory);

		// True for a case-only rename: both paths have the same parent (byte-wise) and final names that differ, but only
		// in ASCII letter case.
		[[nodiscard]] bool IsCaseOnlyRename(std::string_view from, std::string_view to);

	}

}
