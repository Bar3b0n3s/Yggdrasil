#pragma once

#include "Engine/Core/Assert.h"

// The Tests binary's assert handler (Architecture §4.5): tests never throw from an assert handler.

namespace Engine {

	namespace Test {

		// When a doctest test case is running: logs "<FormatAssertInfo(info)> [test case: <name>]" at Critical level and
		// records a doctest failure with the same text. Otherwise (a death-test child, which runs no test case): logs
		// FormatAssertInfo(info) alone. The console sink writes the line to stderr, which is what a death-test parent
		// matches, so the failing case is named there even if doctest's report on stdout were lost. Then it terminates
		// through FatalError(FatalErrorKind::Assert), which flushes the log sinks and C stdio (doctest's report reaches a
		// redirected stdout too) and exits with code 4. Never returns, never throws, never unwinds through noexcept code,
		// destructors or C callbacks. Thread-safe.
		void RecordingAssertHandler(const AssertInfo& info);

		// SetAssertHandler(&RecordingAssertHandler). The Tests main calls it once, before any test or death-test body runs.
		void InstallRecordingAssertHandler();

	}

}
