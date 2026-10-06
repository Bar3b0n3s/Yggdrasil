#pragma once

#include "Engine/Core/Base.h"

#include <optional>
#include <string>
#include <string_view>

// Reading the process environment (Architecture §8.1: ENGINE_GPU; Roadmap M5: the ENGINE_VULKAN_LOADER test hook). The
// OS part lives in Platform/Windows/EnvironmentWindows.cpp (GetEnvironmentVariableW, UTF-16 to UTF-8) and
// Platform/Posix/EnvironmentPosix.cpp (getenv); the MSVC runtime deprecates getenv, and its getenv_s does not exist on
// POSIX, so no other code reads the environment itself. Writing the environment is not offered: a child process gets its
// variables through ProcessSpecification::Environment (Process.h).

namespace Engine {

	// The value of environment variable `name` as UTF-8; nullopt when it is not set. An empty value is returned as an
	// empty string, set. A `name` that is empty or contains '=' or NUL is never set (nullopt). A Windows value that is not
	// valid UTF-16 is converted with U+FFFD replacements. Thread-safe as long as nothing modifies the environment
	// concurrently (the engine never does).
	[[nodiscard]] std::optional<std::string> ReadEnvironmentVariable(std::string_view name);

}
