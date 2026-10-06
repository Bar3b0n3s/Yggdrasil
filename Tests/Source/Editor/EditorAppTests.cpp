#include "TestsPCH.h"

#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/ProjectLock.h"
#include "Support/EditorTestFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

#include <nlohmann/json.hpp>

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

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorApp: --dump-reference writes the method catalogue and the MCP catalogue" * doctest::skip(true))
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

		TEST_CASE("EditorApp: a second editor on a locked project exits with code 3" * doctest::skip(true))
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

		TEST_CASE("EditorApp: --read-only opens a locked project" * doctest::skip(true))
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

		TEST_CASE("EditorApp: --batch runs the file and exits with 1 at the first error" * doctest::skip(true))
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

		TEST_CASE("EditorApp: --upgrade appends its transcript lines and records provenance" * doctest::skip(true))
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

		TEST_CASE("EditorApp: inconsistent editor options are usage errors" * doctest::skip(true))
		{
			Test::TempDirectory userData("EditorUsage");
			const std::vector<std::vector<std::string>> invalid = {
				{ "--headless", "--read-only" },
				{ "--headless", "--renderer", "metal" },
				{ "--headless", "--upgrade" },
			};
			for (const std::vector<std::string>& arguments : invalid)
			{
				const Result<ProcessResult> result = RunEditor(userData, arguments, std::chrono::seconds(60));
				REQUIRE(result.has_value());
				CHECK(result->ExitCode == ExitCode::UsageError);
			}
		}

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
