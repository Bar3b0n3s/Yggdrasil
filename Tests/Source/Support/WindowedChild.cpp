#include "TestsPCH.h"
#include "Support/WindowedChild.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Window.h"
#include "Support/TestCaseTracker.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <atomic>
#include <format>
#include <utility>
#include <vector>

namespace Engine {

	namespace Test {

		namespace Utils {

			// The number of test cases the last doctest run selected (its filters and skips applied), recorded by the
			// listener below when the run ends (process-level state of the Tests binary). RunWindowedChildTestCase reads it
			// to tell a name that selects nothing from a case that passed.
			static std::atomic<uint32_t> s_LastRunSelectedTestCases{ 0 };

		}

		namespace {

			class WindowedChildRunListener final : public TestCaseListener
			{
			public:
				explicit WindowedChildRunListener(const doctest::ContextOptions& /*options*/)
				{
				}

				void test_run_end(const doctest::TestRunStats& stats) override
				{
					Utils::s_LastRunSelectedTestCases.store(stats.numTestCasesPassingFilters);
				}
			};

		}

		int RunWindowedChildTestCase(std::string_view testCase)
		{
			const std::string name(testCase);
			doctest::Context context;
			context.setOption("test-case", name.c_str()); // the exact name: targets contain no filter characters
			context.setOption("no-skip", true);           // the target is permanently skipped in the normal run
			Utils::s_LastRunSelectedTestCases.store(0);
			const int result = context.run();
			if (Utils::s_LastRunSelectedTestCases.load() == 0)
			{
				ENGINE_CORE_ERROR("The windowed child selects no test case named '{}'", name);
				return ExitCode::UsageError;
			}
			return result;
		}

		Result<WindowedChildResult> RunWindowedChild(std::string_view testCase, std::chrono::milliseconds timeout,
			const std::filesystem::path& userDataDirectory)
		{
			std::vector<std::string> arguments = { std::format("--windowed-child={}", testCase) };
			if (!userDataDirectory.empty())
				arguments.push_back("--user-data-dir=" + PathToUtf8(userDataDirectory));
			ENGINE_TRY_ASSIGN(ProcessResult child, Process::Run(MakeTestsChildSpecification(std::move(arguments)), timeout));
			return WindowedChildResult{
				.ExitCode = child.ExitCode,
				.StandardOutput = std::move(child.StandardOutput),
				.StandardError = std::move(child.StandardError),
			};
		}

		std::string DescribeWindowedChildFailure(std::string_view testCase)
		{
			const Result<WindowedChildResult> child = RunWindowedChild(testCase);
			if (!child.has_value())
				return std::format("The windowed child '{}' did not run: {}", testCase, child.error());
			if (child->ExitCode == 0)
				return {};
			return std::format("The windowed child '{}' exited with code {} instead of 0; its standard output was:\n{}\nits "
							   "standard error was:\n{}",
				testCase, child->ExitCode, child->StandardOutput, child->StandardError);
		}

		GlfwPlatform GetNativeGlfwPlatform()
		{
#if defined(ENGINE_PLATFORM_WINDOWS)
			return GlfwPlatform::Win32;
#elif defined(ENGINE_PLATFORM_MACOS)
			return GlfwPlatform::Cocoa;
#elif defined(ENGINE_PLATFORM_LINUX)
			return GlfwPlatform::X11;
#endif
		}

		bool WaitUntilMinimized(Window& window, bool minimized)
		{
			constexpr int MaxWaits = 400;
			constexpr double WaitSeconds = 0.025;
			for (int wait = 0; wait < MaxWaits && window.IsMinimized() != minimized; ++wait)
				window.WaitEventsTimeout(WaitSeconds);
			return window.IsMinimized() == minimized;
		}

	}

	REGISTER_LISTENER("EngineWindowedChildRun", 1, Test::WindowedChildRunListener);

}
