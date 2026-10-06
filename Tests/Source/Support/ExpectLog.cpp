#include "TestsPCH.h"
#include "Support/ExpectLog.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Support/TestCaseTracker.h"

#include <exception>
#include <mutex>

// The state of every live ExpectLog lives in one registry, keyed by the object, and is only touched under the
// registry's mutex: the listener runs on whichever thread logs, and it must never see an expectation half destroyed.

namespace Engine {

	namespace Test {

		namespace {

			struct Expectation
			{
				const ExpectLog* Owner = nullptr;
				LogLevel Level = LogLevel::Error;
				std::string Substring;
				std::string File; // the declaring statement, where an unmet expectation is reported
				uint32_t Line = 0;
				uint32_t MatchCount = 0;
				int UncaughtExceptions = 0; // std::uncaught_exceptions() at construction
			};

			struct ExpectLogRegistry
			{
				std::mutex Mutex;
				std::vector<Expectation> Expectations;
				uint64_t ListenerID = 0; // Log listener of InstallExpectLogListener; written by the Tests main only
			};

		}

		namespace Utils {

			static ExpectLogRegistry& GetExpectLogRegistry()
			{
				static ExpectLogRegistry s_Registry;
				return s_Registry;
			}

			// Counts `entry` for every live expectation it matches and returns whether one did.
			static bool CountMatches(const LogEntry& entry)
			{
				ExpectLogRegistry& registry = GetExpectLogRegistry();
				std::scoped_lock lock(registry.Mutex);
				bool matched = false;
				for (Expectation& expectation : registry.Expectations)
				{
					if (expectation.Level == entry.Level && entry.Message.contains(expectation.Substring))
					{
						++expectation.MatchCount;
						matched = true;
					}
				}
				return matched;
			}

			// The Log listener: undeclared Error and Critical entries fail the running test case. It never logs.
			static void OnLogEntry(const LogEntry& entry)
			{
				const bool declared = CountMatches(entry);
				if (declared || entry.Level < LogLevel::Error)
					return;

				const std::string message = std::format("Undeclared {} entry from the {} logger: \"{}\" (declare it with Test::ExpectLog)",
					LogLevelToString(entry.Level), LogChannelToString(entry.Logger), entry.Message);
				ReportFailureToRunningTestCase(entry.File, entry.Line, message); // entries between test cases are not checked
			}

		}

		void InstallExpectLogListener()
		{
			ExpectLogRegistry& registry = Utils::GetExpectLogRegistry();
			ENGINE_CORE_ASSERT(registry.ListenerID == 0, "The ExpectLog listener is installed already");
			registry.ListenerID = Log::AddListener([](const LogEntry& entry)
			{
				Utils::OnLogEntry(entry);
			});
		}

		void UninstallExpectLogListener()
		{
			ExpectLogRegistry& registry = Utils::GetExpectLogRegistry();
			if (registry.ListenerID == 0)
				return;
			Log::RemoveListener(registry.ListenerID);
			registry.ListenerID = 0;
		}

		ExpectLog::ExpectLog(LogLevel level, std::string_view substring, const std::source_location& location)
		{
			Expectation expectation;
			expectation.Owner = this;
			expectation.Level = level;
			expectation.Substring = std::string(substring);
			expectation.File = location.file_name();
			expectation.Line = location.line();
			expectation.UncaughtExceptions = std::uncaught_exceptions();

			ExpectLogRegistry& registry = Utils::GetExpectLogRegistry();
			std::scoped_lock lock(registry.Mutex);
			registry.Expectations.push_back(std::move(expectation));
		}

		ExpectLog::~ExpectLog()
		{
			Expectation expectation;
			{
				ExpectLogRegistry& registry = Utils::GetExpectLogRegistry();
				std::scoped_lock lock(registry.Mutex);
				const auto found = std::ranges::find(registry.Expectations, this, &Expectation::Owner);
				if (found == registry.Expectations.end())
					return;
				expectation = std::move(*found);
				registry.Expectations.erase(found);
			}

			// A test case unwinding from a failed REQUIRE has already failed; a missing entry is a consequence.
			if (expectation.MatchCount > 0 || std::uncaught_exceptions() > expectation.UncaughtExceptions)
				return;

			const std::string message = std::format("Expected a {} entry containing \"{}\", but none was logged",
				LogLevelToString(expectation.Level), expectation.Substring);
			if (!ReportFailureToRunningTestCase(expectation.File, expectation.Line, message))
				ENGINE_CORE_ERROR("{} ({}:{})", message, expectation.File, expectation.Line);
		}

		uint32_t ExpectLog::GetMatchCount() const
		{
			ExpectLogRegistry& registry = Utils::GetExpectLogRegistry();
			std::scoped_lock lock(registry.Mutex);
			const auto found = std::ranges::find(registry.Expectations, this, &Expectation::Owner);
			return found != registry.Expectations.end() ? found->MatchCount : 0;
		}

	}

}
