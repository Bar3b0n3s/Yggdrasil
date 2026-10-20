#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <string>

// What the exporter's error messages quote of a process it ran: the smoke test, the feature test (Exporter.h steps 6 and
// 6a) and macOS codesign.

namespace Engine {

	struct ProcessResult;

	namespace Utils {

		// The end of `result`'s output for an error message: the last `lineCount` non-empty lines of its standard output
		// followed by its standard error, each without its trailing carriage return and spaces, joined by "\n";
		// "(no output)" when there is none. Pure.
		[[nodiscard]] std::string TailOfProcessOutput(const ProcessResult& result, size_t lineCount);

	}

}
