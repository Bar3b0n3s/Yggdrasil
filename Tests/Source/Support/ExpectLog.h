#pragma once

#include "Engine/Core/RingBufferSink.h"

#include <cstdint>
#include <source_location>
#include <string_view>

// Declared log expectations (Architecture §4.4 test policy): a test case fails when it logs an Error or Critical entry
// it did not declare, and a declared entry must actually appear.

namespace Engine {

	namespace Test {

		// Registers the Log listener behind ExpectLog (Log::AddListener). From then on, every Error or Critical entry, from
		// any thread, that no live ExpectLog matches fails the running test case immediately (a doctest failure quoting the
		// level, logger and message); entries logged while no test case runs are not checked. The Tests main installs it once
		// before doctest runs and removes it afterwards.
		void InstallExpectLogListener();
		void UninstallExpectLogListener();

		// Declares, for its lifetime, that log entries of exactly `level` whose message contains `substring` (case-sensitive)
		// are expected:
		//     Test::ExpectLog expected(LogLevel::Error, "no such file");
		//     CHECK_FALSE(LoadSomething("missing.json"));
		// Every matching entry logged while it is alive, on any thread, counts for it (several live ExpectLogs may match the
		// same entry). Its destructor fails the test case, at `location`, when no entry matched. Expectations at Trace, Info
		// and Warn are allowed: they only count, since undeclared entries below Error never fail a test. Not copyable or
		// movable; create it on the test's stack.
		class ExpectLog
		{
		public:
			ExpectLog(LogLevel level, std::string_view substring, const std::source_location& location = std::source_location::current());
			~ExpectLog();

			ExpectLog(const ExpectLog&) = delete;
			ExpectLog& operator=(const ExpectLog&) = delete;
			ExpectLog(ExpectLog&&) = delete;
			ExpectLog& operator=(ExpectLog&&) = delete;

			// How many entries have matched so far. Thread-safe.
			[[nodiscard]] uint32_t GetMatchCount() const;
		};

	}

}
