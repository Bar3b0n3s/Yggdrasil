#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Process.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/GoldenImage.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>

// The golden images of the Graphics foundation (Roadmap M5, Architecture §15.4): "Triangle", the clear-and-triangle view
// rendered headless at 640x360 through the viewport capture (viewport.screenshot's path), and "ImGuiDemo", the editor UI
// (Dear ImGui's demo window) through editor.screenshot in an Editor process. They run in the golden stage (Release) on
// this machine's device class; Test.py --update-golden writes candidates for review.

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
			// The whole editor UI through editor.screenshot (Roadmap M5; Docs/Decisions/0009-m5-decisions.md decision 33) in a
			// headless editor: a --batch run creates a project, because the method is not available in the launcher state,
			// then asks for the screenshot at the UI's full size. Its PNG outlives the run (AutomationServer::WriteOutputFile).
			// The device class comes from a fixture device, which is created and destroyed before the editor process starts
			// (the probe of Test::ProbeGpuForProcess, which this test needs the device's data from).
			std::string deviceClass;
			{
				Test::HeadlessGpuFixture gpu;
				ENGINE_REQUIRE_GPU(gpu);
				deviceClass = gpu.GetDevice().GetInfo().DeviceClass;
			}
			Test::TempDirectory userData("GoldenImGuiDemo");
			const std::filesystem::path project = userData / "Project";
			const Json create = Json{ { "method", "project.create" }, { "params", Json{ { "path", Test::PathToUtf8(project) }, { "name", "Golden" } } } };
			const Json screenshot = Json{ { "method", "editor.screenshot" }, { "params", Json{ { "maxDimension", MaxViewportScreenshotDimension } } } };
			const std::string requests = create.dump() + '\n' + screenshot.dump() + '\n';
			const std::filesystem::path batch = userData / "ImGuiDemo.jsonl";
			REQUIRE(FileSystem::WriteFileAtomic(batch, std::as_bytes(std::span(requests.data(), requests.size()))).has_value());

			const Result<std::filesystem::path> editor = Test::GetBuiltExecutablePath("Editor");
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			std::vector<std::string> arguments = {
				"--headless",
				"--batch",
				Test::PathToUtf8(batch),
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

			const Result<std::vector<std::filesystem::path>> outputs = FileSystem::ListDirectory(project / "Library" / "Automation" / "Out");
			REQUIRE_MESSAGE(outputs.has_value(), outputs.error().ToString());
			std::vector<std::filesystem::path> screenshots;
			std::ranges::copy_if(*outputs, std::back_inserter(screenshots), [](const std::filesystem::path& file)
			{
				return file.extension() == ".png";
			});
			REQUIRE(screenshots.size() == 1);
			const Result<Image> image = ReadPng(screenshots.front());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			ENGINE_CHECK_GOLDEN("ImGuiDemo", *image, deviceClass);
		}
	}

}
