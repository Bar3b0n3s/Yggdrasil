#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Graphics/VulkanDispatch.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Process.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

// The editor executable as a whole process (Roadmap M2): RunApplication with the editor's application. Every run gets
// --user-data-dir, so the editor's log file goes to the test's temporary directory. Runs that need no GPU pass
// --renderer none; the ones that render are in the GPU suite (EditorFaultInjectionTests.cpp, EditorScreenshotOptionsTests.cpp).

namespace Engine {

	static Result<ProcessResult> RunEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments,
		std::chrono::milliseconds timeout, std::vector<std::pair<std::string, std::string>> environment = {})
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path editor, Test::GetBuiltExecutablePath("Editor"));
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		return Process::Run(
			{ .Executable = std::move(editor), .Arguments = std::move(arguments), .Environment = std::move(environment) }, timeout);
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorApp: --headless --frames 10 exits 0 using ManualClock")
		{
			Test::TempDirectory userData("EditorHeadless");
			const Result<ProcessResult> result =
				RunEditor(userData, { "--headless", "--renderer", "none", "--frames", "10" }, std::chrono::seconds(60));
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
			const Result<ProcessResult> result = RunEditor(userData, { "--renderer", "none", "--frames", "30" }, std::chrono::seconds(10));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->StandardError.contains(
				std::format("Process context ready: Windowed on GLFW's {} platform", GlfwPlatformToString(Test::GetNativeGlfwPlatform()))));
			CHECK(result->StandardError.contains(std::format("Engine context: window '{}' created", ENGINE_PRODUCT_NAME)));
			// The frame loop never throttles a windowed run; presenting paces it (M5).
			CHECK(result->StandardError.contains("Frame loop started: System clock, 60 Hz, unthrottled"));
		}

		TEST_CASE("EditorApp: with ENGINE_VULKAN_LOADER=missing the editor exits 3 with the loader message")
		{
			// Roadmap M5: a missing loader is an initialization failure (exit code 3) with a readable message (§8.1). Headless,
			// so no error dialog can block the run.
			Test::TempDirectory userData("EditorNoLoader");
			const Result<ProcessResult> result = RunEditor(userData, { "--headless", "--frames", "1" }, std::chrono::seconds(60),
				{ { std::string(VulkanLoaderEnvironmentVariable), "missing" } });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::InitFailed);
			CHECK(result->StandardError.contains(NoVulkanLoaderMessage));
			CHECK_FALSE(result->StandardError.contains("Process context: Glfw initialized"));
		}

		TEST_CASE("EditorApp: an ENGINE_VULKAN_LOADER value other than missing exits 3 naming the variable")
		{
			Test::TempDirectory userData("EditorBadLoaderHook");
			const Result<ProcessResult> result = RunEditor(userData, { "--headless", "--frames", "1" }, std::chrono::seconds(60),
				{ { std::string(VulkanLoaderEnvironmentVariable), "absent" } });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::InitFailed);
			CHECK(result->StandardError.contains(VulkanLoaderEnvironmentVariable));
			CHECK(result->StandardError.contains("absent"));
		}

		TEST_CASE("EditorApp: the --renderer none editor needs no Vulkan loader")
		{
			// Logic-only runs (§13.9) never touch the loader, so even a simulated missing one does not matter.
			Test::TempDirectory userData("EditorNoRendererNoLoader");
			const Result<ProcessResult> result = RunEditor(userData, { "--headless", "--renderer", "none", "--frames", "1" },
				std::chrono::seconds(60), { { std::string(VulkanLoaderEnvironmentVariable), "missing" } });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK_FALSE(result->StandardError.contains("Process context: VulkanLoader initialized"));
		}
	}

}
