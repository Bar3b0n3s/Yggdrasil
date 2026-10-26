#include "TestsPCH.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/VulkanDispatch.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/ProjectLock.h"
#include "Engine/Project/ProjectSerializer.h"
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

	// A windowed run gets --audio-device none (Test::WithoutAudioDevice). `workingDirectory` empty: the test process's. A
	// --run-tests run without --junit writes its reports below its working directory (ADR 0022 decision 7), so such a run
	// passes --junit into its temporary directory or runs there.
	static Result<ProcessResult> RunEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments,
		std::chrono::milliseconds timeout, std::vector<std::pair<std::string, std::string>> environment = {}, std::filesystem::path workingDirectory = {})
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path editor, Test::GetBuiltExecutablePath("Editor"));
		arguments = Test::WithoutAudioDevice(std::move(arguments));
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		arguments.push_back("--engine-cache-dir=" + Test::PathToUtf8(userData / "EngineCache"));
		return Process::Run({ .Executable = std::move(editor),
								.Arguments = std::move(arguments),
								.WorkingDirectory = std::move(workingDirectory),
								.Environment = std::move(environment) },
			timeout);
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

	// The open-panel line ("Panels=...") of the imgui.ini at `layout`. Dear ImGui writes the file in text mode, so its lines
	// end in CRLF on Windows.
	static std::string ReadSavedLayoutPanels(const std::filesystem::path& layout)
	{
		const std::string text = FileSystem::ReadText(layout).value_or("");
		constexpr std::string_view Section = "[EnginePanels][Open]";
		const size_t section = text.find(Section);
		REQUIRE_MESSAGE(section != std::string::npos, text);
		const size_t line = text.find_first_not_of("\r\n", section + Section.size());
		REQUIRE(line != std::string::npos);
		return text.substr(line, text.find_first_of("\r\n", line) - line);
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
		TEST_CASE("EditorApp: --check-scripts exits with the static diagnostic result")
		{
			Test::EditorTestFixture fixture("CheckScriptsCli");
			const auto projectFile = CreateProcessTestProject(fixture, "CheckScripts");
			const auto script = projectFile.parent_path() / "Assets/Scripts/Check.luau";
			WriteProcessTestFile(script, "--!strict\nlocal value: number = 3\nreturn value\n");
			const std::vector<std::string> arguments{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--check-scripts" };
			const auto passed = RunEditor(fixture.GetDirectory(), arguments, std::chrono::seconds(120));
			REQUIRE(passed);
			CHECK_MESSAGE(passed->ExitCode == ExitCode::Success, passed->StandardError);
			WriteProcessTestFile(script, "--!strict\nlocal value: number = \"wrong\"\nreturn value\n");
			const auto failed = RunEditor(fixture.GetDirectory(), arguments, std::chrono::seconds(120));
			REQUIRE(failed);
			CHECK_MESSAGE(failed->ExitCode == ExitCode::Failed, failed->StandardError);
			CHECK(failed->StandardError.contains("Check.luau"));
		}

		// §6 "Strict reader" (the M15 completion review's A024): CI's editors pass --strict, so an unknown key in a committed
		// scene fails the run instead of warning.
		TEST_CASE("EditorApp: --strict refuses a project whose scene holds an unknown key with exit code 3, which only warns without it")
		{
			Test::EditorTestFixture fixture("StrictCli");
			const auto projectFile = CreateProcessTestProject(fixture, "Strict");
			WriteProcessTestFile(projectFile.parent_path() / "Assets/Scenes/Main.scene", R"({
	"Format": "Scene",
	"Version": 1,
	"Name": "Main",
	"Seed": 7,
	"ComponentVersions": {
		"Transform": 1
	},
	"Entities": [
		{
			"ID": "1a00000000000001",
			"Name": "Checkout",
			"Parent": null,
			"Active": true,
			"Colour": "red",
			"Tags": [],
			"Components": {
				"Transform": {
					"Translation": [0, 0, 0],
					"Rotation": [0, 0, 0, 1],
					"Scale": [1, 1, 1]
				}
			}
		}
	]
}
)");
			std::vector<std::string> arguments{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--check-scripts" };
			const auto lenient = RunEditor(fixture.GetDirectory(), arguments, std::chrono::seconds(120));
			REQUIRE(lenient);
			CHECK_MESSAGE(lenient->ExitCode == ExitCode::Success, lenient->StandardError);
			arguments.push_back("--strict");
			const auto strict = RunEditor(fixture.GetDirectory(), arguments, std::chrono::seconds(120));
			REQUIRE(strict);
			INFO("strict stderr: ", strict->StandardError);
			CHECK(strict->ExitCode == ExitCode::InitFailed);
			CHECK(strict->StandardError.contains("Assets/Scenes/Main.scene"));
			CHECK(strict->StandardError.contains("Colour"));
		}

		TEST_CASE("EditorApp: --run-tests honours the case filter and exits with the suite result")
		{
			Test::EditorTestFixture fixture("RunTestsCli");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const auto projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			ProjectSettings settings = fixture.GetEditor().GetProject().GetSettings();
			TestSuiteSettings suite;
			suite.Script = "Assets/Tests/Cli.test.luau";
			suite.Scene = "Assets/Scenes/Main.scene";
			settings.Testing.Suites.push_back(std::move(suite));
			const auto project = ProjectSerializer::SaveToString(settings, fixture.GetEngine().GetTypeRegistry());
			REQUIRE(project);
			REQUIRE(fixture.GetEditor().CloseProject());
			WriteProcessTestFile(projectFile, *project);
			WriteProcessTestFile(projectFile.parent_path() / "Assets/Tests/Cli.test.luau", R"(
return Test.Suite("Cli", function()
	Test.Case("passes", function() Test.WaitTicks(2) Test.Expect(true) end)
	Test.Case("fails", function() Test.Expect(false, "CLI failure") end)
end)
)");
			// The reports go to the test's directory (ADR 0022 decision 7), never the repository's bin/TestResults.
			std::vector<std::string> arguments{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--run-tests", "--junit",
				Test::PathToUtf8(fixture.GetDirectory() / "Reports" / "Cli.xml") };
			const auto failed = RunEditor(fixture.GetDirectory(), arguments, std::chrono::seconds(120));
			REQUIRE(failed);
			CHECK_MESSAGE(failed->ExitCode == ExitCode::Failed, failed->StandardError);
			CHECK(failed->StandardError.contains("CLI failure"));
			arguments.insert(arguments.end(), { "--filter", "Cli/passes" });
			const auto passed = RunEditor(fixture.GetDirectory(), arguments, std::chrono::seconds(120));
			REQUIRE(passed);
			CHECK_MESSAGE(passed->ExitCode == ExitCode::Success, passed->StandardError);
		}

		// Docs/Decisions/0026-m16-contract.md decision 13 (G9): a fresh clone, and the games suite's index copies, have no
		// Library/, so a case's Assets.Load imports the asset synchronously inside the case. That import is host work that
		// runs no Luau: its time must not count toward the callback budget (it did, and FeatureTest's Formats case failed
		// with "script exceeded 500 ms" from a cold cache while the Inter font imported for 2 s in Debug). The case loads
		// two fonts and five textures nothing imported before, under a callback budget far below their import time.
		TEST_CASE("EditorApp: --run-tests passes a case that loads uncached assets from a cold import cache")
		{
			Test::EditorTestFixture fixture("RunTestsColdCache");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			const std::filesystem::path root = projectFile.parent_path();
			ProjectSettings settings = fixture.GetEditor().GetProject().GetSettings();
			settings.Scripting.CallbackBudgetMs = 100;
			TestSuiteSettings suite;
			suite.Script = "Assets/Tests/Cold.test.luau";
			suite.Scene = "Assets/Scenes/Main.scene";
			settings.Testing.Suites.push_back(std::move(suite));
			const auto project = ProjectSerializer::SaveToString(settings, fixture.GetEngine().GetTypeRegistry());
			REQUIRE(project);
			REQUIRE(fixture.GetEditor().CloseProject());
			WriteProcessTestFile(projectFile, *project);
			std::string paths;
			for (const std::string_view asset : { "Fonts/Inter-Regular.otf", "Fonts/Glyphs.ttf", "Textures/Alpha.tga", "Textures/Baseline.jpeg",
					 "Textures/Photo.jpg", "Textures/Rgba.png", "Textures/Solid.bmp" })
			{
				const Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(std::format("Assets/{}", asset)));
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				const std::filesystem::path target = root / "Assets" / "Cold" / FileSystem::PathFromUtf8(asset);
				REQUIRE(FileSystem::CreateDirectories(target.parent_path()).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(target, *bytes).has_value());
				paths += std::format("\t\"Assets/Cold/{}\",\n", asset);
			}
			WriteProcessTestFile(root / "Assets/Tests/Cold.test.luau", std::format(R"(--!strict
local PATHS = {{
{}}}
return Test.Suite("Cold", function()
	Test.Case("loads assets nothing imported before", function()
		for _, path in PATHS do
			local asset = Assets.Load(path)
			Test.Expect(asset ~= nil, `{{path}} loads`)
		end
	end)
end)
)",
																		   paths));
			// A fresh copy without Library/: every asset above is imported for the first time inside the case.
			std::error_code error;
			std::filesystem::remove_all(root / "Library", error);
			REQUIRE_FALSE(error);
			REQUIRE_FALSE(std::filesystem::exists(root / "Library", error));
			const auto run = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--run-tests", "--junit",
					Test::PathToUtf8(fixture.GetDirectory() / "Reports" / "Cold.xml") },
				std::chrono::seconds(300));
			REQUIRE(run);
			CHECK_FALSE(run->StandardError.contains("possible infinite loop"));
			CHECK_MESSAGE(run->ExitCode == ExitCode::Success, run->StandardError);
		}

		TEST_CASE("EditorApp: --run-tests runs in a windowed editor")
		{
			// ADR 0019 decision 6: the normal windowed editor constructs its AudioEngine with deterministic decoding, so
			// test.run (behind --run-tests), test.list and an attached agent's runs work there, not only headless.
			Test::EditorTestFixture fixture("RunTestsWindowed");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const auto projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			ProjectSettings settings = fixture.GetEditor().GetProject().GetSettings();
			TestSuiteSettings suite;
			suite.Script = "Assets/Tests/Windowed.test.luau";
			settings.Testing.Suites.push_back(std::move(suite));
			const auto project = ProjectSerializer::SaveToString(settings, fixture.GetEngine().GetTypeRegistry());
			REQUIRE(project);
			REQUIRE(fixture.GetEditor().CloseProject());
			WriteProcessTestFile(projectFile, *project);
			WriteProcessTestFile(projectFile.parent_path() / "Assets/Tests/Windowed.test.luau", R"(
return Test.Suite("Windowed", function()
	Test.Case("runs", function() Test.WaitTicks(2) Test.Expect(true) end)
end)
)");
			const auto result = RunEditor(fixture.GetDirectory(),
				{ "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--run-tests", "--junit", Test::PathToUtf8(fixture.GetDirectory() / "Reports" / "Windowed.xml") },
				std::chrono::seconds(120));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->StandardError.contains("Process context ready: Windowed"));
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK_FALSE(result->StandardError.contains("deterministic audio decoding"));
		}

		TEST_CASE("EditorApp: first save of a recovered untitled scene removes only its generation")
		{
			Test::EditorTestFixture fixture("HostUntitledRecovery");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const auto projectFile = editor.GetProject().GetProjectFile();
			const auto recoveryRoot = fixture.GetProjectRoot() / "Library/Autosave";
			Autosave saves(editor);
			// Every recovered scene opens again with the oldest active (Docs/Decisions/0031-m20-contract.md decision 15), so
			// the scene the batch saves is the one autosaved first.
			editor.SetScene(editor.CreateScene("RecoverUntitled"), std::nullopt, true);
			const auto recovered = saves.Save(AutosaveReason::BeforePlay);
			REQUIRE(recovered.has_value());
			REQUIRE(recovered->Written);
			editor.SetScene(editor.CreateScene("PreserveUntitled"), std::nullopt, true);
			const auto second = saves.Save(AutosaveReason::BeforePlay);
			REQUIRE(second.has_value());
			REQUIRE(second->Written);
			const auto preservedPath = recoveryRoot / second->Generation / "Scene.json";
			const auto preserved = FileSystem::ReadText(preservedPath);
			REQUIRE(preserved.has_value());
			REQUIRE(saves.Reset());
			REQUIRE(editor.CloseProject().has_value());

			const Json open = { { "method", "project.open" }, { "params", { { "path", Test::PathToUtf8(projectFile) }, { "recover", true } } } };
			const Json save = { { "method", "scene.save" }, { "params", { { "path", "Assets/Scenes/FirstSave.scene" } } } };
			const auto batchPath = fixture.GetDirectory() / "RecoverSave.jsonl";
			WriteProcessTestFile(batchPath, open.dump() + "\n" + save.dump() + "\n");
			const auto result = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--batch", Test::PathToUtf8(batchPath) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO(result->StandardError);
			REQUIRE(result->ExitCode == ExitCode::Success);
			CHECK_FALSE(FileSystem::Exists(recoveryRoot / recovered->Generation));
			CHECK(FileSystem::ReadText(preservedPath).value_or("") == *preserved);
			const auto saved = FileSystem::ReadText(fixture.GetProjectRoot() / "Assets/Scenes/FirstSave.scene");
			REQUIRE(saved.has_value());
			CHECK(saved->contains("RecoverUntitled"));
			CHECK_FALSE(saved->contains("PreserveUntitled"));
			CHECK(ProjectLock::Acquire(fixture.GetProjectRoot() / "Library/Editor.lock").has_value());
		}

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
			for (const std::string_view file : { "Engine.d.luau", "ScriptAPI.md" })
			{
				const auto generated = FileSystem::ReadText(out / file);
				REQUIRE(generated);
				CHECK(generated->contains("Script"));
				CHECK(generated->contains("OnFixedUpdate"));
			}
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

		// ADR 0022 decision 8 (implemented by the contract): a committed scaffold's batch run with --id-seed writes the same entity
		// ids and asset handles every time and on every machine, and another seed gives other ids. Like every scaffold it ends
		// with project.refreshAssets, so the new scene's .meta (its handle) is written inside the seeded run.
		TEST_CASE("EditorApp: --id-seed makes a batch run's entity ids and asset handles reproducible")
		{
			Test::EditorTestFixture fixture("EditorIdSeed");
			const auto run = [&fixture](std::string_view name, std::string_view seed)
			{
				const std::filesystem::path projectFile = CreateProcessTestProject(fixture, name);
				const std::string open = Json{ { "method", "project.open" }, { "params", Json{ { "path", Test::PathToUtf8(projectFile) } } } }.dump();
				const std::string batch = open + "\n" + R"({"method":"scene.new","params":{"path":"Assets/Scenes/Main.scene"}})" + "\n" + R"({"method":"entity.create","params":{"name":"Board"}})" + "\n" + R"({"method":"scene.save","params":{}})" + "\n" + R"({"method":"project.refreshAssets","params":{}})" + "\n";
				const std::filesystem::path batchPath = fixture.GetDirectory() / (std::string(name) + ".jsonl");
				WriteProcessTestFile(batchPath, batch);
				const Result<ProcessResult> result = RunEditor(fixture.GetDirectory(),
					{ "--headless", "--renderer", "none", "--batch", Test::PathToUtf8(batchPath), "--id-seed", std::string(seed) }, std::chrono::seconds(60));
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				REQUIRE_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
				const Result<std::string> scene = FileSystem::ReadText(projectFile.parent_path() / "Assets/Scenes/Main.scene");
				const Result<std::string> meta = FileSystem::ReadText(projectFile.parent_path() / "Assets/Scenes/Main.scene.meta");
				REQUIRE((scene.has_value() && meta.has_value()));
				return std::pair<std::string, std::string>{ *scene, *meta };
			};
			const auto first = run("SeedA", "1401");
			const auto second = run("SeedB", "1401");
			CHECK(first.first.contains(R"("Board")"));
			CHECK(first.first == second.first);
			CHECK(first.second == second.second);
			const auto other = run("SeedC", "1402");
			CHECK(other.first != first.first);
			CHECK(other.second != first.second);
		}

		// ADR 0022 decision 7 (stream A): --run-tests writes the JUnit report to --junit and the JSON report beside it, or both
		// to <working directory>/bin/TestResults/FeatureTest-Editor.{xml,json}, from the run's result whatever its outcome,
		// independent of the automation server's offload directory (which the server removes at exit).
		TEST_CASE("EditorApp: --run-tests writes its JUnit and JSON reports to --junit or below the working directory")
		{
			Test::EditorTestFixture fixture("RunTestsReports");
			const std::filesystem::path projectFile = CreateProcessTestProject(fixture, "Reports");
			WriteProcessTestFile(projectFile.parent_path() / "Assets/Tests/Reports.test.luau",
				"return Test.Suite(\"Reports\", function() Test.Case(\"fails\", function() Test.Expect(false, \"report failure\") end) end)\n");
			const Json suites = Json::array({ Json{ { "Script", "Assets/Tests/Reports.test.luau" } } });
			const std::string patch = Json{ { "method", "project.setSettings" }, { "params", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", suites } } } } } } } }.dump();
			const std::string open = Json{ { "method", "project.open" }, { "params", Json{ { "path", Test::PathToUtf8(projectFile) } } } }.dump();
			WriteProcessTestFile(fixture.GetDirectory() / "Suites.jsonl", open + "\n" + patch + "\n");
			const Result<ProcessResult> configured =
				RunEditor(fixture.GetDirectory(), { "--headless", "--renderer", "none", "--batch", Test::PathToUtf8(fixture.GetDirectory() / "Suites.jsonl") }, std::chrono::seconds(60));
			REQUIRE(configured.has_value());
			REQUIRE_MESSAGE(configured->ExitCode == ExitCode::Success, configured->StandardError);

			const std::filesystem::path junit = fixture.GetDirectory() / "Reports" / "Run.xml";
			const Result<ProcessResult> explicitPath = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--run-tests", "--junit", Test::PathToUtf8(junit) }, std::chrono::seconds(120));
			REQUIRE(explicitPath.has_value());
			CHECK_MESSAGE(explicitPath->ExitCode == ExitCode::Failed, explicitPath->StandardError);
			CHECK(FileSystem::ReadText(junit).value_or("").contains("report failure"));
			const Result<std::string> json = FileSystem::ReadText(fixture.GetDirectory() / "Reports" / "Run.json");
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			CHECK(json->contains("report failure"));

			const std::filesystem::path workingDirectory = fixture.GetDirectory() / "Cwd";
			REQUIRE(FileSystem::CreateDirectories(workingDirectory));
			const Result<ProcessResult> defaultPath = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--run-tests" }, std::chrono::seconds(120), {}, workingDirectory);
			REQUIRE(defaultPath.has_value());
			CHECK(defaultPath->ExitCode == ExitCode::Failed);
			CHECK(FileSystem::Exists(workingDirectory / "bin/TestResults/FeatureTest-Editor.xml"));
			CHECK(FileSystem::Exists(workingDirectory / "bin/TestResults/FeatureTest-Editor.json"));
		}

		// Review finding M01 (ADR 0022 decision 7, EditorCommandLine.h "Written whatever the outcome"): a --run-tests run whose
		// test.run fails before any result exists (type errors block discovery, §11.9) still writes both reports, as a failed
		// run naming the error, over the passing reports an earlier run left at the same paths.
		TEST_CASE("EditorApp: --run-tests writes failed reports over stale ones when the run fails before a result exists")
		{
			Test::EditorTestFixture fixture("RunTestsPreflightReports");
			const std::filesystem::path projectFile = CreateProcessTestProject(fixture, "Preflight");
			WriteProcessTestFile(projectFile.parent_path() / "Assets/Tests/Typed.test.luau",
				"--!strict\nlocal wrong: number = \"text\"\nreturn Test.Suite(\"Typed\", function() Test.Case(\"passes\", function() Test.Expect(wrong == wrong) end) end)\n");
			const Json suites = Json::array({ Json{ { "Script", "Assets/Tests/Typed.test.luau" } } });
			const std::string patch = Json{ { "method", "project.setSettings" }, { "params", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", suites } } } } } } } }.dump();
			const std::string open = Json{ { "method", "project.open" }, { "params", Json{ { "path", Test::PathToUtf8(projectFile) } } } }.dump();
			WriteProcessTestFile(fixture.GetDirectory() / "Suites.jsonl", open + "\n" + patch + "\n");
			const Result<ProcessResult> configured =
				RunEditor(fixture.GetDirectory(), { "--headless", "--renderer", "none", "--batch", Test::PathToUtf8(fixture.GetDirectory() / "Suites.jsonl") }, std::chrono::seconds(60));
			REQUIRE(configured.has_value());
			REQUIRE_MESSAGE(configured->ExitCode == ExitCode::Success, configured->StandardError);

			const std::filesystem::path junit = fixture.GetDirectory() / "Reports" / "Preflight.xml";
			const std::filesystem::path json = fixture.GetDirectory() / "Reports" / "Preflight.json";
			REQUIRE(FileSystem::CreateDirectories(junit.parent_path()));
			WriteProcessTestFile(junit, "<testsuites stale=\"true\"/>\n");
			WriteProcessTestFile(json, R"({"mode": "Editor", "passed": true, "stale": true})");
			const Result<ProcessResult> run = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--run-tests", "--junit", Test::PathToUtf8(junit) }, std::chrono::seconds(120));
			REQUIRE(run.has_value());
			CHECK_MESSAGE(run->ExitCode == ExitCode::Failed, run->StandardError);
			const Result<std::string> xml = FileSystem::ReadText(junit);
			REQUIRE_MESSAGE(xml.has_value(), xml.error().ToString());
			CHECK_FALSE(xml->contains("stale"));
			CHECK(xml->contains("<error message="));
			CHECK(xml->contains("script type errors block test discovery"));
			const Result<std::string> report = FileSystem::ReadText(json);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK_FALSE(report->contains("stale"));
			CHECK(report->contains(R"("mode": "Editor")"));
			CHECK(report->contains(R"("passed": false)"));
			CHECK(report->contains("script type errors block test discovery"));
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
			CHECK(transcript->contains("project.refreshAssets"));
			CHECK(transcript->contains("project.upgrade"));
			CHECK(transcript->contains(R"("cli")"));
			// project.upgrade is the second request of the cli batch (ADR 0026 decision 11): line 3, after project.refreshAssets
			// and its response, and it carries its own request id (G6).
			const Result<std::string> provenance = FileSystem::ReadText(projectFile.parent_path() / "Automation/Provenance.json");
			REQUIRE(provenance.has_value());
			const Result<std::vector<ProvenanceEntry>> entries = ProvenanceRecorder::FromText(*provenance);
			REQUIRE_MESSAGE(entries.has_value(), entries.error().ToString());
			const auto scene = std::ranges::find(*entries, std::string_view("Assets/Scenes/Old.scene"), &ProvenanceEntry::Path);
			REQUIRE(scene != entries->end());
			CHECK(scene->Method == "project.upgrade");
			CHECK(scene->Client == "cli");
			CHECK(scene->RequestId == VariantValue(Json(2)));
			CHECK(scene->TranscriptLine == std::optional<uint64_t>(3));
		}

		TEST_CASE("EditorApp: --upgrade defers the asset scan to a transcribed project.refreshAssets before project.upgrade")
		{
			Test::EditorTestFixture fixture("EditorUpgradeDeferredScan");
			const std::filesystem::path projectFile = CreateProcessTestProject(fixture, "Deferred");
			const std::filesystem::path root = projectFile.parent_path();
			// A source without a .meta, as an engine change or a hand copy leaves one (the open-time scan writes its .meta),
			// and a scene in an old format (project.upgrade rewrites it).
			const Result<Buffer> png = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/Textures/Checker.png"));
			REQUIRE(png.has_value());
			REQUIRE(FileSystem::CreateDirectories(root / "Assets/Textures").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(root / "Assets/Textures/Checker.png", *png).has_value());
			const Result<std::string> v0 = Test::ReadTestDataText("Formats/Scene/v0.scene");
			REQUIRE(v0.has_value());
			WriteProcessTestFile(root / "Assets/Scenes/Old.scene", *v0);

			const Result<ProcessResult> result = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--upgrade" }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);

			// The cli batch [project.refreshAssets, project.upgrade]: lines 1 and 3 are its requests, 2 and 4 their responses.
			const auto readTranscript = [&root]()
			{
				const Result<std::string> transcript = FileSystem::ReadText(root / "Automation/BuildLog.jsonl");
				REQUIRE(transcript.has_value());
				std::vector<Json> read;
				for (size_t begin = 0; begin < transcript->size();)
				{
					const size_t end = transcript->find('\n', begin);
					REQUIRE(end != std::string::npos);
					Result<Json> line = JsonReader::Parse(std::string_view(*transcript).substr(begin, end - begin));
					REQUIRE_MESSAGE(line.has_value(), line.error().ToString());
					read.push_back(std::move(*line));
					begin = end + 1;
				}
				return read;
			};
			std::vector<Json> lines = readTranscript();
			REQUIRE(lines.size() == 4);
			const std::pair<const char*, int> requests[] = { { "project.refreshAssets", 1 }, { "project.upgrade", 3 } };
			for (const auto& [method, line] : requests)
			{
				CAPTURE(std::string(method));
				Json& request = lines[static_cast<size_t>(line - 1)];
				Json& response = lines[static_cast<size_t>(line)];
				CHECK(request["type"] == Json("request"));
				CHECK(request["method"] == Json(method));
				CHECK(request["client"] == Json("cli"));
				CHECK(response["type"] == Json("response"));
				CHECK(response["requestLine"] == Json(line));
				CHECK(response["ok"] == Json(true));
			}

			// The scan's .meta writes belong to project.refreshAssets at line 1, the scene's rewrite to project.upgrade at line
			// 3, and no entry is "ui" (§13.12 rule 2).
			const Result<std::string> provenance = FileSystem::ReadText(root / "Automation/Provenance.json");
			REQUIRE(provenance.has_value());
			const Result<std::vector<ProvenanceEntry>> entries = ProvenanceRecorder::FromText(*provenance);
			REQUIRE_MESSAGE(entries.has_value(), entries.error().ToString());
			for (const ProvenanceEntry& entry : *entries)
			{
				CAPTURE(entry.Path);
				CHECK(entry.Method != "ui");
				CHECK(entry.Client != "ui");
			}
			const auto find = [&entries](std::string_view path) -> const ProvenanceEntry*
			{
				const auto found = std::ranges::find(*entries, path, &ProvenanceEntry::Path);
				return found == entries->end() ? nullptr : &*found;
			};
			const ProvenanceEntry* meta = find("Assets/Textures/Checker.png.meta");
			REQUIRE(meta != nullptr);
			CHECK(meta->Method == "project.refreshAssets");
			CHECK(meta->Client == "cli");
			CHECK(meta->TranscriptLine == std::optional<uint64_t>(1));
			const ProvenanceEntry* scene = find("Assets/Scenes/Old.scene");
			REQUIRE(scene != nullptr);
			CHECK(scene->Method == "project.upgrade");
			CHECK(scene->TranscriptLine == std::optional<uint64_t>(3));

			// A failing project.refreshAssets (here a .luaurc the type checker cannot read) leaves the project open until the
			// batch ends, so its response line is transcribed and the run fails with the request's own error, not with a
			// transcript error.
			const Buffer invalidLuaurc = { std::byte{ 0xff } };
			REQUIRE(FileSystem::WriteFileAtomic(root / ".luaurc", invalidLuaurc, { .KeepBackup = false }).has_value());
			const Result<ProcessResult> failed = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--upgrade" }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(failed.has_value(), failed.error().ToString());
			CHECK_MESSAGE(failed->ExitCode == ExitCode::Failed, failed->StandardError);
			CHECK(failed->StandardError.contains(".luaurc"));
			CHECK_FALSE(failed->StandardError.contains("writing the transcript"));
			lines = readTranscript();
			REQUIRE(lines.size() == 6);
			CHECK(lines[4]["type"] == Json("request"));
			CHECK(lines[4]["method"] == Json("project.refreshAssets"));
			CHECK(lines[5]["type"] == Json("response"));
			CHECK(lines[5]["requestLine"] == Json(5));
			CHECK(lines[5]["ok"] == Json(false));
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

		// M16 friction F20: `Editor --help` was an unknown option (exit 2) that printed the option list only inside the error.
		TEST_CASE("EditorApp: --help and -h print the usage to standard output and exit 0, and an unknown option stays a usage error")
		{
			Test::TempDirectory userData("EditorHelp");
			for (const std::string_view help : { "--help", "-h" })
			{
				CAPTURE(help);
				const Result<ProcessResult> result = RunEditor(userData, { std::string(help) }, std::chrono::seconds(60));
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				INFO("editor stderr: ", result->StandardError);
				CHECK(result->ExitCode == ExitCode::Success);
				CHECK(result->StandardOutput.starts_with("Usage: Editor [options]"));
				for (const std::string_view option : { "--help", "--project", "--headless", "--renderer", "--export", "--viewport-screenshot", "--user-data-dir" })
					CHECK_MESSAGE(result->StandardOutput.contains(option), option);
				// Nothing started: no log line, and no user-data directory of the product.
				CHECK(result->StandardError.empty());
				std::error_code error;
				CHECK(std::filesystem::is_empty(userData.GetPath(), error));
				CHECK_FALSE(error);
			}
			const Result<ProcessResult> unknown = RunEditor(userData, { "--help", "--bogus" }, std::chrono::seconds(60));
			REQUIRE(unknown.has_value());
			CHECK(unknown->ExitCode == ExitCode::UsageError);
			CHECK(unknown->StandardOutput.empty());
			CHECK(unknown->StandardError.contains("--bogus"));
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

		// M15 (§12.1, §14.2; Docs/Decisions/0024-m15-contract.md decision 3): the one-shot export. Editor --export exports from
		// the checkout's bin directory, so it packages the Runtime of this test binary's configuration, which Build.py built
		// with its CRT and notices beside it.
		TEST_CASE("EditorApp: --export writes the package and its report, and a refused output directory exits 1 with the report")
		{
			Test::EditorTestFixture fixture("EditorExport");
			fixture.CreateAndOpenProject("ExportGame");
			fixture.CreateAndOpenScene();
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			ProjectSettings settings = fixture.GetEditor().GetProject().GetSettings();
			settings.StartScene = "Assets/Scenes/Main.scene";
			settings.Export.BuildScenes = { "Assets/Scenes/Main.scene" };
			const Result<std::string> project = ProjectSerializer::SaveToString(settings, fixture.GetEngine().GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(fixture.GetEditor().CloseProject().has_value());
			WriteProcessTestFile(projectFile, *project);

			const std::string outputName = Test::PathToUtf8(Test::GetTestOptions().ExecutablePath.parent_path().parent_path().filename());
			const std::string config = outputName.substr(0, outputName.find('-'));
			const std::filesystem::path output = fixture.GetDirectory() / "Packages" / "ExportGame";
			const std::filesystem::path reportPath = fixture.GetDirectory() / "Reports" / "Export.json";
			const Result<ProcessResult> exported = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--export", Test::PathToUtf8(output), "--config", config,
					"--export-report", Test::PathToUtf8(reportPath) },
				std::chrono::seconds(300));
			REQUIRE_MESSAGE(exported.has_value(), exported.error().ToString());
			INFO("editor stderr: ", exported->StandardError);
			CHECK(exported->ExitCode == ExitCode::Success);
			const Result<std::string> reportText = FileSystem::ReadText(reportPath);
			REQUIRE_MESSAGE(reportText.has_value(), reportText.error().ToString());
			const Result<Json> report = JsonReader::Parse(*reportText);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			const JsonReader reportReader(*report);
			CHECK(reportReader.ReadMember<bool>("success").value_or(false));
			const Result<JsonReader> result = reportReader.GetMember("result");
			REQUIRE_MESSAGE(result.has_value(), *reportText);
			CHECK(FileSystem::PathFromUtf8(result->ReadMember<std::string>("outputDirectory").value_or("")).lexically_normal() == output.lexically_normal());
			CHECK(result->ReadMember<std::string>("config").value_or("") == config);
			// The report's result is project.export's whole result (MakeProjectExportResult), the M15 members included.
			CHECK_FALSE(result->ReadMember<bool>("testing").value_or(true));
			CHECK(result->ReadMember<uint32_t>("testSuiteCount").value_or(1) == 0);
			CHECK(result->ReadMember<uint32_t>("replayCount").value_or(1) == 0);
			CHECK(result->HasMember("resourcesPatched"));
			const Result<JsonReader> timings = result->GetMember("phaseTimings");
			REQUIRE_MESSAGE(timings.has_value(), *reportText);
			REQUIRE(timings->GetArraySize().value_or(0) > 0);
			const Result<JsonReader> firstTiming = timings->GetElement(0);
			REQUIRE(firstTiming.has_value());
			CHECK(firstTiming->ReadMember<std::string>("phase").value_or("") == "Validate");
			const Result<std::string> executable = result->ReadMember<std::string>("executable");
			REQUIRE(executable.has_value());
			CHECK(FileSystem::Exists(FileSystem::PathFromUtf8(*executable)));
			// The project's Build/ directory is untouched: the package went to --export's directory.
			CHECK_FALSE(FileSystem::Exists(projectFile.parent_path() / "Build"));

			// The project itself (and a directory above it) is never an output directory: exit 1, and the report says why.
			const Result<ProcessResult> refused = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--export", Test::PathToUtf8(projectFile.parent_path()),
					"--config", config, "--export-report", Test::PathToUtf8(reportPath) },
				std::chrono::seconds(300));
			REQUIRE_MESSAGE(refused.has_value(), refused.error().ToString());
			INFO("refused stderr: ", refused->StandardError);
			CHECK(refused->ExitCode == ExitCode::Failed);
			const Result<Json> failed = JsonReader::Parse(FileSystem::ReadText(reportPath).value_or(""));
			REQUIRE(failed.has_value());
			const JsonReader failedReader(*failed);
			CHECK_FALSE(failedReader.ReadMember<bool>("success").value_or(true));
			const Result<JsonReader> error = failedReader.GetMember("error");
			REQUIRE(error.has_value());
			CHECK(error->ReadMember<std::string>("errorCode").value_or("") == "InvalidArgument");
			CHECK(error->ReadMember<std::string>("message").value_or("").contains("the project"));
			CHECK(FileSystem::Exists(projectFile));

			// --export without --config is a usage error.
			const Result<ProcessResult> usage = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--export", Test::PathToUtf8(output) }, std::chrono::seconds(60));
			REQUIRE(usage.has_value());
			CHECK(usage->ExitCode == ExitCode::UsageError);

			// So is --frames, which would end the run with exit code 0 before the export finished, without a package or a
			// report (the M15 review's M03).
			const std::filesystem::path framesOutput = fixture.GetDirectory() / "Packages" / "Frames";
			const std::filesystem::path framesReport = fixture.GetDirectory() / "Reports" / "Frames.json";
			const Result<ProcessResult> frames = RunEditor(fixture.GetDirectory(),
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(projectFile), "--export", Test::PathToUtf8(framesOutput), "--config", config,
					"--export-report", Test::PathToUtf8(framesReport), "--frames", "5" },
				std::chrono::seconds(60));
			REQUIRE(frames.has_value());
			INFO("frames stderr: ", frames->StandardError);
			CHECK(frames->ExitCode == ExitCode::UsageError);
			CHECK(frames->StandardError.contains("--frames"));
			CHECK_FALSE(FileSystem::Exists(framesOutput));
		}

		TEST_CASE("EditorApp: --export of a project that does not open exits 3 and still writes the report")
		{
			Test::TempDirectory userData("EditorExportMissing");
			const std::filesystem::path reportPath = userData / "Export.json";
			const Result<ProcessResult> result = RunEditor(userData,
				{ "--headless", "--renderer", "none", "--project", Test::PathToUtf8(userData / "Missing" / "Missing.eproj"), "--export",
					Test::PathToUtf8(userData / "Package"), "--config", "Release", "--export-report", Test::PathToUtf8(reportPath) },
				std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::InitFailed);
			const Result<Json> report = JsonReader::Parse(FileSystem::ReadText(reportPath).value_or(""));
			REQUIRE(report.has_value());
			const JsonReader reader(*report);
			CHECK_FALSE(reader.ReadMember<bool>("success").value_or(true));
			const Result<JsonReader> error = reader.GetMember("error");
			REQUIRE(error.has_value());
			CHECK(error->HasMember("issues"));
			CHECK(error->HasMember("code"));
			CHECK_FALSE(FileSystem::Exists(userData / "Package"));
		}

		TEST_CASE("EditorApp: --bake-engine-assets fills the engine cooked cache and exits 0")
		{
			// RunEditor points the engine cooked cache (§7.5) at <userData>/EngineCache, which starts empty: the first run bakes
			// the File entries this build can import without a GPU (the Default font and the ten sound effect presets of M12) and
			// the Generated blue noise (M8), and skips the environments, which need a GPU (a warning naming them, which does not
			// fail the run).
			Test::TempDirectory userData("EditorBakeEngineAssets");
			const std::filesystem::path font = userData / "EngineCache" / BuiltinAssetHandles::DefaultFont.ToString();
			const Result<ProcessResult> first =
				RunEditor(userData, { "--headless", "--renderer", "none", "--bake-engine-assets" }, std::chrono::seconds(180));
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK_MESSAGE(first->ExitCode == ExitCode::Success, first->StandardError);
			CHECK_MESSAGE(first->StandardError.contains("Engine assets: 12 baked, 0 up to date, 2 not baked"), first->StandardError);
			CHECK(first->StandardError.contains("engine://Environments/Studio"));
			const Result<std::vector<std::filesystem::path>> files = FileSystem::ListDirectory(font);
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			// The cooked font and its manifest: <key>.bin and <key>.import.
			REQUIRE(files->size() == 2);
			CHECK(((*files)[0].extension() == ".bin" && (*files)[1].extension() == ".import"));

			// A second run finds all twelve entries up to date and bakes nothing.
			const Result<ProcessResult> second =
				RunEditor(userData, { "--headless", "--renderer", "none", "--bake-engine-assets" }, std::chrono::seconds(180));
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			CHECK_MESSAGE(second->ExitCode == ExitCode::Success, second->StandardError);
			CHECK_MESSAGE(second->StandardError.contains("Engine assets: 0 baked, 12 up to date, 2 not baked"), second->StandardError);
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
			// A native window on the system clock; Linux CI provides a display through Xvfb. RunEditor adds --audio-device none:
			// no test opens the machine's audio device (§15.1 T1), which a windowed editor would by default (M12).
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
			// The audio engine runs without a device by configuration: it neither opened one nor tried to (AudioEngine::Create
			// logs all three at startup). Every editor decodes deterministically from startup, the windowed one included
			// (ADR 0019 decision 6), so its test runs need no other resource manager.
			CHECK(result->StandardError.contains("Audio engine: no device, Deterministic decoding"));
			CHECK_FALSE(result->StandardError.contains("Audio engine: device"));
			CHECK_FALSE(result->StandardError.contains("No audio device could be created"));
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

		TEST_CASE("EditorApp: an unusable project layout path releases initialized host services without GPU errors" * doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::EditorTestFixture fixture("EditorLayoutFailure");
			const auto projectFile = CreateProcessTestProject(fixture, "LayoutFailure");
			const auto library = projectFile.parent_path() / "Library";
			REQUIRE(FileSystem::CreateDirectories(library).has_value());
			WriteProcessTestFile(library / "Editor", "Preserve this file");
			const auto result = RunRenderingEditor(fixture.GetDirectory(), { "--headless", "--project", Test::PathToUtf8(projectFile), "--frames", "1" });
			REQUIRE(result.has_value());
			CheckCleanInitializationFailure(*result);
			CHECK(FileSystem::ReadText(library / "Editor").value_or("") == "Preserve this file");
			// The partially initialized host released its project lock as well as UI/GPU resources.
			CHECK(ProjectLock::Acquire(library / "Editor.lock").has_value());
		}

		TEST_CASE("EditorApp: shutdown stores the project layout with its open panels before releasing the project and leaves the user-data layout alone"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::EditorTestFixture fixture("EditorLayoutShutdown");
			const auto projectFile = CreateProcessTestProject(fixture, "LayoutShutdown");
			const auto projectLayout = projectFile.parent_path() / "Library" / "Editor" / "imgui.ini";
			const auto userLayout = fixture.GetDirectory() / ENGINE_PRODUCT_NAME / "Editor" / "imgui.ini";
			// The launcher's layout. The project's layout replaces it before the first frame, so it is never loaded or saved.
			const std::string launcherLayout = "[EnginePanels][Open]\nPanels=Stats\n\n";
			REQUIRE(FileSystem::CreateDirectories(userLayout.parent_path()).has_value());
			WriteProcessTestFile(userLayout, launcherLayout);
			const auto runEditor = [&fixture, &projectFile]()
			{
				const auto result = RunRenderingEditor(fixture.GetDirectory(), { "--headless", "--project", Test::PathToUtf8(projectFile), "--frames", "3" });
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				INFO("editor stderr: ", result->StandardError);
				CHECK(result->ExitCode == ExitCode::Success);
				CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
			};
			runEditor();
			CHECK(FileSystem::ReadText(projectLayout).value_or("").contains("[Window]"));
			const std::string saved = ReadSavedLayoutPanels(projectLayout);
			CHECK(saved == "Panels=SceneHierarchy,Inspector,SceneViewport,GameViewport,ContentBrowser,Console");
			// Nothing after the project's layout is saved anywhere: the launcher's layout is unchanged.
			CHECK(FileSystem::ReadText(userLayout).value_or("") == launcherLayout);
			CHECK(ProjectLock::Acquire(projectFile.parent_path() / "Library" / "Editor.lock").has_value());

			// The next session opens the saved panels and saves them again at its shutdown.
			std::string layout = FileSystem::ReadText(projectLayout).value_or("");
			const size_t panels = layout.find(saved);
			REQUIRE(panels != std::string::npos);
			layout.replace(panels, saved.size(), "Panels=Automation,Console");
			WriteProcessTestFile(projectLayout, layout);
			runEditor();
			CHECK(ReadSavedLayoutPanels(projectLayout) == "Panels=Console,Automation");
			CHECK(FileSystem::ReadText(userLayout).value_or("") == launcherLayout);
		}

		TEST_CASE("EditorApp: a launcher-state editor keeps its open panels in the user-data layout across a restart"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorLauncherLayout");
			const auto userLayout = userData / ENGINE_PRODUCT_NAME / "Editor" / "imgui.ini";
			REQUIRE(FileSystem::CreateDirectories(userLayout.parent_path()).has_value());
			WriteProcessTestFile(userLayout, "[EnginePanels][Open]\nPanels=Stats,Console\n\n");
			const auto result = RunRenderingEditor(userData, { "--headless", "--frames", "3" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
			CHECK(ReadSavedLayoutPanels(userLayout) == "Panels=Console,Stats");
		}
	}

}
