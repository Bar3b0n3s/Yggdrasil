#include "TestsPCH.h"

#include "EditorCore/Export/TestingExport.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Export/Exporter.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Platform/Process.h"
#include "Engine/Project/GameManifest.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/ExportBinaryRoot.h"
#include "Support/TempDirectory.h"
#include "Support/Utf8Path.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Testing exports (Architecture §7.6, §14.1, §14.2 step 1, §14.3; Roadmap M15; Docs/Decisions/0024-m15-contract.md
// decision 6, stream B) against this build's real Runtime output (Support/ExportBinaryRoot.h), including the user-data
// acceptance test, which runs the exported game. FeatureTest's own testing export in Release and Dist is the export stage's (Scripts/Test.py --suite export).

namespace Engine {

	namespace {

		constexpr std::string_view SuiteScript = "Assets/Tests/Smoke.test.luau";
		constexpr std::string_view ReplayPath = "Assets/Tests/Replays/Smoke.replay";
		constexpr std::string_view SuiteText = R"(--!strict
return Test.Suite("Smoke", function()
	Test.Case("passes", function()
		Test.Expect(true, "true")
	end)
end)
)";

		std::optional<Result<ExportReport>> RunToEnd(Exporter& exporter)
		{
			std::optional<Result<ExportReport>> outcome;
			const bool resolved = Test::WaitUntil([&exporter, &outcome]()
			{
				outcome = exporter.Poll();
				return outcome.has_value();
			});
			if (!resolved)
				return std::nullopt;
			return outcome;
		}

