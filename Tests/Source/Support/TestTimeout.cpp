#include "TestsPCH.h"
#include "Support/TestTimeout.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Support/TestCaseTracker.h"

#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace Engine {

	namespace Test {

		namespace {

			using SteadyClock = std::chrono::steady_clock;

			// Limits above this are treated as this (about a year), so a huge --test-timeout cannot overflow the clock.
			constexpr double MaxLimitSeconds = 3.0e7;
			// How long the expiry report may take before the process exits without it, should a hung test hold a lock
			// the report needs (the log's or doctest's).
			constexpr std::chrono::seconds ReportGracePeriod{ 10 };

			struct WatchdogState
			{
				std::mutex Mutex;
				std::condition_variable Changed;
				std::thread Thread;
				bool IsRunning = false;
				bool StopRequested = false;
				double DefaultSeconds = 0.0;

				// The armed test case: set when a case starts, cleared when it ends.
				bool IsArmed = false;
				uint64_t ArmCount = 0; // changes with every arming, so the thread notices a new case with the same deadline
				std::string CaseName;
				double LimitSeconds = 0.0;
				SteadyClock::time_point Deadline;
			};

		}

		namespace Utils {

			static WatchdogState& GetWatchdogState()
			{
				static WatchdogState s_State;
				return s_State;
			}

			// Logs the expiry, flushes the log and C stdio (doctest's report on a redirected stdout) and exits with code 5.
			// The report runs on its own thread so that the exit happens even if the report blocks.
			[[noreturn]] static void ExpireTestCase(const std::string& caseName, double limitSeconds)
			{
				std::mutex reportMutex;
				std::condition_variable reportDone;
				bool isReported = false;
				std::thread reporter([&caseName, limitSeconds, &reportMutex, &reportDone, &isReported]()
				{
					ENGINE_CORE_CRITICAL("Test case '{}' exceeded its time limit of {} s and was stopped", caseName, limitSeconds);
					Log::Flush();
					std::fflush(nullptr);
					std::scoped_lock lock(reportMutex);
					isReported = true;
					reportDone.notify_one();
				});

				{
					std::unique_lock lock(reportMutex);
					reportDone.wait_for(lock, ReportGracePeriod, [&isReported]()
					{
						return isReported;
					});
				}
				// std::_Exit ends every thread without running destructors, so `reporter` is never joined or destroyed.
				std::fflush(nullptr);
				std::_Exit(TestTimeoutExitCode);
			}

			static void RunWatchdog()
			{
				WatchdogState& state = GetWatchdogState();
				std::unique_lock lock(state.Mutex);
				while (!state.StopRequested)
				{
					if (!state.IsArmed)
					{
						state.Changed.wait(lock);
						continue;
					}

					const uint64_t armCount = state.ArmCount;
					const SteadyClock::time_point deadline = state.Deadline;
					const bool changed = state.Changed.wait_until(lock, deadline, [&state, armCount]()
					{
						return state.StopRequested || !state.IsArmed || state.ArmCount != armCount;
					});
					if (changed)
						continue;

					const std::string caseName = state.CaseName;
					const double limitSeconds = state.LimitSeconds;
					lock.unlock();
					ExpireTestCase(caseName, limitSeconds);
				}
			}

		}

		namespace {

			// Arms the watchdog for each test case and disarms it at the end. The overrides are defined in the class: the
			// naming lint recognizes doctest's snake_case names as overrides by their `override` declarator.
			class TestTimeoutListener final : public TestCaseListener
			{
			public:
				explicit TestTimeoutListener(const doctest::ContextOptions& /*options*/)
				{
				}

				void test_case_start(const doctest::TestCaseData& testCase) override
				{
					WatchdogState& state = Utils::GetWatchdogState();
					std::scoped_lock lock(state.Mutex);
					if (!state.IsRunning)
						return;

					const double limitSeconds = testCase.m_timeout > 0.0 ? testCase.m_timeout : state.DefaultSeconds;
					const std::chrono::duration<double> limit(std::min(limitSeconds, MaxLimitSeconds));
					state.IsArmed = true;
					++state.ArmCount;
					state.CaseName = testCase.m_name;
					state.LimitSeconds = limitSeconds;
					state.Deadline = SteadyClock::now() + std::chrono::duration_cast<SteadyClock::duration>(limit);
					state.Changed.notify_all();
				}

				void test_case_end(const doctest::CurrentTestCaseStats& /*stats*/) override
				{
					WatchdogState& state = Utils::GetWatchdogState();
					std::scoped_lock lock(state.Mutex);
					state.IsArmed = false;
					state.Changed.notify_all();
				}
			};

		}

		void StartTestTimeoutWatchdog(double defaultSeconds)
		{
			ENGINE_CORE_ASSERT(defaultSeconds > 0.0 && std::isfinite(defaultSeconds), "The default test timeout {} s is not positive",
				defaultSeconds);

			WatchdogState& state = Utils::GetWatchdogState();
			std::scoped_lock lock(state.Mutex);
			ENGINE_CORE_ASSERT(!state.IsRunning, "The test timeout watchdog is running already");
			state.IsRunning = true;
			state.StopRequested = false;
			state.IsArmed = false;
			state.DefaultSeconds = defaultSeconds;
			state.Thread = std::thread(&Utils::RunWatchdog);
		}

		void StopTestTimeoutWatchdog()
		{
			WatchdogState& state = Utils::GetWatchdogState();
			{
				std::scoped_lock lock(state.Mutex);
				if (!state.IsRunning)
					return;
				state.StopRequested = true;
				state.Changed.notify_all();
			}
			state.Thread.join();

			std::scoped_lock lock(state.Mutex);
			state.IsRunning = false;
			state.StopRequested = false;
			state.IsArmed = false;
		}

	}

	REGISTER_LISTENER("EngineTestTimeout", 0, Test::TestTimeoutListener);

}
