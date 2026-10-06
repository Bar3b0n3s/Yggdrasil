#include "TestsPCH.h"

#include "Engine/Core/Assert.h"

#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Support/DeathTest.h"

#include <entt/entity/registry.hpp>

namespace Engine {

	// The line of the assertion in the Core/AssertHandlerReceivesTheReport child, set right before it fails.
	static uint32_t s_ExpectedAssertLine = 0;

	static size_t CountTo(size_t limit)
	{
		return limit;
	}

	static void IgnoreAssert(const AssertInfo& /*info*/)
	{
	}

	// Describes the report it receives, then ends the process as every handler must.
	static void DescribeAssertHandler(const AssertInfo& info)
	{
		const std::string_view file = info.File.substr(info.File.find_last_of("/\\") + 1);
		ENGINE_CORE_CRITICAL("Handler saw {} '{}' with '{}' (client {}, line matches {}, file {}, function set {})",
			AssertKindToString(info.Kind), info.Expression, info.Message, info.IsClient, info.Line == s_ExpectedAssertLine, file,
			!info.Function.empty());
		FatalError(FatalErrorKind::Assert, "the describing handler finished");
	}

	// Runs inside FatalError after the default assert handler logged its report: describes that report from the ring
	// buffer. FatalError logged its own line after it, so the report is the second-to-last entry.
	static void DescribeLoggedAssertReport(FatalErrorKind /*kind*/, std::string_view /*message*/)
	{
		const std::vector<LogEntry> entries = Log::GetRingBuffer().ReadLast(2);
		if (entries.size() != 2)
			return;

		const LogEntry& report = entries[0];
		const std::string_view file = std::string_view(report.File).substr(report.File.find_last_of("/\\") + 1);
		ENGINE_CORE_CRITICAL("Logged report: {} {} {}: {}", LogChannelToString(report.Logger), LogLevelToString(report.Level), file,
			report.Message);
	}

	ENGINE_DEATH_TEST("Core/AssertFires")
	{
		const std::vector<int> values = { 1, 2 };
		ENGINE_CORE_ASSERT(values.size() == 3, "Expected {} values, got {}", 3, values.size());
	}

	ENGINE_DEATH_TEST("Core/ClientAssertFires")
	{
		const size_t count = CountTo(5);
		ENGINE_ASSERT(count < 5, "Client count {} is too large", count);
	}

	ENGINE_DEATH_TEST("Core/VerifyFires")
	{
		const size_t count = CountTo(2);
		ENGINE_CORE_VERIFY(count == 0, "Verify saw {}", count);
	}

	ENGINE_DEATH_TEST("Core/ClientVerifyFires")
	{
		const size_t count = CountTo(8);
		ENGINE_VERIFY(count == 1, "Client verify saw {}", count);
	}

	ENGINE_DEATH_TEST("Core/UnreachableFires")
	{
		ENGINE_UNREACHABLE("Reached the unreachable branch {}", 17);
	}

	ENGINE_DEATH_TEST("Core/EnttAssertRoutesToEngine")
	{
		entt::registry registry;
		const entt::entity entity = registry.create();
		registry.destroy(entity);
		registry.emplace<int>(entity, 1);
	}

	ENGINE_DEATH_TEST("Core/AssertHandlerReceivesTheReport")
	{
		SetAssertHandler(&DescribeAssertHandler);
		const size_t value = CountTo(3);
		s_ExpectedAssertLine = static_cast<uint32_t>(__LINE__ + 1);
		ENGINE_ASSERT(value == 4, "Value is {}", value);
	}

	ENGINE_DEATH_TEST("Core/ReturningAssertHandlerStillExits")
	{
		SetAssertHandler(&IgnoreAssert);
		const size_t value = CountTo(6);
		ENGINE_CORE_VERIFY(value == 7, "Returning handler saw {}", value);
	}

	ENGINE_DEATH_TEST("Core/DefaultAssertHandlerLogsToTheAppLogger")
	{
		SetAssertHandler(nullptr);
		SetFatalErrorHandler(&DescribeLoggedAssertReport);
		const size_t count = CountTo(9);
		ENGINE_ASSERT(count < 9, "Default handler count {}", count);
	}

