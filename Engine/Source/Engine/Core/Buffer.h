#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace Engine {

	// An owned, contiguous block of bytes: file contents (VirtualFileSystem::ReadFile), cooked payloads, BinaryWriter
	// output.
	using Buffer = std::vector<std::byte>;

	// The bytes of `text`, without a terminator. The span views `text`'s storage.
	[[nodiscard]] inline std::span<const std::byte> AsBytes(std::string_view text)
	{
		return std::as_bytes(std::span<const char>(text.data(), text.size()));
	}

	// `bytes` viewed as characters, without validation (see IsValidUtf8 in Utf8.h). The view aliases `bytes`' storage.
	[[nodiscard]] inline std::string_view AsStringView(std::span<const std::byte> bytes)
	{
		return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

}
