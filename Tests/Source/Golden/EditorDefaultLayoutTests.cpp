#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Platform/Process.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/GoldenImage.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: EditorDefaultLayout")
		{
			// Use the actual host and fresh-frame screenshot method with a new user directory, so no saved layout or
			// preferences from the developer's editor can affect the reference.
			std::string deviceClass;
			{
				Test::HeadlessGpuFixture gpu;
				ENGINE_REQUIRE_GPU(gpu);
				deviceClass = gpu.GetDevice().GetInfo().DeviceClass;
			}
			Test::TempDirectory userData("GoldenEditorDefaultLayout");
			const std::filesystem::path project = userData / "Project";
			const Json create = Json{ { "method", "project.create" },
				{ "params", Json{ { "path", Test::PathToUtf8(project) }, { "name", "Golden" }, { "template", "Basic3D" } } } };
			const Json open = Json{ { "method", "scene.open" }, { "params", Json{ { "path", "Assets/Scenes/Main.scene" } } } };
			const Json screenshot = Json{ { "method", "editor.screenshot" }, { "params", Json{ { "maxDimension", MaxViewportScreenshotDimension } } } };
			const std::string requests = create.dump() + '\n' + open.dump() + '\n' + screenshot.dump() + '\n';
			const std::filesystem::path batch = userData / "EditorDefaultLayout.jsonl";
			REQUIRE(FileSystem::WriteFileAtomic(batch, std::as_bytes(std::span(requests.data(), requests.size()))));

			const Result<std::filesystem::path> editor = Test::GetBuiltExecutablePath("Editor");
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			std::vector<std::string> arguments = { "--headless", "--batch", Test::PathToUtf8(batch),
				"--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) };
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> result = Process::Run({ .Executable = *editor, .Arguments = std::move(arguments) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			INFO("editor stdout: ", result->StandardOutput);
			REQUIRE(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());

			const Result<std::vector<std::filesystem::path>> outputs = FileSystem::ListDirectory(project / "Library" / "Automation" / "Out");
			REQUIRE_MESSAGE(outputs.has_value(), outputs.error().ToString());
			std::vector<std::filesystem::path> screenshots;
			std::ranges::copy_if(*outputs, std::back_inserter(screenshots), [](const std::filesystem::path& path)
			{
				return path.extension() == ".png";
			});
			REQUIRE(screenshots.size() == 1);
			const Result<Image> image = ReadPng(screenshots.front());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(image->Width >= 640);
			CHECK(image->Height >= 360);
			ENGINE_CHECK_GOLDEN("EditorDefaultLayout", *image, deviceClass);
		}
	}

}
