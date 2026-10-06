#pragma once

#include "Engine/Core/Base.h"

#include <filesystem>
#include <string>
#include <string_view>

// Conversions between UTF-8 text and host paths for FileSystem and NativeDirectoryMount. std::filesystem::path's own
// narrow-string constructor uses the ANSI code page on Windows, and its UTF-8 accessors throw on file names that are not
// valid UTF-16; these never throw (except std::bad_alloc).

namespace Engine {

	namespace Utils {

		// The host path spelled by the UTF-8 text `utf8` (forward slashes are separators on every host). An ill-formed
		// sequence becomes U+FFFD, so the result names no existing file rather than a wrong one.
		[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view utf8);

		// The generic form of `path` (forward slashes) as UTF-8. On Windows an unpaired UTF-16 surrogate becomes U+FFFD; on
		// POSIX hosts the bytes are returned unchanged, valid UTF-8 or not.
		[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);

		// The last component of `path` (its file name) as UTF-8, converted like PathToUtf8.
		[[nodiscard]] std::string FileNameToUtf8(const std::filesystem::path& path);

	}

}
