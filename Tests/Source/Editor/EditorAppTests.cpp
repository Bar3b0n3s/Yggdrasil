#include "TestsPCH.h"

#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Graphics/VulkanDispatch.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/ProjectLock.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

#include <nlohmann/json.hpp>

// The editor executable as a whole process (Roadmap M2, M4): RunApplication with the editor's application, its EditorCore
// modes (--project, --read-only, --batch, --upgrade, --dump-reference, --bake-engine-assets) and its loader checks (M5). Every run gets
// --user-data-dir, so the editor's log file goes to the test's temporary directory, and --engine-cache-dir below it, so no
// run writes the checkout's engine cooked cache (bin/EngineCache). Runs that need no GPU pass --renderer
// none; the ones that render are in the GPU suite: the initialization failures of a rendering editor below, and
// EditorFaultInjectionTests.cpp and EditorScreenshotOptionsTests.cpp.

namespace Engine {

	static Result<ProcessResult> RunEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments,
		std::chrono::milliseconds timeout, std::vector<std::pair<std::string, std::string>> environment = {})
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path editor, Test::GetBuiltExecutablePath("Editor"));
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		arguments.push_back("--engine-cache-dir=" + Test::PathToUtf8(userData / "EngineCache"));
		return Process::Run(
			{ .Executable = std::move(editor), .Arguments = std::move(arguments), .Environment = std::move(environment) }, timeout);
	}

	// Creates the project <directory>/<name> (Empty template) for an editor process to open.
	static std::filesystem::path CreateProcessTestProject(Test::EditorTestFixture& fixture, std::string_view name)
	{
		const Result<CreatedProject> created = ProjectManager::CreateProject(
			{ .Directory = fixture.GetProjectRoot(name), .Name = std::string(name), .Template = ProjectTemplate::Empty, .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources" / "Templates" / "Projects" },
			fixture.GetEngine().GetTypeRegistry());
		REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
		return created->ProjectFile;
	}

	static void WriteProcessTestFile(const std::filesystem::path& path, std::string_view text)
	{
		REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size()))).has_value());
	}

	// RunEditor for a rendering editor started by a GPU test: with Test::GetGpuApplicationArguments (validation and
	// --expect-no-gpu-errors).
	static Result<ProcessResult> RunRenderingEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments)
	{
		const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
		arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
		return RunEditor(userData, std::move(arguments), std::chrono::seconds(60));
	}

	// A rendering editor whose initialization failed after it created its GPU objects (the viewport capture): exit code
	// 3, with everything released before the device is destroyed, so no crash, no GPU object still alive at the device's
	// destruction and no validation message.
	static void CheckCleanInitializationFailure(const ProcessResult& result)
	{
		INFO("editor stderr: ", result.StandardError);
		CHECK(result.ExitCode == ExitCode::InitFailed);
		CHECK_FALSE(result.StandardError.contains("Crash:"));
		CHECK_FALSE(result.StandardError.contains("GPU objects are still alive"));
		CHECK(Test::FindGpuMessageLines(result.StandardError).empty());
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorApp: --dump-reference writes the method catalogue and the MCP catalogue")
		{
			Test::TempDirectory userData("EditorDumpReference");
			const std::filesystem::path out = userData / "Reference";
			const Result<ProcessResult> result =
				RunEditor(userData, { "--headless", "--renderer", "none", "--dump-reference", Test::PathToUtf8(out) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			const Result<std::string> methods = FileSystem::ReadText(out / "Methods.json");
			REQUIRE(methods.has_value());
			CHECK(methods->contains(R"("MethodCatalog")"));
			CHECK(methods->contains(R"("entity.create")"));
			CHECK_FALSE(methods->contains(R"("debug.stall")"));
			const Result<std::string> catalog = FileSystem::ReadText(out / "catalog.json");
			REQUIRE(catalog.has_value());
			CHECK(catalog->contains(R"("entity_create")"));
			// The MCP bridge's committed catalogue is exactly this output (Tools/MCP/catalog.json, §13.8).
			const Result<std::string> committed = FileSystem::ReadText(Test::GetRepositoryRoot() / "Tools/MCP/catalog.json");
			REQUIRE(committed.has_value());
			CHECK(*committed == *catalog);
		}

		TEST_CASE("EditorApp: a second editor on a locked project exits with code 3")
		{
			Test::EditorTestFixture fixture("EditorLocked");
			const std::filesystem::path projectFile = CreateProcessTestProject(fixture, "Locked");
			REQUIRE(FileSystem::CreateDirectories(projectFile.parent_path() / "Library").has_value());
			Result<ProjectLock> lock = ProjectLock::Acquire(projectFile.parent_path() / "Library" / "Editor.lock");
			REQUIRE(lock.has_value());
			const Result<ProcessResult> result = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--frames", "1" }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->ExitCode == ExitCode::InitFailed);
			CHECK(result->StandardError.contains(std::format("locked by process {}", Process::GetCurrentId())));
		}

		TEST_CASE("EditorApp: --read-only opens a locked project")
		{
			Test::EditorTestFixture fixture("EditorReadOnly");
			const std::filesystem::path projectFile = CreateProcessTestProject(fixture, "Shared");
			REQUIRE(FileSystem::CreateDirectories(projectFile.parent_path() / "Library").has_value());
			Result<ProjectLock> lock = ProjectLock::Acquire(projectFile.parent_path() / "Library" / "Editor.lock");
			REQUIRE(lock.has_value());
			const Result<ProcessResult> result = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--read-only", "--frames", "1" },
				std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
		}

		TEST_CASE("EditorApp: --batch runs the file and exits with 1 at the first error")
		{
			Test::EditorTestFixture fixture("EditorBatch");
			const std::filesystem::path projectFile = CreateProcessTestProject(fixture, "Batch");
			// The project path becomes JSON text through Json, which escapes it and keeps it UTF-8 (Support/Utf8Path.h).
			const std::string open = Json{ { "method", "project.open" }, { "params", Json{ { "path", Test::PathToUtf8(projectFile) } } } }.dump();
			const std::string good = open + "\n" + R"({"method":"scene.new","params":{"path":"Assets/Scenes/Main.scene"}})" + "\n" + R"({"method":"entity.create","params":{"name":"Board"}})" + "\n" + R"({"method":"scene.save","params":{}})" + "\n";
			WriteProcessTestFile(fixture.GetDirectory() / "Good.jsonl", good);
			const Result<ProcessResult> passed = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--batch", Test::PathToUtf8(fixture.GetDirectory() / "Good.jsonl") }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(passed.has_value(), passed.error().ToString());
			CHECK_MESSAGE(passed->ExitCode == ExitCode::Success, passed->StandardError);
			CHECK(FileSystem::ReadText(projectFile.parent_path() / "Assets/Scenes/Main.scene").value_or("").contains(R"("Board")"));

			WriteProcessTestFile(fixture.GetDirectory() / "Bad.jsonl", std::string(R"({"method":"scene.tree","params":{}})") + "\n");
			const Result<ProcessResult> failed = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--batch", Test::PathToUtf8(fixture.GetDirectory() / "Bad.jsonl") }, std::chrono::seconds(60));
			REQUIRE(failed.has_value());
			CHECK(failed->ExitCode == ExitCode::Failed);
			CHECK(failed->StandardError.contains("line 1"));
		}

		TEST_CASE("EditorApp: --upgrade appends its transcript lines and records provenance")
		{
			Test::EditorTestFixture fixture("EditorUpgrade");
			const std::filesystem::path projectFile = CreateProcessTestProject(fixture, "Old");
			const Result<std::string> v0 = Test::ReadTestDataText("Formats/Scene/v0.scene");
			REQUIRE(v0.has_value());
			WriteProcessTestFile(projectFile.parent_path() / "Assets/Scenes/Old.scene", *v0);
			const Result<ProcessResult> result = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--upgrade" }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			const Result<std::string> transcript = FileSystem::ReadText(projectFile.parent_path() / "Automation/BuildLog.jsonl");
			REQUIRE(transcript.has_value());
			CHECK(transcript->contains("project.upgrade"));
			CHECK(transcript->contains(R"("cli")"));
			const Result<std::string> provenance = FileSystem::ReadText(projectFile.parent_path() / "Automation/Provenance.json");
			REQUIRE(provenance.has_value());
			CHECK(provenance->contains(R"("Method": "project.upgrade")"));
			CHECK(provenance->contains(R"("TranscriptLine": 1)"));
		}

		TEST_CASE("EditorApp: --dump-reference writes both catalogues in their formats without test hooks")
		{
			Test::TempDirectory userData("EditorDumpFormats");
			const std::filesystem::path out = userData / "Reference";
			const Result<ProcessResult> result =
				RunEditor(userData, { "--headless", "--renderer", "none", "--dump-reference", Test::PathToUtf8(out) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			const Result<std::string> methods = FileSystem::ReadText(out / "Methods.json");
			REQUIRE(methods.has_value());
			CHECK(methods->starts_with("{\n\t\"Format\": \"MethodCatalog\",\n\t\"Version\": 1,\n\t\"ProtocolVersion\": \"1.0\","));
			CHECK_FALSE(methods->contains(R"("debug.stall")"));
			CHECK_FALSE(methods->contains(R"("debug.pend")"));
			const Result<std::string> catalog = FileSystem::ReadText(out / "catalog.json");
			REQUIRE(catalog.has_value());
			CHECK(catalog->starts_with("{\n\t\"Format\": \"McpCatalog\",\n\t\"Version\": 1,\n\t\"ProtocolVersion\": \"1.0\","));
			CHECK(catalog->ends_with("}\n"));
			// A one-shot run never listens: no session file names it.
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(userData / ENGINE_PRODUCT_NAME / "Automation" / "Sessions", error));
		}

		TEST_CASE("EditorApp: --batch exits with 1 at a request that fails and names its line")
		{
			Test::TempDirectory userData("EditorBatchUnknown");
			WriteProcessTestFile(userData / "Unknown.jsonl", std::string(R"({"method":"nosuch.method","params":{}})") + "\n");
			const Result<ProcessResult> result = RunEditor(userData,
				{ "--headless", "--renderer", "none", "--batch", Test::PathToUtf8(userData / "Unknown.jsonl") }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->ExitCode == ExitCode::Failed);
			CHECK(result->StandardError.contains("line 1"));
			CHECK(result->StandardError.contains("nosuch.method"));

			const Result<ProcessResult> missing = RunEditor(userData,
				{ "--headless", "--renderer", "none", "--batch", Test::PathToUtf8(userData / "Missing.jsonl") }, std::chrono::seconds(60));
			REQUIRE(missing.has_value());
			CHECK(missing->ExitCode == ExitCode::InitFailed);
		}

		TEST_CASE("EditorApp: inconsistent editor options are usage errors")
		{
			Test::TempDirectory userData("EditorUsage");
			const std::vector<std::vector<std::string>> invalid = {
				{ "--headless", "--read-only" },
				{ "--headless", "--renderer", "metal" },
				{ "--headless", "--upgrade" },
				{ "--headless", "--renderer", "none", "--bake-engine-assets", "--automation" },
				{ "--headless", "--renderer", "none", "--bake-engine-assets", "--dump-reference", "Reference" },
			};
			for (const std::vector<std::string>& arguments : invalid)
			{
				const Result<ProcessResult> result = RunEditor(userData, arguments, std::chrono::seconds(60));
				REQUIRE(result.has_value());
				CHECK(result->ExitCode == ExitCode::UsageError);
			}
		}

		TEST_CASE("EditorApp: --bake-engine-assets fills the engine cooked cache and exits 0")
		{
			// RunEditor points the engine cooked cache (§7.5) at <userData>/EngineCache, which starts empty: the first run bakes
			// the one File entry this build can import (the Default font) and skips the environments, which have no importer
			// before M8 (a warning naming them, which does not fail the run).
			Test::TempDirectory userData("EditorBakeEngineAssets");
			const std::filesystem::path font = userData / "EngineCache" / BuiltinAssetHandles::DefaultFont.ToString();
			const Result<ProcessResult> first =
				RunEditor(userData, { "--headless", "--renderer", "none", "--bake-engine-assets" }, std::chrono::seconds(180));
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK_MESSAGE(first->ExitCode == ExitCode::Success, first->StandardError);
			CHECK_MESSAGE(first->StandardError.contains("Engine assets: 1 baked, 0 up to date, 2 not baked"), first->StandardError);
			CHECK(first->StandardError.contains("engine://Environments/Studio"));
			const Result<std::vector<std::filesystem::path>> files = FileSystem::ListDirectory(font);
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			// The cooked font and its manifest: <key>.bin and <key>.import.
			REQUIRE(files->size() == 2);
			CHECK(((*files)[0].extension() == ".bin" && (*files)[1].extension() == ".import"));

			// A second run finds the entry up to date and imports nothing.
			const Result<ProcessResult> second =
				RunEditor(userData, { "--headless", "--renderer", "none", "--bake-engine-assets" }, std::chrono::seconds(180));
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			CHECK_MESSAGE(second->ExitCode == ExitCode::Success, second->StandardError);
			CHECK_MESSAGE(second->StandardError.contains("Engine assets: 0 baked, 1 up to date, 2 not baked"), second->StandardError);
			CHECK(FileSystem::ListDirectory(font).value_or(std::vector<std::filesystem::path>()) == *files);
		}

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

		TEST_CASE("EditorApp: a rendering editor whose EditorCore initialization fails exits 3 without GPU errors"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The batch file is loaded last, after the viewport capture, the editor and the server exist; a failure there must
			// release all of them while the device still exists (Application::Run destroys it without calling OnShutdown).
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorRenderingMissingBatch");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--batch", Test::PathToUtf8(userData / "Missing.jsonl") });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CheckCleanInitializationFailure(*result);
			CHECK(result->StandardError.contains("Missing.jsonl"));
		}

		TEST_CASE("EditorApp: a rendering editor on a locked project exits 3 without GPU errors" * doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::EditorTestFixture fixture("EditorRenderingLocked");
			const std::filesystem::path projectFile = CreateProcessTestProject(fixture, "Locked");
			REQUIRE(FileSystem::CreateDirectories(projectFile.parent_path() / "Library").has_value());
			Result<ProjectLock> lock = ProjectLock::Acquire(projectFile.parent_path() / "Library" / "Editor.lock");
			REQUIRE(lock.has_value());
			const Result<ProcessResult> result =
				RunRenderingEditor(fixture.GetDirectory(), { "--headless", "--project", Test::PathToUtf8(projectFile), "--frames", "1" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CheckCleanInitializationFailure(*result);
			CHECK(result->StandardError.contains(std::format("locked by process {}", Process::GetCurrentId())));
		}
	}

}
