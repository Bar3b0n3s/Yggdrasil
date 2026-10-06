#include "TestsPCH.h"

#include "Support/ExpectLog.h"

#include "Engine/Core/Log.h"
#include "Support/ChildProcess.h"
#include "Support/TestOptions.h"

#include <thread>

namespace Engine {

	// Runs the ChildTargets case `name` alone in a child process and checks that it failed, with `expectedFailure` in the
	// child's doctest report (standard output). A failing case is the observable effect of the listener, so the meta-tests
	// below look at it from outside, where a deliberately failing case does not fail this run.
	static void CheckTargetFails(std::string_view name, std::string_view expectedFailure)
	{
		const std::vector<std::string> arguments = { "--no-skip", std::format("--test-case={}", name) };
		const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath, arguments,
			std::chrono::seconds(60));
		REQUIRE(result.has_value());
		INFO("child stdout: ", result->StandardOutput);
		CHECK(result->ExitCode == 1);
		CHECK(result->StandardOutput.contains(expectedFailure));
	}

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not contract stubs: the targets of the meta-tests below, which run each one alone
		// in a child with --no-skip. They fail by design, so they belong to Test::ChildTargetSuite.
		TEST_CASE("ExpectLog: the undeclared error target logs an Error" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			ENGINE_CORE_ERROR("Undeclared error from the ExpectLog meta-test");
		}

		TEST_CASE("ExpectLog: the undeclared critical target logs a Critical" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			ENGINE_CRITICAL("Undeclared critical from the ExpectLog meta-test");
		}

		TEST_CASE("ExpectLog: the missing entry target declares an entry it never logs" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			Test::ExpectLog expected(LogLevel::Error, "this message is never logged");
		}

		TEST_CASE("ExpectLog: the wrong level target logs an Error where a Warn is expected"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			Test::ExpectLog expected(LogLevel::Warn, "disk almost full");
			ENGINE_CORE_ERROR("disk almost full"); // an Error does not satisfy a Warn expectation and is undeclared
		}

		TEST_CASE("ExpectLog: the expired expectation target logs after its expectation ended"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			{
				Test::ExpectLog expected(LogLevel::Error, "scoped");
				ENGINE_CORE_ERROR("scoped error inside");
			}
			ENGINE_CORE_ERROR("scoped error after the expectation ended");
		}

		TEST_CASE("ExpectLog: the worker thread target logs an undeclared Error on another thread"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			std::thread worker([]()
			{
				ENGINE_CORE_ERROR("Undeclared error from a worker thread");
			});
			worker.join();
		}

		TEST_CASE("ExpectLog: the failed require target stops with an unmet expectation"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			Test::ExpectLog expected(LogLevel::Error, "logged after the REQUIRE");
			REQUIRE(Test::GetTestOptions().DeathTest == "a death test that is not running");
			ENGINE_CORE_ERROR("logged after the REQUIRE");
		}

		// The meta-test of Roadmap M1: the ExpectLog listener fails a test case that logs an undeclared Error.
		TEST_CASE("ExpectLog: an undeclared ENGINE_CORE_ERROR fails its test case")
		{
			CheckTargetFails("ExpectLog: the undeclared error target logs an Error", "Undeclared error from the ExpectLog meta-test");
		}

		TEST_CASE("ExpectLog: an undeclared Critical fails its test case")
		{
			CheckTargetFails("ExpectLog: the undeclared critical target logs a Critical",
				"Undeclared critical from the ExpectLog meta-test");
		}

		TEST_CASE("ExpectLog: an expected entry that never appears fails its test case")
		{
			CheckTargetFails("ExpectLog: the missing entry target declares an entry it never logs",
				"\"this message is never logged\", but none was logged");
		}

		TEST_CASE("ExpectLog: the level must match exactly")
		{
			CheckTargetFails("ExpectLog: the wrong level target logs an Error where a Warn is expected", "\"disk almost full\" (declare");
		}

		TEST_CASE("ExpectLog: an expectation covers only its own lifetime")
		{
			CheckTargetFails("ExpectLog: the expired expectation target logs after its expectation ended",
				"scoped error after the expectation ended");
		}

		TEST_CASE("ExpectLog: an undeclared error on another thread fails the running test case")
		{
			CheckTargetFails("ExpectLog: the worker thread target logs an undeclared Error on another thread",
				"Undeclared error from a worker thread");
		}

		TEST_CASE("ExpectLog: a test case stopped by REQUIRE reports no missing entry")
		{
			const std::vector<std::string> arguments = {
				"--no-skip",
				"--test-case=ExpectLog: the failed require target stops with an unmet expectation",
			};
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			INFO("child stdout: ", result->StandardOutput);
			CHECK(result->ExitCode == 1);
			CHECK(result->StandardOutput.contains("REQUIRE"));
			CHECK_FALSE(result->StandardOutput.contains("but none was logged"));
		}

		TEST_CASE("ExpectLog: a declared error passes and is counted")
		{
			Test::ExpectLog expected(LogLevel::Error, "could not open");
			ENGINE_CORE_ERROR("Asset 'Track.glb' could not open: {}", "missing");
			ENGINE_CORE_ERROR("Asset 'Ball.glb' could not open: {}", "missing");
			CHECK(expected.GetMatchCount() == 2);
		}

		TEST_CASE("ExpectLog: every live expectation that matches counts the entry")
		{
			Test::ExpectLog broad(LogLevel::Error, "Shader");
			Test::ExpectLog narrow(LogLevel::Error, "Shader 'Lit' failed");
			Test::ExpectLog unrelated(LogLevel::Error, "Texture");
			ENGINE_CORE_ERROR("Shader 'Lit' failed to compile");
			ENGINE_CORE_ERROR("Shader 'Sky' failed to compile");
			ENGINE_CORE_ERROR("Texture 'Albedo.png' is missing");
			CHECK(broad.GetMatchCount() == 2);
			CHECK(narrow.GetMatchCount() == 1);
			CHECK(unrelated.GetMatchCount() == 1);
		}

		TEST_CASE("ExpectLog: undeclared warnings never fail a test")
		{
			ENGINE_CORE_WARN("An undeclared warning is fine");
			ENGINE_WARN("So is a client warning");

			Test::ExpectLog expected(LogLevel::Warn, "counted warning");
			ENGINE_CORE_WARN("A counted warning");
			CHECK(expected.GetMatchCount() == 1);
		}

		TEST_CASE("ExpectLog: entries logged on other threads are matched")
		{
			Test::ExpectLog expected(LogLevel::Error, "worker failed");
			std::thread worker([]()
			{
				ENGINE_CORE_ERROR("Import worker failed on '{}'", "Track.glb");
			});
			worker.join();
			CHECK(expected.GetMatchCount() == 1);
		}
	}

}
