#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Platform/Process.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <chrono>
#include <format>
#include <string>
#include <vector>

// The editor's viewport (Editor/EditorApp.h "Viewport"; Roadmap M7): the view of the scene renderer drawn under the UI and
// blitted into the frame's target, whose format is the swapchain's (BGRA8_UNORM) in a windowed editor and RGBA8_UNORM in a
// headless one. Headless rendering editors run it in every rendering test (the screenshot suites, the golden "ImGuiDemo");
// this case runs it windowed, validated like every GPU test's process (--expect-no-gpu-errors). The views themselves are
// tested through their screenshots (EditorScreenshotOptionsTests.cpp, Tests/Automation/test_screenshot.py and
// test_game_view.py).

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorApp: a windowed rendering editor draws its viewport without GPU errors" * doctest::test_suite(Test::GpuSuite))
		{
			// A native window on the system clock (Linux CI provides a display through Xvfb), rendering for 30 frames.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorWindowedViewport");
			const Result<std::filesystem::path> editor = Test::GetBuiltExecutablePath("Editor");
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			// Windowed, so without --audio-device none it would open the machine's audio device (§15.1 T1).
			std::vector<std::string> arguments =
				Test::WithoutAudioDevice({ "--frames", "30", "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) });
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> result =
				Process::Run({ .Executable = *editor, .Arguments = std::move(arguments) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
			CHECK(result->StandardError.contains("Audio engine: no device"));
			CHECK_FALSE(result->StandardError.contains("Audio engine: device"));
			CHECK_FALSE(result->StandardError.contains("No audio device could be created"));
			// §8.5: the scene renderer's pipelines are created once at startup and their count is logged.
			CHECK(result->StandardError.contains(
				std::format("Created the scene renderer's {} pipelines", SceneRendererPipelines::GetLayoutDescriptions().size())));
		}
	}

}
