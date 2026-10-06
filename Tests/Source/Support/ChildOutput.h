#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What a child process leaves behind for its parent test: values logged on standard error and crash reports. Child
// bodies log what the parent checks as "<label>: [<value>]", so a value is found without parsing log prefixes.

namespace Engine {

	namespace Test {

		// The text between "<prefix>[" and the next ']' in `text`; empty when either is missing.
		[[nodiscard]] std::string FindBracketedValue(std::string_view text, std::string_view prefix);

		// Whether `parts` appear in `text` in this order, each after the end of the one before.
		[[nodiscard]] bool ContainsInOrder(std::string_view text, std::span<const std::string> parts);

		// The files in `directory` whose names start with "crash-" and end with `extension` (".txt", ".dmp"), sorted; empty
		// when the directory does not exist.
		[[nodiscard]] std::vector<std::filesystem::path> ListCrashFiles(const std::filesystem::path& directory, std::string_view extension);

		// The text of the one crash report in <userDataRoot>/<ENGINE_PRODUCT_NAME>/Crashes, where a Tests child started with
		// --user-data-dir=<userDataRoot> writes its reports. Errors: NotFound unless there is exactly one report; those of
		// FileSystem::ReadText.
		[[nodiscard]] Result<std::string> ReadOnlyCrashReport(const std::filesystem::path& userDataRoot);

	}

}
