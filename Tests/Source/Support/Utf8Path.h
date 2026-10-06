#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// UTF-8 text and host paths in tests (Architecture §16: paths are UTF-8 internally). std::filesystem::path's narrow
// constructor and string() use the ANSI code page on Windows, so tests convert through these wherever a path becomes
// text or text becomes a path: child-process arguments, messages, file names read back from a directory.

namespace Engine {

	namespace Test {

		// The host path spelled by the UTF-8 text `text` (no validation: tests pass well-formed text).
		[[nodiscard]] inline std::filesystem::path PathFromUtf8(std::string_view text)
		{
			return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
		}

		// `path` in its native format as UTF-8.
		[[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path& path)
		{
			const std::u8string text = path.u8string();
			return std::string(reinterpret_cast<const char*>(text.data()), text.size());
		}

	}

}
