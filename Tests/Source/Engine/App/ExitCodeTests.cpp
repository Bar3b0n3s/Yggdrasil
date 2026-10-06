#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"

#include "Engine/Core/FatalError.h"

namespace Engine {

	TEST_SUITE("App")
	{
		TEST_CASE("ExitCode: values follow the documented table and match FatalError's")
		{
			// Architecture §4.1: CI, the MCP bridge and the exporter rely on these values.
			CHECK(ExitCode::Success == 0);
			CHECK(ExitCode::Failed == 1);
			CHECK(ExitCode::UsageError == 2);
			CHECK(ExitCode::InitFailed == 3);
			CHECK(ExitCode::Crash == 4);
			CHECK(ExitCode::Timeout == 5);

			// Core's FatalError exits with the same values (ADR 0003 decision 2).
			CHECK(GetFatalErrorExitCode(FatalErrorKind::InitFailed) == ExitCode::InitFailed);
			CHECK(GetFatalErrorExitCode(FatalErrorKind::Assert) == ExitCode::Crash);
			CHECK(GetFatalErrorExitCode(FatalErrorKind::DeviceLost) == ExitCode::Crash);
		}

		TEST_CASE("ExitCode: each documented code has its name and other values have none")
		{
			static_assert(ExitCodeToString(ExitCode::Success) == "Success");
			CHECK(ExitCodeToString(ExitCode::Success) == "Success");
			CHECK(ExitCodeToString(ExitCode::Failed) == "Failed");
			CHECK(ExitCodeToString(ExitCode::UsageError) == "UsageError");
			CHECK(ExitCodeToString(ExitCode::InitFailed) == "InitFailed");
			CHECK(ExitCodeToString(ExitCode::Crash) == "Crash");
			CHECK(ExitCodeToString(ExitCode::Timeout) == "Timeout");
			CHECK(ExitCodeToString(6).empty());
			CHECK(ExitCodeToString(-1).empty());
			CHECK(ExitCodeToString(255).empty());
		}
	}

}
