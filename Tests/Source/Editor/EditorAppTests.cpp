#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Process.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

// The editor executable as a whole process (Roadmap M2): RunApplication with the editor's (for now empty) application.
// Every run gets --user-data-dir, so the editor's log file goes to the test's temporary directory.

namespace Engine {

	static Result<ProcessResult> RunEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments,
		std::chrono::milliseconds timeout)
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path editor, Test::GetBuiltExecutablePath("Editor"));
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		return Process::Run({ .Executable = std::move(editor), .Arguments = std::move(arguments) }, timeout);
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorApp: --headless --frames 10 exits 0 using ManualClock")
		{
			Test::TempDirectory userData("EditorHeadless");
			const Result<ProcessResult> result = RunEditor(userData, { "--headless", "--frames", "10" }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			// Headless play without lockstep is paced at FixedHz (§4.2), never run flat out.
			CHECK(result->StandardError.contains("Frame loop started: Manual clock, 60 Hz, throttled"));
			CHECK(result->StandardError.contains("Process context: Glfw initialized"));
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(userData / ENGINE_PRODUCT_NAME / "Logs" / "Editor.log", error));
		}

		TEST_CASE("EditorApp: a windowed editor opens and closes within a 10 second timeout")
		{
			// A native window on the system clock; Linux CI provides a display through Xvfb.
			Test::TempDirectory userData("EditorWindowed");
			const Result<ProcessResult> result = RunEditor(userData, { "--frames", "30" }, std::chrono::seconds(10));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->StandardError.contains(
				std::format("Process context ready: Windowed on GLFW's {} platform", GlfwPlatformToString(Test::GetNativeGlfwPlatform()))));
			CHECK(result->StandardError.contains(std::format("Engine context: window '{}' created", ENGINE_PRODUCT_NAME)));
			// The frame loop never throttles a windowed run; presenting paces it (M5).
			CHECK(result->StandardError.contains("Frame loop started: System clock, 60 Hz, unthrottled"));
		}
	}

}