	ENGINE_DEATH_TEST("Core/DefaultAssertHandlerLogsToTheEngineLogger")
	{
		SetAssertHandler(nullptr);
		SetFatalErrorHandler(&DescribeLoggedAssertReport);
		ENGINE_UNREACHABLE("Default handler reached {}", "the end");
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Assert: Core/AssertFires exits with code 4 and the message")
		{
			ENGINE_CHECK_DEATH("Core/AssertFires", "Expected 3 values, got 2");
			ENGINE_CHECK_DEATH("Core/AssertFires", "Assertion failed: values.size() == 3");
		}

		TEST_CASE("Assert: ENGINE_ASSERT fires like ENGINE_CORE_ASSERT")
		{
			ENGINE_CHECK_DEATH("Core/ClientAssertFires", "Assertion failed: count < 5: Client count 5 is too large");
		}

		TEST_CASE("Assert: a failed ENGINE_CORE_VERIFY exits with code 4 and the message")
		{
			ENGINE_CHECK_DEATH("Core/VerifyFires", "Verify failed: count == 0: Verify saw 2");
		}

		TEST_CASE("Assert: a failed ENGINE_VERIFY exits with code 4 and the message")
		{
			ENGINE_CHECK_DEATH("Core/ClientVerifyFires", "Verify failed: count == 1: Client verify saw 8");
		}

		TEST_CASE("Assert: ENGINE_UNREACHABLE exits with code 4 and the message")
		{
			ENGINE_CHECK_DEATH("Core/UnreachableFires", "Unreachable code reached: Reached the unreachable branch 17");
		}

		TEST_CASE("Assert: EnTT assertions go through the engine handler")
		{
			ENGINE_CHECK_DEATH("Core/EnttAssertRoutesToEngine", "Invalid entity");
		}

		TEST_CASE("Assert: the installed handler receives the kind, expression, message and location")
		{
			ENGINE_CHECK_DEATH("Core/AssertHandlerReceivesTheReport",
				"Handler saw Assert 'value == 4' with 'Value is 3' "
				"(client true, line matches true, file AssertTests.cpp, function set true)");
		}

		TEST_CASE("Assert: a handler that returns still ends the process with code 4")
		{
			ENGINE_CHECK_DEATH("Core/ReturningAssertHandlerStillExits",
				"Fatal error (Assert): Verify failed: value == 7: Returning handler saw 6");
		}

		TEST_CASE("Assert: the default handler logs client assertions to the App logger and exits with code 4")
		{
			ENGINE_CHECK_DEATH("Core/DefaultAssertHandlerLogsToTheAppLogger",
				"Logged report: App Critical AssertTests.cpp: Assertion failed: count < 9: Default handler count 9");
			ENGINE_CHECK_DEATH("Core/DefaultAssertHandlerLogsToTheAppLogger", "Fatal error (Assert): Assertion failed: count < 9");
		}

		TEST_CASE("Assert: the default handler logs engine assertions to the Engine logger")
		{
			ENGINE_CHECK_DEATH("Core/DefaultAssertHandlerLogsToTheEngineLogger",
				"Logged report: Engine Critical AssertTests.cpp: Unreachable code reached: Default handler reached the end");
		}

		TEST_CASE("Assert: a passing assertion evaluates its condition once and continues")
		{
			int evaluations = 0;
			auto condition = [&evaluations]()
			{
				++evaluations;
				return true;
			};

			ENGINE_CORE_ASSERT(condition(), "Never shown");
			ENGINE_CORE_VERIFY(condition(), "Never shown either");

			// Tests run in Debug and Release only, where both are evaluated.
			CHECK(evaluations == 2);
		}

		TEST_CASE("Assert: the message arguments are evaluated only when the assertion fails")
		{
			int formatted = 0;
			auto describe = [&formatted]()
			{
				++formatted;
				return std::string("details");
			};

			ENGINE_CORE_ASSERT(formatted == 0, "{}", describe());
			ENGINE_ASSERT(formatted == 0, "{}", describe());
			ENGINE_CORE_VERIFY(formatted == 0, "{}", describe());
			ENGINE_VERIFY(formatted == 0, "{}", describe());

			CHECK(formatted == 0);
		}

		TEST_CASE("Assert: SetAssertHandler returns the previous handler and nullptr restores the default")
		{
			const AssertHandler original = GetAssertHandler();

			const AssertHandler previous = SetAssertHandler(&IgnoreAssert);
			CHECK(previous == original);
			CHECK(GetAssertHandler() == &IgnoreAssert);

			CHECK(SetAssertHandler(nullptr) == &IgnoreAssert);
			CHECK(GetAssertHandler() == &DefaultAssertHandler);

			SetAssertHandler(original);
			CHECK(GetAssertHandler() == original);
		}

		TEST_CASE("Assert: FormatAssertInfo renders the documented single line")
		{
			AssertInfo info;
			info.Kind = AssertKind::Assert;
			info.Expression = "index < size";
			info.Message = "Index 7 out of range";
			info.File = "Scene.cpp";
			info.Line = 42;
			info.Function = "GetEntity";
			CHECK(FormatAssertInfo(info) == "Assertion failed: index < size: Index 7 out of range (Scene.cpp:42, GetEntity)");

			info.Kind = AssertKind::Verify;
			CHECK(FormatAssertInfo(info) == "Verify failed: index < size: Index 7 out of range (Scene.cpp:42, GetEntity)");

			info.Message = {};
			CHECK(FormatAssertInfo(info) == "Verify failed: index < size (Scene.cpp:42, GetEntity)");

			info.Kind = AssertKind::Unreachable;
			info.Expression = {};
			info.Message = "Unknown AudioBus 9";
			CHECK(FormatAssertInfo(info) == "Unreachable code reached: Unknown AudioBus 9 (Scene.cpp:42, GetEntity)");
		}

		TEST_CASE("Assert: FormatAssertInfo leaves out the parts that are not set")
		{
			AssertInfo info;
			info.Kind = AssertKind::Unreachable;
			info.File = "Audio.cpp";
			info.Line = 7;
			CHECK(FormatAssertInfo(info) == "Unreachable code reached (Audio.cpp:7)");

			// ENGINE_UNREACHABLE has no condition, so an expression is never shown for it.
			info.Expression = "ignored";
			info.Function = "Mix";
			CHECK(FormatAssertInfo(info) == "Unreachable code reached (Audio.cpp:7, Mix)");

			info.Kind = AssertKind::Assert;
			info.Expression = {};
			info.Message = "No expression";
			CHECK(FormatAssertInfo(info) == "Assertion failed: No expression (Audio.cpp:7, Mix)");
		}

		TEST_CASE("Assert: AssertKindToString names every kind")
		{
			CHECK(AssertKindToString(AssertKind::Assert) == "Assert");
			CHECK(AssertKindToString(AssertKind::Verify) == "Verify");
			CHECK(AssertKindToString(AssertKind::Unreachable) == "Unreachable");
		}
	}

}
