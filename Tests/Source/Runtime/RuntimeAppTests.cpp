#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Platform/Process.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

// The runtime executable as a whole process (Roadmap M2): RunApplication with the runtime's (for now empty)
// application.

namespace Engine {

	TEST_SUITE("Runtime")
	{
		TEST_CASE("RuntimeApp: --headless --frames 10 exits 0 using ManualClock")
		{
			const Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			Test::TempDirectory userData("RuntimeHeadless");
			const ProcessSpecification specification = {
				.Executable = *runtime,
				.Arguments = { "--headless", "--frames", "10", "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) },
			};
			const Result<ProcessResult> result = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			// Headless play without lockstep is paced at FixedHz (§4.2), never run flat out.
			CHECK(result->StandardError.contains("Frame loop started: Manual clock, 60 Hz, throttled"));
			// The log file follows --user-data-dir, never the real user-data folder.
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(userData / ENGINE_PRODUCT_NAME / "Logs" / "Runtime.log", error));
		}
	}

}
