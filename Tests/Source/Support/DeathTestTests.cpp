#include "TestsPCH.h"

#include "Support/DeathTest.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"

namespace Engine {

	static int ComputeAnswer()
	{
		return 41;
	}

	ENGINE_DEATH_TEST("Support/AssertsWithMessage")
	{
		const int answer = ComputeAnswer();
		ENGINE_CORE_ASSERT(answer == 42, "The answer is {}, not 42", answer);
	}

	ENGINE_DEATH_TEST("Support/ReturnsWithoutDying")
	{
	}

	TEST_SUITE("Support")
	{
		TEST_CASE("DeathTest: registered names are listed sorted and found" * doctest::skip(true))
		{
			const std::vector<std::string> names = Test::GetDeathTestNames();
			CHECK(std::ranges::is_sorted(names));
			CHECK(std::ranges::find(names, "Support/AssertsWithMessage") != names.end());
			CHECK(std::ranges::find(names, "Core/AssertFires") != names.end());
			CHECK(Test::FindDeathTest("Support/ReturnsWithoutDying") != nullptr);
			CHECK(Test::FindDeathTest("Support/NoSuchTest") == nullptr);
		}

		TEST_CASE("DeathTest: the child exits with code 4 and the assert message on stderr" * doctest::skip(true))
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Support/AssertsWithMessage");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("The answer is 41, not 42"));
			CHECK(result->StandardError.contains("Assertion failed: answer == 42"));

			Test::CheckDeath("Support/AssertsWithMessage", "The answer is 41, not 42");
		}

		TEST_CASE("DeathTest: a body that returns makes the child exit with code 1" * doctest::skip(true))
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Support/ReturnsWithoutDying");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == 1);
			CHECK(result->StandardError.contains("returned without dying"));
		}

		TEST_CASE("DeathTest: an unknown name is NotFound without spawning a child" * doctest::skip(true))
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Support/NoSuchTest");
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::NotFound);
			CHECK(Test::RunDeathTestBody("Support/NoSuchTest") == 2);
		}
	}

}
