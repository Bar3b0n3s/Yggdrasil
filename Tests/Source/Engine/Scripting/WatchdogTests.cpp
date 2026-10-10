#include "TestsPCH.h"
#include "Engine/Scripting/Watchdog.h"

#include "Engine/Core/Random.h"
#include "Engine/Scripting/Sandbox.h"

#include <doctest/doctest.h>

#include <limits>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Watchdog: infinite loop raises timeout")
		{
			Random random(19);
			double now = 0.0;
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.ClockSeconds = [&now]
			{
				now += 0.02;
				return now;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Scripts/Spin.luau");
			REQUIRE(path.has_value());
			const Status result = (*sandbox)->RunSource(*path, "while true do end");
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Timeout);
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Kind == ScriptErrorKind::Timeout);
			CHECK(error->Line == 1);
			CHECK(error->Message.find("possible infinite loop") != std::string::npos);
		}

		TEST_CASE("ScriptWatchdog: caught timeout stays latched and message text cannot spoof the error kind")
		{
			Random random(59);
			double now = 0.0;
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.IsTestRun = true;
			specification.CallbackBudgetMs = 1000;
			specification.ClockSeconds = [&now]
			{
				now += 0.02;
				return now;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/CaughtTimeout.luau");
			REQUIRE(path.has_value());
			CHECK_FALSE((*sandbox)->Evaluate("pcall(function() while true do end end); return 1", *path).has_value());
			const auto timeout = (*sandbox)->GetLastError();
			REQUIRE(timeout.has_value());
			CHECK(timeout->Kind == ScriptErrorKind::Timeout);
			CHECK_FALSE((*sandbox)->Evaluate("error('possible infinite loop; memory limit exceeded')", *path).has_value());
			const auto runtime = (*sandbox)->GetLastError();
			REQUIRE(runtime.has_value());
			CHECK(runtime->Kind == ScriptErrorKind::Runtime);
			CHECK((*sandbox)->Evaluate("return 1", *path).has_value());
			CHECK_FALSE((*sandbox)->GetLastError().has_value());
		}

		TEST_CASE("Watchdog: no error raised from a GC interrupt")
		{
			ScriptWatchdog watchdog;
			REQUIRE(watchdog.Enter(250, 1.0).has_value());
			CHECK_FALSE(watchdog.CheckInterrupt(0, 2.0));
			CHECK_FALSE(watchdog.CheckInterrupt(1, std::numeric_limits<double>::quiet_NaN()));
			CHECK_FALSE(watchdog.CheckInterrupt(99, 3.0));
			CHECK(watchdog.CheckInterrupt(-1, 2.0));
			watchdog.Leave();
		}

		TEST_CASE("Watchdog: OnCreate inside Instantiate inherits the outer deadline")
		{
			ScriptWatchdog watchdog;
			REQUIRE(watchdog.Enter(1000, 10.0).has_value());
			REQUIRE(watchdog.Enter(5000, 10.75).has_value());
			CHECK(watchdog.GetDepth() == 2);
			CHECK(watchdog.GetDeadlineSeconds() == 11.0);
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 10.99));
			CHECK(watchdog.CheckInterrupt(-1, 11.0));
			watchdog.Leave();
			CHECK(watchdog.CheckInterrupt(-1, 11.01));
			watchdog.Leave();
			CHECK(watchdog.GetDepth() == 0);
			CHECK(watchdog.GetDeadlineSeconds() == 0.0);
			// ScriptEngine's Instantiate callback test exercises the same nested ScriptCall scopes with real entities.
		}

		TEST_CASE("ScriptWatchdog: independent resumes restart the deadline and invalid entries do not alter depth")
		{
			ScriptWatchdog watchdog;
			REQUIRE(watchdog.Enter(250, 1.0).has_value());
			CHECK(watchdog.CheckInterrupt(-1, 1.25));
			watchdog.Leave();
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 2.0));
			REQUIRE(watchdog.Enter(250, 2.0).has_value());
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 2.1));
			CHECK_FALSE(watchdog.Enter(0, 2.1).has_value());
			CHECK_FALSE(watchdog.Enter(250, 1.0).has_value());
			CHECK(watchdog.GetDepth() == 1);
			watchdog.Leave();
		}

		TEST_CASE("ScriptWatchdog: callback budgets enforce the minimum and preserve exact test overrides")
		{
			const auto invalid = ScriptWatchdog::ResolveCallbackBudget(9, true);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(ScriptWatchdog::ResolveCallbackBudget(10, true).value_or(0) == 10);
			CHECK(ScriptWatchdog::ResolveCallbackBudget(9000, false).value_or(0) == 9000);
#if defined(ENGINE_DIST)
			CHECK(ScriptWatchdog::ResolveCallbackBudget(1000, false).value_or(0) == 5000);
#else
			CHECK(ScriptWatchdog::ResolveCallbackBudget(1000, false).value_or(0) == 1000);
#endif
			// Tests has no Dist target: exported FeatureTest must exercise the Dist policy too.
		}
	}

}
