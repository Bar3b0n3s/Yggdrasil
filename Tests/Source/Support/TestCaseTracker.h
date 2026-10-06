#pragma once

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Which doctest test case is running, for the support code that reacts on other threads: the ExpectLog listener (log
// entries from any thread), the recording assert handler (asserts on any thread) and the timeout watchdog. A doctest
// listener registered in TestCaseTracker.cpp keeps it current in every doctest run of the Tests binary. A death-test
// child runs no doctest, so no test case is ever running there.

namespace Engine {

	namespace Test {

		struct RunningTestCase
		{
			std::string Name;
			std::string File;
			uint32_t Line = 0;
			double TimeoutSeconds = 0.0; // the doctest::timeout decorator; 0 when the case has none
			uint64_t Serial = 0;         // unique per started test case in this process, increasing from 1
		};

		// The test case doctest is running (also between the runs of its subcases), or nullopt. Thread-safe.
		[[nodiscard]] std::optional<RunningTestCase> GetRunningTestCase();

		// Adds a non-fatal doctest failure (like FAIL_CHECK) with `message` at `file`:`line` to the running test case and
		// returns true; returns false and reports nothing when no test case is running. Thread-safe, from any thread: the
		// tracker cannot move to another test case during the call, so a failure is never attributed to the next case.
		bool ReportFailureToRunningTestCase(const std::string& file, uint32_t line, std::string_view message);

		// Base of the support code's doctest listeners (REGISTER_LISTENER): every IReporter callback is a no-op, so a
		// listener overrides only the events it needs. doctest creates listeners when a run starts, calls them on the
		// thread that runs doctest and destroys them when the run ends.
		class TestCaseListener : public doctest::IReporter
		{
		public:
			void report_query(const doctest::QueryData& /*data*/) override {}
			void test_run_start() override {}
			void test_run_end(const doctest::TestRunStats& /*stats*/) override {}
			void test_case_start(const doctest::TestCaseData& /*testCase*/) override {}
			void test_case_reenter(const doctest::TestCaseData& /*testCase*/) override {}
			void test_case_end(const doctest::CurrentTestCaseStats& /*stats*/) override {}
			void test_case_exception(const doctest::TestCaseException& /*exception*/) override {}
			void subcase_start(const doctest::SubcaseSignature& /*signature*/) override {}
			void subcase_end() override {}
			void log_assert(const doctest::AssertData& /*data*/) override {}
			void log_message(const doctest::MessageData& /*data*/) override {}
			void test_case_skipped(const doctest::TestCaseData& /*testCase*/) override {}
		};

	}

}
