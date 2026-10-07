#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Platform/Process.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

// The Editor's --viewport-screenshot and --editor-screenshot options (Editor/EditorApp.h): thin callers of the screenshot
// capability (Renderer/ViewportCapture.h, ImGui/ImGuiScreenshot.h), which viewport.screenshot and editor.screenshot call
// too, through the same captures (ScreenshotMethodsTests.cpp, Tests/Automation/test_screenshot.py, the golden
// "ImGuiDemo"). The options belong to the Editor executable, whose sources are not linked into Tests, so these tests run it
// as a process; the capture functions themselves are tested in process (ViewportCaptureTests.cpp, ImGuiScreenshotTests.cpp).

namespace Engine {

	// A headless editor rendering with the Vulkan renderer, validated like every GPU test's process
	// (Test::GetGpuApplicationArguments), with its logs in `userData`.
	static Result<ProcessResult> RunScreenshotEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments)
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path editor, Test::GetBuiltExecutablePath("Editor"));
		const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
		arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		return Process::Run({ .Executable = std::move(editor), .Arguments = std::move(arguments) }, std::chrono::seconds(60));
	}

	// An editor without a renderer (the screenshot options' validation and their Unsupported path need no GPU).
	static Result<ProcessResult> RunLogicOnlyEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments,
		std::chrono::milliseconds timeout)
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path editor, Test::GetBuiltExecutablePath("Editor"));
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		return Process::Run({ .Executable = std::move(editor), .Arguments = std::move(arguments) }, timeout);
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorApp: the screenshot options need --frames, and the editor screenshot at least two")
		{
			Test::TempDirectory userData("EditorScreenshotOptions");
			const std::string png = Test::PathToUtf8(userData / "Screenshot.png");
			const std::array<std::vector<std::string>, 3> invalid = { {
				{ "--headless", "--renderer", "none", "--viewport-screenshot", png },
				{ "--headless", "--renderer", "none", "--editor-screenshot", png },
				{ "--headless", "--renderer", "none", "--frames", "1", "--editor-screenshot", png },
			} };
			for (const std::vector<std::string>& arguments : invalid)
			{
				const std::string option = arguments[3] == "--frames" ? arguments[5] : arguments[3];
				CAPTURE(option);
				const Result<ProcessResult> result = RunLogicOnlyEditor(userData, arguments, std::chrono::seconds(60));
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				CHECK(result->ExitCode == ExitCode::UsageError);
				CHECK(result->StandardError.contains("--frames"));
			}
		}

		TEST_CASE("EditorApp: screenshots of a --renderer none editor fail the run with exit code 1")
		{
			// §13.9: without a renderer the screenshot capability is Unsupported, which a --frames run reports as Failed.
			Test::TempDirectory userData("EditorScreenshotWithoutRenderer");
			const std::filesystem::path png = userData / "Viewport.png";
			const Result<ProcessResult> result = RunLogicOnlyEditor(userData,
				{ "--headless", "--renderer", "none", "--frames", "2", "--viewport-screenshot", Test::PathToUtf8(png) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Failed);
			CHECK(result->StandardError.contains("Cannot capture the viewport screenshot"));
			CHECK(result->StandardError.contains("Unsupported"));
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(png, error));
		}

		TEST_CASE("EditorApp: --viewport-screenshot writes the 640x360 clear-and-triangle view"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("ViewportScreenshot");
			const std::filesystem::path png = userData / "Viewport.png";
			const Result<ProcessResult> result =
				RunScreenshotEditor(userData, { "--headless", "--frames", "2", "--viewport-screenshot", Test::PathToUtf8(png) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
			const Result<Image> image = ReadPng(png);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(image->Width == DefaultViewportScreenshotWidth);
			CHECK(image->Height == DefaultViewportScreenshotHeight);
			// The triangle covers the centre; the clear colour the corners.
			const std::span<const std::byte> centreRow = image->GetRow(image->Height / 2);
			const std::span<const std::byte> topRow = image->GetRow(0);
			const size_t centre = static_cast<size_t>(image->Width / 2) * 4;
			const auto centrePixel = centreRow.begin() + static_cast<std::ptrdiff_t>(centre);
			CHECK_FALSE(std::equal(centrePixel, centrePixel + 3, topRow.begin()));
		}

		TEST_CASE("EditorApp: --editor-screenshot writes the editor UI at the window's framebuffer size"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorScreenshot");
			const std::filesystem::path png = userData / "Editor.png";
			const Result<ProcessResult> result =
				RunScreenshotEditor(userData, { "--headless", "--frames", "3", "--editor-screenshot", Test::PathToUtf8(png) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
			const Result<Image> image = ReadPng(png);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			// The headless editor's window has the default WindowSpecification size on the null platform (content scale 1).
			CHECK(image->Width == 1600);
			CHECK(image->Height == 900);
			// The demo window is drawn over the clear colour (FrameClearColor, opaque black).
			const bool onlyClearColor = std::ranges::all_of(image->Pixels, [](std::byte value)
			{
				return value == std::byte{ 0 } || value == std::byte{ 255 };
			});
			CHECK_FALSE(onlyClearColor);
		}
	}

}
