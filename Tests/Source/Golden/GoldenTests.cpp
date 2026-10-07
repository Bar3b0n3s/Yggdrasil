#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Process.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/GoldenImage.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

// The golden images of the Graphics foundation (Roadmap M5, Architecture §15.4): "Triangle", the clear-and-triangle view
// rendered headless at 640x360 through the viewport capture (viewport.screenshot's path), and "ImGuiDemo", the editor UI
// (Dear ImGui's demo window) through the editor screenshot path. They run in the golden stage (Release) on this machine's
// device class; Test.py --update-golden writes candidates for review.

namespace Engine {

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: Triangle")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<Scope<ViewportCapture>> capture = ViewportCapture::Create(device, gpu.GetPipelines());
			REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
			const Result<Image> image = (*capture)->Capture({ .Width = 640, .Height = 360 });
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			ENGINE_CHECK_GOLDEN("Triangle", *image, device.GetInfo().DeviceClass);
		}

		TEST_CASE("Golden: ImGuiDemo")
		{
			// The editor's screenshot path (editor.screenshot's function) in a headless editor; the device class comes from a
			// fixture device, which is created and destroyed before the editor process starts (the probe of
			// Test::ProbeGpuForProcess, which this test needs the device's data from).
			std::string deviceClass;
			{
				Test::HeadlessGpuFixture gpu;
				ENGINE_REQUIRE_GPU(gpu);
				deviceClass = gpu.GetDevice().GetInfo().DeviceClass;
			}
			Test::TempDirectory userData("GoldenImGuiDemo");
			const std::filesystem::path png = userData / "ImGuiDemo.png";
			const Result<std::filesystem::path> editor = Test::GetBuiltExecutablePath("Editor");
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			std::vector<std::string> arguments = {
				"--headless",
				"--frames",
				"3",
				"--editor-screenshot",
				Test::PathToUtf8(png),
				"--user-data-dir=" + Test::PathToUtf8(userData.GetPath()),
			};
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> result =
				Process::Run({ .Executable = *editor, .Arguments = std::move(arguments) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			REQUIRE(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
			const Result<Image> image = ReadPng(png);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			ENGINE_CHECK_GOLDEN("ImGuiDemo", *image, deviceClass);
		}
	}

}
