#include "TestsPCH.h"

#include "Engine/Core/FatalError.h"

#include "Engine/Core/Log.h"
#include "Support/DeathTest.h"

namespace Engine {

	static void LogFromFatalHandler(FatalErrorKind kind, std::string_view message)
	{
		ENGINE_CORE_CRITICAL("Fatal handler ran for {}: {}", FatalErrorKindToString(kind), message);
		FatalError(FatalErrorKind::Assert, "nested fatal error from the handler");
	}

	static size_t CountOccurrences(std::string_view text, std::string_view pattern)
	{
		size_t count = 0;
		size_t position = text.find(pattern);
		while (position != std::string_view::npos)
		{
			++count;
			position = text.find(pattern, position + pattern.size());
		}
		return count;
	}

	ENGINE_DEATH_TEST("Core/FatalErrorExits")
	{
		FatalError(FatalErrorKind::GpuHang, "simulated hang after 10 s");
	}

	ENGINE_DEATH_TEST("Core/FatalErrorRunsHandlerOnce")
	{
		SetFatalErrorHandler(&LogFromFatalHandler);
		FatalError(FatalErrorKind::DeviceLost, "simulated device loss");
	}

	ENGINE_DEATH_TEST("Core/FatalInitFailedExits")
	{
		FatalError(FatalErrorKind::InitFailed, "no Vulkan loader");
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("FatalError: logs the kind and message and exits with code 4" * doctest::skip(true))
		{
			Test::CheckDeath("Core/FatalErrorExits", "Fatal error (GpuHang): simulated hang after 10 s");
		}

		TEST_CASE("FatalError: InitFailed exits with code 3" * doctest::skip(true))
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Core/FatalInitFailedExits");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalInitFailedExitCode);
			CHECK(result->StandardError.contains("Fatal error (InitFailed): no Vulkan loader"));
		}

		TEST_CASE("FatalError: the handler runs once and a nested call skips it" * doctest::skip(true))
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Core/FatalErrorRunsHandlerOnce");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(CountOccurrences(result->StandardError, "Fatal handler ran for DeviceLost: simulated device loss") == 1);
		}

		TEST_CASE("FatalError: InitFailed maps to exit code 3 and every other kind to 4" * doctest::skip(true))
		{
			CHECK(GetFatalErrorExitCode(FatalErrorKind::InitFailed) == 3);
			CHECK(GetFatalErrorExitCode(FatalErrorKind::Assert) == 4);
			CHECK(GetFatalErrorExitCode(FatalErrorKind::UnhandledException) == 4);
			CHECK(GetFatalErrorExitCode(FatalErrorKind::OutOfMemory) == 4);
			CHECK(GetFatalErrorExitCode(FatalErrorKind::DeviceLost) == 4);
			CHECK(GetFatalErrorExitCode(FatalErrorKind::GpuHang) == 4);
			CHECK(GetFatalErrorExitCode(FatalErrorKind::Gpu) == 4);
		}

		TEST_CASE("FatalError: FatalErrorKindToString names every kind" * doctest::skip(true))
		{
			CHECK(FatalErrorKindToString(FatalErrorKind::Assert) == "Assert");
			CHECK(FatalErrorKindToString(FatalErrorKind::UnhandledException) == "UnhandledException");
			CHECK(FatalErrorKindToString(FatalErrorKind::OutOfMemory) == "OutOfMemory");
			CHECK(FatalErrorKindToString(FatalErrorKind::DeviceLost) == "DeviceLost");
			CHECK(FatalErrorKindToString(FatalErrorKind::GpuHang) == "GpuHang");
			CHECK(FatalErrorKindToString(FatalErrorKind::Gpu) == "Gpu");
			CHECK(FatalErrorKindToString(FatalErrorKind::InitFailed) == "InitFailed");
		}

		TEST_CASE("FatalError: SetFatalErrorHandler returns the previous handler" * doctest::skip(true))
		{
			const FatalErrorHandler original = SetFatalErrorHandler(&LogFromFatalHandler);
			CHECK(SetFatalErrorHandler(original) == &LogFromFatalHandler);
		}
	}

}
