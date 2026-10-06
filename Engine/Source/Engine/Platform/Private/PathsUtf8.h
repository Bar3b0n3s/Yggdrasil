#pragma once

#include "Engine/Core/Base.h"

#include <filesystem>
#include <string>
#include <string_view>

// UTF-8 text and the host's native strings for Platform's own files (Paths, Process, ProjectLock, CrashHandler). Engine
// text is UTF-8 (Architecture §16); paths, command lines and environment blocks use the host encoding: UTF-16 on
// Windows, bytes on Linux and macOS. std::filesystem::path's narrow constructor uses the ANSI code page on Windows, and
// its UTF-8 accessors throw on ill-formed input; these conversions never fail. Core's equivalents are private to Core.

namespace Engine {

	namespace Utils {

		using NativeString = std::filesystem::path::string_type;
		using NativeStringView = std::basic_string_view<std::filesystem::path::value_type>;

		// `utf8` in the host encoding; an ill-formed sequence becomes U+FFFD, so the result never names an unrelated file.
		// Callers that must reject ill-formed text check it first (IsValidUtf8).
		[[nodiscard]] NativeString NativeStringFromUtf8(std::string_view utf8);

		// `native` as UTF-8. On Windows an unpaired UTF-16 surrogate becomes U+FFFD; on Linux and macOS the bytes are
		// returned unchanged.
		[[nodiscard]] std::string NativeStringToUtf8(NativeStringView native);

		// The host path spelled by the UTF-8 text `utf8` (NativeStringFromUtf8).
		[[nodiscard]] std::filesystem::path NativePathFromUtf8(std::string_view utf8);

		// `path` in its native form (the host's separators) as UTF-8, for messages and reports.
		[[nodiscard]] std::string NativePathToUtf8(const std::filesystem::path& path);

	}

}
