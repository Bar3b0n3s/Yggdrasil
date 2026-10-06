#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/GlfwLibrary.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

// Windowed child processes (Architecture §4.1, §15.2): a process is windowed or headless, never both, and the Tests
// binary runs headless (GLFW's null platform). A test that needs a real window (a native platform, a minimized window,
// later a swapchain) therefore runs in a child process, `Tests --windowed-child=<test case>`, spawned like a death test.
//
// The child target is an ordinary doctest test case in the ChildTargets suite, permanently skipped in the normal run
// (ADR 0004 section 5, extended by ADR 0005), so its body uses CHECK and REQUIRE as usual:
//
//     TEST_CASE("Window: a native window reports its framebuffer size"
//         * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
//     {
//         ... runs only in the windowed child ...
//     }
//
//     TEST_CASE("Window: a native window opens and closes")
//     {
//         ENGINE_CHECK_WINDOWED_CHILD("Window: a native window reports its framebuffer size");
//     }
//
// In the child the Tests main creates its ProcessContext with WindowMode::Windowed (the native platform), runs doctest
// with exactly that test case selected and skips lifted, and exits with doctest's result: 0 when every check passed, 1
// otherwise, 2 (UsageError) when the name selects no test case. Target names must not contain ',', '*', '?', '[' or ']',
// which doctest's --test-case filter interprets.
//
// Windowed children need a display: Windows and macOS have one, Linux CI runs the unit suite in an Xvfb display. On X11,
// iconifying and the window-manager side of resizing and focus only happen when a window manager runs: glfwIconifyWindow
// merely asks the window manager (WM_CHANGE_STATE), and GLFW reports iconified once the manager sets WM_STATE. The Linux
// CI jobs therefore start a lightweight window manager inside the Xvfb display (ADR 0005 decision 13); a target that
// minimizes, restores or resizes a native window relies on it.

namespace Engine {

	class Window;

	namespace Test {

		inline constexpr std::chrono::milliseconds DefaultWindowedChildTimeout{ 60000 };

		struct WindowedChildResult
		{
			int ExitCode = 0;
			std::string StandardOutput{}; // doctest's report
			std::string StandardError{};  // the child's log
		};

		// Child mode (`Tests --windowed-child=<testCase>`, called by the Tests main after it created the windowed
		// ProcessContext and the test harness): runs `testCase` through doctest and returns the exit code described above.
		[[nodiscard]] int RunWindowedChildTestCase(std::string_view testCase);

		// Parent mode: spawns GetTestOptions().ExecutablePath with "--windowed-child=<testCase>", "--user-data-dir=<directory>"
		// when `userDataDirectory` is not empty, and ChildProcessOption (MakeTestsChildSpecification, TestOptions.h), and
		// returns its exit code and output. Errors: Io when the child cannot be started; Timeout when it runs longer than
		// `timeout` (it is killed).
		[[nodiscard]] Result<WindowedChildResult> RunWindowedChild(std::string_view testCase,
			std::chrono::milliseconds timeout = DefaultWindowedChildTimeout, const std::filesystem::path& userDataDirectory = {});

		// The standard expectation, checked by ENGINE_CHECK_WINDOWED_CHILD: RunWindowedChild(testCase) succeeds and the child
		// exited with code 0. Returns "" when that holds, otherwise a description quoting the exit code and the child's
		// standard output and standard error.
		[[nodiscard]] std::string DescribeWindowedChildFailure(std::string_view testCase);

		// The platform GLFW runs on in a windowed process on this host: Win32, Cocoa or X11.
		[[nodiscard]] GlfwPlatform GetNativeGlfwPlatform();

		// In a windowed target: waits for OS events until window.IsMinimized() equals `minimized`, and returns whether it
		// does. Minimizing and restoring are asynchronous on X11 (the window manager acts) and on macOS (an animation), so
		// a target waits for them before it measures; the wait ends after 400 waits of 25 ms (10 s).
		[[nodiscard]] bool WaitUntilMinimized(Window& window, bool minimized);

	}

}

// The standard expectation of a windowed child (DescribeWindowedChildFailure) as one doctest CHECK at the call site.
#define ENGINE_CHECK_WINDOWED_CHILD(testCase) \
	do \
	{ \
		const std::string engineWindowedChildFailure = ::Engine::Test::DescribeWindowedChildFailure(testCase); \
		CHECK_MESSAGE(engineWindowedChildFailure.empty(), engineWindowedChildFailure); \
	} while (false)