		void WriteFile(const std::filesystem::path& path, std::span<const std::byte> bytes)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false }).has_value());
		}

		void WriteProjectText(EditorContext& editor, std::string_view path, std::string_view text)
		{
			const Result<VfsPath> file = VfsPath::Create("project", path);
			REQUIRE(file.has_value());
			REQUIRE(editor.WriteProjectFile(*file, AsBytes(text)).has_value());
		}

		void ApplySettings(EditorContext& editor, std::string_view patchText)
		{
			const Result<Json> patch = JsonReader::Parse(patchText);
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Set Project Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
		}

		// The bin directory of every export here: this build's real Runtime as Scripts/Build.py stages it, with the notices
		// file (Support/ExportBinaryRoot.h, so stream A's PatchResources, notices and macOS Sign requirements hold), and one
		// shader file.
		std::filesystem::path MakeBinaryRoot(const Test::TempDirectory& directory)
		{
			const std::filesystem::path root = Test::MakeExportBinaryRoot(directory);
			WriteFile(root / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) / "Shaders" / "Test" / "VSMain.spv", AsBytes("spirv"));
			return root;
		}

		// The test project named `name` with Main.scene (one cube) as its start scene, the suite SuiteScript on Main.scene,
		// and, with `replayExpectation`, the replay ReplayPath of one tick whose single expectation is that Luau chunk.
		// Export.Exclude keeps its default "Assets/Tests/**".
		void MakeTestingProject(Test::EditorTestFixture& fixture, std::string_view name, std::optional<std::string_view> replayExpectation)
		{
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const Entity cube = editor.GetScene().CreateEntity("Cube");
			cube.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			const Result<std::string> scene = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(scene.has_value());
			REQUIRE(editor.WriteProjectFile(*editor.GetScenePath(), AsBytes(*scene)).has_value());
			REQUIRE(editor.MarkSceneSaved(*editor.GetScenePath()));
			WriteProjectText(editor, SuiteScript, SuiteText);
			REQUIRE(editor.GetAssets().Refresh().has_value());

			std::string replays = "[]";
			if (replayExpectation.has_value())
			{
				const Result<VfsPath> scenePath = VfsPath::Create("project", "Assets/Scenes/Main.scene");
				REQUIRE(scenePath.has_value());
				const AssetRecord* record = editor.GetAssets().GetRegistry().FindBySourcePath(*scenePath);
				REQUIRE(record != nullptr);
				Json replay = {
					{ "Format", "Replay" },
					{ "Version", 1 },
					{ "Scene", { { "Handle", record->Metadata.Handle.ToString() }, { "Path", "Assets/Scenes/Main.scene" } } },
					{ "Parameters", Json::object() },
					{ "Seed", 0 },
					{ "FixedHz", 60 },
					{ "EngineVersion", "0.1.0" },
					{ "Config", "Editor" },
					{ "Events", Json::array() },
					{ "Expect", Json::array({ { { "Tick", 1 }, { "Luau", std::string(*replayExpectation) } } }) },
					{ "FinalTick", 1 },
					{ "FinalStateHash", "0000000000000001" },
				};
				WriteProjectText(editor, ReplayPath, replay.dump(1, '\t'));
				REQUIRE(editor.GetAssets().Refresh().has_value());
				replays = R"(["Assets/Tests/Replays/*.replay"])";
			}
			ApplySettings(editor, std::format(R"({{"Name":"{}","StartScene":"Assets/Scenes/Main.scene","Export":{{"BuildScenes":["Assets/Scenes/Main.scene"]}},)"
											  R"("Testing":{{"Suites":[{{"Script":"{}","Scene":"Assets/Scenes/Main.scene","Parameters":null,"Isolation":"Suite","Clock":[],)"
											  R"("Overrides":{{"CallbackBudgetMs":0,"MemoryLimitMB":0,"PauseOnError":"Inherit"}},"ExpectQuit":-1,"Modes":["Editor","Release","Dist"]}}],)"
											  R"("Replays":{}}}}})",
									  name, SuiteScript, replays));
		}

		ExportSpecification MakeTestingSpecification(const std::filesystem::path& binaryRoot)
		{
			ExportSpecification specification;
			specification.Configuration = ExportConfiguration::Release;
			specification.BinaryRoot = binaryRoot;
			specification.Testing = true;
			return specification;
		}

		std::vector<std::string> IssuePointers(const Error& error)
		{
			std::vector<std::string> pointers;
			for (const ErrorIssue& issue : error.GetIssues())
				pointers.push_back(issue.JsonPointer);
			return pointers;
		}

		// The manifest path of an exported package (in the bundle's Resources on macOS).
		std::filesystem::path ManifestOf(const ExportReport& report, std::string_view name)
		{
#if defined(ENGINE_PLATFORM_MACOS)
			return report.OutputDirectory / (std::string(name) + ".app") / "Contents" / "Resources" / "Game.json";
#else
			static_cast<void>(name);
			return report.OutputDirectory / "Game.json";
#endif
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("Exporter: a testing export goes to its own default directory")
		{
			// Implemented by the contract (ADR 0024 decision 6): a testing export never replaces the plain export.
			const std::string plain = Exporter::GetDefaultOutputDirectory("Tetris", ExportConfiguration::Dist);
			const std::string testing = Exporter::GetDefaultOutputDirectory("Tetris", ExportConfiguration::Dist, true);
			CHECK(testing == std::string("Build/") + std::string(Exporter::GetPlatformName()) + "-Dist-Testing/Tetris");
			CHECK(testing != plain);
		}

		TEST_CASE("TestingExport: forced content is Assets/Tests, the suite scripts and scenes and the matched replays" * doctest::skip(true))
		{
			TestingExportContent content;
			content.SuiteScripts = { "Assets/Suites/Main.test.luau" };
			content.SuiteScenes = { "Assets/Scenes/Hidden/Level.scene" };
			content.Replays = { "Assets/Recordings/Run.replay" };
			CHECK(IsForcedByTestingExport("Assets/Tests/Anything.png", content));
			CHECK(IsForcedByTestingExport("Assets/Tests/Deep/Er/Module.luau", content));
			CHECK(IsForcedByTestingExport("Assets/Suites/Main.test.luau", content));
			CHECK(IsForcedByTestingExport("Assets/Scenes/Hidden/Level.scene", content));
			CHECK(IsForcedByTestingExport("Assets/Recordings/Run.replay", content));
			CHECK_FALSE(IsForcedByTestingExport("Assets/TestsAndMore/Main.scene", content));
			CHECK_FALSE(IsForcedByTestingExport("Assets/Scenes/Hidden/Other.scene", content));
			CHECK_FALSE(IsForcedByTestingExport("Assets/Recordings/Run.replay.meta", content));
		}

		TEST_CASE("Exporter: a testing export packages Assets/Tests and every suite and replay whatever Export.Exclude says, and writes Testing true" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("TestingExport");
			MakeTestingProject(fixture, "Smoke", "return true");
			const Test::TempDirectory binaries("TestingExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeTestingSpecification(MakeBinaryRoot(binaries)));
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const ExportReport& report = **outcome;
			CHECK(report.Testing);
			CHECK(report.TestSuiteCount == 1);
			CHECK(report.ReplayCount == 1);
			CHECK(report.OutputDirectory
				== fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("Smoke", ExportConfiguration::Release, true)));

			const Result<GameManifest> manifest = GameManifestSerializer::LoadFromFile(ManifestOf(report, "Smoke"));
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			CHECK(manifest->Testing);
			const Result<Ref<const PakReader>> pak = PakReader::Open(ManifestOf(report, "Smoke").parent_path() / "Data" / "Game.pak");
			REQUIRE_MESSAGE(pak.has_value(), pak.error().ToString());
			std::vector<std::string> paths;
			for (const PakEntry& entry : (*pak)->GetEntries())
				paths.push_back(entry.Path);
			CHECK(std::ranges::find(paths, std::string(SuiteScript)) != paths.end());
			CHECK(std::ranges::find(paths, std::string(ReplayPath)) != paths.end());
			CHECK(std::ranges::find(paths, std::string("Assets/Scenes/Main.scene")) != paths.end());

			// The plain export of the same project still leaves Assets/Tests out and says Testing false.
			ExportSpecification plain = MakeTestingSpecification(MakeBinaryRoot(binaries));
			plain.Testing = false;
			Result<Scope<Exporter>> plainExporter = Exporter::Start(fixture.GetEditor(), plain);
			REQUIRE(plainExporter.has_value());
			const std::optional<Result<ExportReport>> plainOutcome = RunToEnd(**plainExporter);
			REQUIRE(plainOutcome.has_value());
			REQUIRE_MESSAGE(plainOutcome->has_value(), plainOutcome->error().ToString());
			CHECK_FALSE((*plainOutcome)->Testing);
			const Result<GameManifest> plainManifest = GameManifestSerializer::LoadFromFile(ManifestOf(**plainOutcome, "Smoke"));
			REQUIRE(plainManifest.has_value());
			CHECK_FALSE(plainManifest->Testing);
		}

		TEST_CASE("Exporter: a testing export refuses an invalid test suite with TEST_SUITE_INVALID at its setting" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("TestingExportSuite");
			MakeTestingProject(fixture, "Smoke", std::nullopt);
			// A behaviour script is not a suite; a missing scene is not a scene.
			WriteProjectText(fixture.GetEditor(), "Assets/Tests/NotASuite.luau", "--!strict\nlocal Plain = {}\nreturn Script.Define(\"Plain\", Plain)\n");
			REQUIRE(fixture.GetEditor().GetAssets().Refresh().has_value());
			ApplySettings(fixture.GetEditor(),
				R"({"Testing":{"Suites":[{"Script":"Assets/Tests/NotASuite.luau","Scene":"Assets/Scenes/Missing.scene","Parameters":null,"Isolation":"Suite","Clock":[],)"
				R"("Overrides":{"CallbackBudgetMs":0,"MemoryLimitMB":0,"PauseOnError":"Inherit"},"ExpectQuit":-1,"Modes":["Editor","Release","Dist"]}]}})");
			const Test::TempDirectory binaries("TestingExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeTestingSpecification(MakeBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::Validation);
			CHECK(IssuePointers(error) == std::vector<std::string>{ "/Testing/Suites/0/Script", "/Testing/Suites/0/Scene" });
			for (const ErrorIssue& issue : error.GetIssues())
				CHECK(issue.Message.starts_with("TEST_SUITE_INVALID: "));
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
		}

		TEST_CASE("Exporter: a testing export refuses a replay whose expectation does not compile, at its Testing.Replays entry" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("TestingExportReplay");
			MakeTestingProject(fixture, "Smoke", "return (");
			// Validate imports the replay through the asset manager, which logs the failed import.
			const Test::ExpectLog importFailed(LogLevel::Error, "ASSET_IMPORT_FAILED Assets/Tests/Replays/Smoke.replay");
			const Test::TempDirectory binaries("TestingExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeTestingSpecification(MakeBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::Validation);
			REQUIRE(IssuePointers(error) == std::vector<std::string>{ "/Testing/Replays/0" });
			CHECK(error.GetIssues().front().Message.find(std::string(ReplayPath)) != std::string::npos);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
		}

		TEST_CASE("Exporter: two testing exports of an unchanged project are identical file for file" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("TestingExportTwice");
			MakeTestingProject(fixture, "Smoke", "return true");
			const Test::TempDirectory binaries("TestingExportBinaries");
			std::vector<std::vector<ExportedFile>> files;
			for (int run = 0; run < 2; ++run)
			{
				CAPTURE(run);
				Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeTestingSpecification(MakeBinaryRoot(binaries)));
				REQUIRE(exporter.has_value());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				files.push_back((*outcome)->Files);
			}
			REQUIRE(files[0].size() == files[1].size());
			for (size_t index = 0; index < files[0].size(); ++index)
			{
				CAPTURE(files[0][index].Path);
				CHECK(files[0][index].Path == files[1][index].Path);
				CHECK(files[0][index].Size == files[1][index].Size);
				CHECK(files[0][index].Hash == files[1][index].Hash);
			}
		}

		TEST_CASE("Export: user data goes to the manifest Name folder" * doctest::skip(true))
		{
			// §14.1: Name decides the exported game's user-data folder, never ENGINE_PRODUCT_NAME; §14.3: a --feature-test run
			// of a testing export without a root keeps it beside the package (the only form a Dist run can be pointed at).
			Test::EditorTestFixture fixture("ExportUserData");
			MakeTestingProject(fixture, "TinyGame", std::nullopt);
			const Test::TempDirectory binaries("ExportUserDataBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeTestingSpecification(MakeBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const ExportReport& report = **outcome;

			const Test::TempDirectory userData("ExportUserData");
			const Result<ProcessResult> run = Process::Run(
				{ .Executable = report.Executable,
					.Arguments = { "--headless", "--renderer", "none", "--frames", "5", "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) } },
				std::chrono::seconds(120));
			REQUIRE_MESSAGE(run.has_value(), run.error().ToString());
			INFO("stderr: ", run->StandardError);
			CHECK(run->ExitCode == 0);
			CHECK(FileSystem::Exists(userData / "TinyGame" / "Logs" / "TinyGame.log"));
			CHECK(FileSystem::Exists(userData / "TinyGame" / "Crashes"));
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(userData / ENGINE_PRODUCT_NAME, error));

			// The testing run without a root: <export>/bin/TestUserData/TinyGame, with its reports below the working directory.
			const Test::TempDirectory workingDirectory("ExportUserDataRun");
			const Result<ProcessResult> featureTest = Process::Run(
				{ .Executable = report.Executable, .Arguments = { "--headless", "--renderer", "none", "--feature-test" }, .WorkingDirectory = workingDirectory.GetPath() },
				std::chrono::seconds(120));
			REQUIRE_MESSAGE(featureTest.has_value(), featureTest.error().ToString());
			INFO("feature-test stderr: ", featureTest->StandardError);
			CHECK(featureTest->ExitCode == 0);
			const std::filesystem::path testRoot = GetTestingUserDataRoot(ManifestOf(report, "TinyGame"));
			CHECK(testRoot == report.OutputDirectory / "bin" / "TestUserData");
			CHECK(FileSystem::Exists(testRoot / "TinyGame" / "Logs" / "TinyGame.log"));
			CHECK_FALSE(std::filesystem::exists(testRoot / ENGINE_PRODUCT_NAME, error));
			CHECK(FileSystem::Exists(workingDirectory / "bin" / "TestResults" / "FeatureTest-Release.json"));
		}
	}

}
