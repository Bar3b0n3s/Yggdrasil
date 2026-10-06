#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <string_view>

namespace Engine {

	// True when `text` is well-formed UTF-8 (RFC 3629): no overlong encodings, no surrogate code points
	// (U+D800-U+DFFF), nothing above U+10FFFF, no truncated sequences. The empty string and embedded U+0000 bytes are
	// valid. Used by VirtualFileSystem::ReadText, FileSystem::ReadText, VfsPath and the JSON reader and writer.
	[[nodiscard]] bool IsValidUtf8(std::string_view text);

	// The byte offset of the first invalid sequence in `text`, or text.size() when it is valid.
	[[nodiscard]] size_t FindInvalidUtf8(std::string_view text);

}
