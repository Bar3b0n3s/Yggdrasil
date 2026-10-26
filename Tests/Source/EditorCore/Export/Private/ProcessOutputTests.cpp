#include "TestsPCH.h"

#include "EditorCore/Export/Private/ProcessOutput.h"
#include "Engine/Platform/Process.h"

#include <string>

// What the exporter's errors quote of the processes it runs (the smoke test, the feature test, codesign).

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ProcessOutput: the tail is the last non-empty lines of standard output, then standard error")
		{
			const ProcessResult result{ .ExitCode = 1, .StandardOutput = "one\r\n\ntwo  \nthree", .StandardError = "four\n\n" };
			CHECK(Utils::TailOfProcessOutput(result, 10) == "one\ntwo\nthree\nfour");
			CHECK(Utils::TailOfProcessOutput(result, 2) == "three\nfour");
			CHECK(Utils::TailOfProcessOutput(ProcessResult{ .ExitCode = 0, .StandardOutput = " \r\n", .StandardError = "" }, 5) == "(no output)");
		}
	}

}
