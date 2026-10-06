#include "TestsPCH.h"

#include "Support/TestOptions.h"

#include "Support/ChildProcess.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("TestOptions: parses the death-test and timeout options and ignores doctest's" * doctest::skip(true))
		{
			const std::array<const char*, 5> argv = {
				"Tests",
				"--test-case=Core*",
				"--death-test=Core/AssertFires",
				"--test-timeout=2.5",
				"-s",
			};
			const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
			REQUIRE(options.has_value());
			CHECK(options->DeathTest == "Core/AssertFires");
			CHECK(options->DefaultTimeoutSeconds == 2.5);
			// ExecutablePath comes from the OS, not from argv[0] (which is a bare name without ".exe" here).
			const Result<std::filesystem::path> running = Test::GetCurrentExecutablePath();
			REQUIRE(running.has_value());
			CHECK(options->ExecutablePath == *running);
			CHECK(options->ExecutablePath.is_absolute());
			CHECK(options->ExecutablePath.stem() == "Tests");
		}

		TEST_CASE("TestOptions: defaults apply when no engine option is given" * doctest::skip(true))
		{
			const std::array<const char*, 1> argv = { "Tests" };
			const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
			REQUIRE(options.has_value());
			CHECK(options->DeathTest.empty());
			CHECK(options->DefaultTimeoutSeconds == 120.0);
		}

		TEST_CASE("TestOptions: malformed values are InvalidArgument" * doctest::skip(true))
		{
			for (const char* bad : { "--death-test=", "--test-timeout=0", "--test-timeout=-1", "--test-timeout=soon" })
			{
				const std::array<const char*, 2> argv = { "Tests", bad };
				const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
				INFO("argument: ", bad);
				REQUIRE_FALSE(options.has_value());
				CHECK(options.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(options.error().GetMessageText().contains(std::string_view(bad).substr(0, std::string_view(bad).find('='))));
			}
		}
	}

}
