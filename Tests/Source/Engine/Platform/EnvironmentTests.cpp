#include "TestsPCH.h"

#include "Engine/Platform/Environment.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"
#include "Support/DeathTest.h"
#include "Support/TestOptions.h"

namespace Engine {

	// A value with characters outside ASCII, which Windows stores as UTF-16 (source files are UTF-8).
	static constexpr std::string_view Greeting = "Grüße ✓";

	// Prints the variables the parent set for this child, then returns.
	ENGINE_DEATH_TEST("Platform/ReportsEnvironment")
	{
		const std::optional<std::string> greeting = ReadEnvironmentVariable("ENGINE_TEST_GREETING");
		const std::optional<std::string> empty = ReadEnvironmentVariable("ENGINE_TEST_EMPTY");
		const std::optional<std::string> unset = ReadEnvironmentVariable("ENGINE_TEST_NEVER_SET");
		ENGINE_CORE_WARN("Greeting: [{}], empty set: [{}], empty value: [{}], unset: [{}]", greeting.value_or("<unset>"),
			empty.has_value() ? "yes" : "no", empty.value_or("<unset>"), unset.has_value() ? "set" : "unset");
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("Environment: reads variables set for the process as UTF-8 and reports unset ones")
		{
			ProcessSpecification specification = Test::MakeTestsChildSpecification({ "--death-test=Platform/ReportsEnvironment" });
			specification.Environment = {
				{ "ENGINE_TEST_GREETING", std::string(Greeting) },
				{ "ENGINE_TEST_EMPTY", "" },
			};
			const Result<ProcessResult> child = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == ExitCode::Failed); // the body returns without dying
			CHECK(child->StandardError.contains(std::format("Greeting: [{}]", Greeting)));
			CHECK(child->StandardError.contains("empty set: [yes], empty value: []"));
			CHECK(child->StandardError.contains("unset: [unset]"));
		}

		TEST_CASE("Environment: names that cannot be variables are never set")
		{
			CHECK_FALSE(ReadEnvironmentVariable("").has_value());
			CHECK_FALSE(ReadEnvironmentVariable("A=B").has_value());
			CHECK_FALSE(ReadEnvironmentVariable(std::string_view("A\0B", 3)).has_value());
		}
	}

}
