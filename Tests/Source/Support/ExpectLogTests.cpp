#include "TestsPCH.h"

#include "Support/ExpectLog.h"

#include "Engine/Core/Log.h"

#include <thread>

namespace Engine {

	TEST_SUITE("Support")
	{
		// The meta-test of Roadmap M1: the ExpectLog listener fails a test case that logs an undeclared Error.
		// should_fail inverts the outcome, so this case passes exactly when the listener did its job.
		TEST_CASE("ExpectLog: an undeclared ENGINE_CORE_ERROR fails its test case" * doctest::should_fail() * doctest::skip(true))
		{
			ENGINE_CORE_ERROR("Undeclared error from the ExpectLog meta-test");
		}

		TEST_CASE("ExpectLog: an undeclared Critical fails its test case" * doctest::should_fail() * doctest::skip(true))
		{
			ENGINE_CRITICAL("Undeclared critical from the ExpectLog meta-test");
		}

		TEST_CASE("ExpectLog: an expected entry that never appears fails its test case" * doctest::should_fail() * doctest::skip(true))
		{
			Test::ExpectLog expected(LogLevel::Error, "this message is never logged");
		}

		TEST_CASE("ExpectLog: a declared error passes and is counted" * doctest::skip(true))
		{
			Test::ExpectLog expected(LogLevel::Error, "could not open");
			ENGINE_CORE_ERROR("Asset 'Track.glb' could not open: {}", "missing");
			ENGINE_CORE_ERROR("Asset 'Ball.glb' could not open: {}", "missing");
			CHECK(expected.GetMatchCount() == 2);
		}

		TEST_CASE("ExpectLog: the level must match exactly" * doctest::should_fail() * doctest::skip(true))
		{
			Test::ExpectLog expected(LogLevel::Warn, "disk almost full");
			ENGINE_CORE_ERROR("disk almost full"); // an Error does not satisfy a Warn expectation and is undeclared
		}

		TEST_CASE("ExpectLog: undeclared warnings never fail a test" * doctest::skip(true))
		{
			ENGINE_CORE_WARN("An undeclared warning is fine");
			ENGINE_WARN("So is a client warning");

			Test::ExpectLog expected(LogLevel::Warn, "counted warning");
			ENGINE_CORE_WARN("A counted warning");
			CHECK(expected.GetMatchCount() == 1);
		}

		TEST_CASE("ExpectLog: entries logged on other threads are matched" * doctest::skip(true))
		{
			Test::ExpectLog expected(LogLevel::Error, "worker failed");
			std::thread worker([]()
			{
				ENGINE_CORE_ERROR("Import worker failed on '{}'", "Track.glb");
			});
			worker.join();
			CHECK(expected.GetMatchCount() == 1);
		}

		TEST_CASE("ExpectLog: an expectation covers only its own lifetime" * doctest::should_fail() * doctest::skip(true))
		{
			{
				Test::ExpectLog expected(LogLevel::Error, "scoped");
				ENGINE_CORE_ERROR("scoped error inside");
			}
			ENGINE_CORE_ERROR("scoped error after the expectation ended");
		}
	}

}
