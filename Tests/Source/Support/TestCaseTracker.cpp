#include "TestsPCH.h"
#include "Support/TestCaseTracker.h"

#include <mutex>

namespace Engine {

	namespace Test {

		namespace {

			struct TrackerState
			{
				// Held while a test case starts or ends and while a failure is reported, so a report from another
				// thread lands entirely inside the case it was meant for.
				std::mutex Mutex;
				std::optional<RunningTestCase> Running;
				uint64_t LastSerial = 0;
			};

		}

		namespace Utils {

			// Function-local static: reports may come from the static initialization or destruction of other files.
			static TrackerState& GetTrackerState()
			{
				static TrackerState s_State;
				return s_State;
			}

		}

		namespace {

			class TestCaseTrackerListener final : public TestCaseListener
			{
			public:
				explicit TestCaseTrackerListener(const doctest::ContextOptions& /*options*/)
				{
				}

				void test_case_start(const doctest::TestCaseData& testCase) override
				{
					TrackerState& state = Utils::GetTrackerState();
					std::scoped_lock lock(state.Mutex);
					RunningTestCase running;
					running.Name = testCase.m_name;
					running.File = testCase.m_file.c_str();
					running.Line = testCase.m_line;
					running.TimeoutSeconds = testCase.m_timeout;
					running.Serial = ++state.LastSerial;
					state.Running = std::move(running);
				}

				void test_case_end(const doctest::CurrentTestCaseStats& /*stats*/) override
				{
					TrackerState& state = Utils::GetTrackerState();
					std::scoped_lock lock(state.Mutex);
					state.Running.reset();
				}
			};

		}

		std::optional<RunningTestCase> GetRunningTestCase()
		{
			TrackerState& state = Utils::GetTrackerState();
			std::scoped_lock lock(state.Mutex);
			return state.Running;
		}

		bool ReportFailureToRunningTestCase(const std::string& file, uint32_t line, std::string_view message)
		{
			TrackerState& state = Utils::GetTrackerState();
			std::scoped_lock lock(state.Mutex);
			if (!state.Running.has_value())
				return false;

			// doctest's reporters copy the file name while they run, so a temporary is fine; FAIL_CHECK never throws.
			const std::string text(message);
			ADD_FAIL_CHECK_AT(file.c_str(), static_cast<int>(line), text);
			return true;
		}

	}

	REGISTER_LISTENER("EngineTestCaseTracker", 0, Test::TestCaseTrackerListener);

}
