#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// Death tests (Architecture §4.5): an expected assert is tested in a child process, never by unwinding.
//
//     ENGINE_DEATH_TEST("Core/AssertFires")
//     {
//         ENGINE_CORE_ASSERT(false, "The answer is {}", 42);
//     }
//
//     TEST_CASE("Assert: a failed ENGINE_CORE_ASSERT exits with code 4 and its message")
//     {
//         ENGINE_CHECK_DEATH("Core/AssertFires", "The answer is 42");
//     }
//
// The body runs only when the binary is started as `Tests --death-test=Core/AssertFires`; the parent test spawns that
// child and checks its exit code and standard error. Names are "<Module>/<CamelCaseName>", unique in the binary.

namespace Engine {

	namespace Test {

		using DeathTestBody = void (*)();

		inline constexpr std::chrono::milliseconds DefaultDeathTestTimeout{ 30000 };

		// Registers `body` under `name` and returns true (it initializes a static in ENGINE_DEATH_TEST). A duplicate name is a
		// programmer error reported by the child and parent runners (both registrations are kept so the error names the two
		// locations). Safe during static initialization (the registry is a function-local static).
		bool RegisterDeathTest(std::string_view name, DeathTestBody body, const char* file, int line);

		// The body registered as `name`, or nullptr.
		[[nodiscard]] DeathTestBody FindDeathTest(std::string_view name);

		// Every registered name, sorted.
		[[nodiscard]] std::vector<std::string> GetDeathTestNames();

		// Child mode (`Tests --death-test=<name>`): runs the body and returns the exit code the main returns when the body
		// does not terminate the process: 1 (Failed, with an Error log "Death test '<name>' returned without dying"), or 2
		// (UsageError) for an unknown or duplicated name. A body that asserts never returns here: the recording assert
		// handler exits with 4.
		[[nodiscard]] int RunDeathTestBody(std::string_view name);

		struct DeathTestResult
		{
			int ExitCode = 0;
			std::string StandardOutput;
			std::string StandardError;
		};

		// Parent mode: spawns GetTestOptions().ExecutablePath with "--death-test=<name>" through RunChildProcess and returns
		// its exit code and output. Errors: NotFound when `name` is not registered (checked in-process before spawning), Io
		// when the child cannot be started, Timeout when it runs longer than `timeout` (it is killed).
		[[nodiscard]] Result<DeathTestResult> RunDeathTest(std::string_view name,
			std::chrono::milliseconds timeout = DefaultDeathTestTimeout);

		// The standard expectation, checked by ENGINE_CHECK_DEATH: RunDeathTest(name) succeeds, the child exited with code
		// 4 (FatalCrashExitCode) and its standard error contains `expectedSubstring` (case-sensitive). Returns "" when all
		// of that holds, otherwise a description of every unmet condition that quotes the exit code and the child's
		// standard error.
		[[nodiscard]] std::string DescribeDeathMismatch(std::string_view name, std::string_view expectedSubstring);

	}

}

// The standard expectation of a death test (DescribeDeathMismatch) as one doctest CHECK at the call site, so a passing
// death test counts as an assertion like any other and a failure is reported at the caller's line.
#define ENGINE_CHECK_DEATH(name, expectedSubstring) \
	do \
	{ \
		const std::string engineDeathMismatch = ::Engine::Test::DescribeDeathMismatch(name, expectedSubstring); \
		CHECK_MESSAGE(engineDeathMismatch.empty(), engineDeathMismatch); \
	} while (false)

// Defines and registers a death-test body; follow it with the body's braces. Use at namespace scope in a test file.
#define ENGINE_DEATH_TEST(name) \
	static void ENGINE_CONCAT(EngineDeathTestBody, __LINE__)(); \
	[[maybe_unused]] static const bool ENGINE_CONCAT(EngineDeathTestRegistered, __LINE__) = \
		::Engine::Test::RegisterDeathTest(name, &ENGINE_CONCAT(EngineDeathTestBody, __LINE__), __FILE__, __LINE__); \
	static void ENGINE_CONCAT(EngineDeathTestBody, __LINE__)()
