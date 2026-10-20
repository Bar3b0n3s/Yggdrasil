#include "TestsPCH.h"

#include "EditorCore/Export/ExportFeatureTest.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Export/Exporter.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Platform/Process.h"
#include "Engine/Project/GameManifest.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExportBinaryRoot.h"
#include "Support/TempDirectory.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Step 6a's run of an exported testing package (Exporter.h, Export/ExportFeatureTest.h; Docs/Decisions/0030-m19-contract.md
// decision 10, A-DISTSELF), started on packages that this build's real Runtime output (Support/ExportBinaryRoot.h) exported.
// project.export {featureTest} through the exporter is ExportMethodsTests.cpp's and Tests/Automation/test_testing_export.py's.

namespace Engine {

	namespace {

		// The project EditorTestFixture::CreateAndOpenProject creates, and so the exported game's Name.
		constexpr std::string_view GameName = "TestProject";
		constexpr std::string_view RecordedReplay = "Assets/Tests/Recorded.replay";
		constexpr std::string_view DriftedReplay = "Assets/Tests/Drifted.replay";
		// The bound of an exported game's run, a failure bound only (a Debug Runtime takes seconds).
		constexpr std::chrono::seconds RunBound{ 300 };

		void WriteFile(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, AsBytes(text), { .KeepBackup = false }).has_value());
		}

		void ApplySettings(EditorContext& editor, std::string_view patchText)
		{
			const Result<Json> patch = JsonReader::Parse(patchText);
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Set Project Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
		}

		// The bin directory of every export here: this build's real Runtime as Scripts/Build.py stages it, and one shader file.
		std::filesystem::path MakeBinaryRoot(const Test::TempDirectory& directory)
		{
			const std::filesystem::path root = Test::MakeExportBinaryRoot(directory);
			WriteFile(root / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) / "Shaders" / "Test" / "VSMain.spv", "spirv");
			return root;
		}

		// The fixture's project with Main.scene open as its start and build scene.
		void MakeExportableProject(Test::EditorTestFixture& fixture)
		{
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			ApplySettings(fixture.GetEditor(), R"({"StartScene":"Assets/Scenes/Main.scene","Export":{"BuildScenes":["Assets/Scenes/Main.scene"]}})");
		}

		// Records RecordedReplay over 30 ticks of the open scene through input.record and returns its final state hash.
		std::string RecordReplay(EditorContext& editor)
		{
			Test::AutomationTestClient client(editor);
			REQUIRE(client.Call("scene.save", Json::object()).has_value());
			REQUIRE(client.Call("input.record", Json{ { "action", "start" } }).has_value());
			REQUIRE(client.Call("play.step", Json{ { "ticks", 30 }, { "render", "none" } }).has_value());
			const Result<Json> recorded = client.Call("input.record", Json{ { "action", "stop" }, { "path", std::string(RecordedReplay) } });
			REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
			const Result<std::string> stateHash = JsonReader((*recorded)["stateHash"]).ReadString();
			REQUIRE(stateHash.has_value());
			return *stateHash;
		}

		// Writes DriftedReplay: 10 ticks of Main.scene without input, whose recorded final state hash no run reproduces.
		void WriteDriftedReplay(EditorContext& editor)
		{
			const Result<VfsPath> scenePath = VfsPath::Create("project", "Assets/Scenes/Main.scene");
			REQUIRE(scenePath.has_value());
			const AssetRecord* record = editor.GetAssets().GetRegistry().FindBySourcePath(*scenePath);
			REQUIRE(record != nullptr);
			const Json replay = {
				{ "Format", "Replay" },
				{ "Version", 1 },
				{ "Scene", { { "Handle", record->Metadata.Handle.ToString() }, { "Path", "Assets/Scenes/Main.scene" } } },
				{ "Parameters", Json::object() },
				{ "Seed", 0 },
				{ "FixedHz", 60 },
				{ "EngineVersion", "0.1.0" },
				{ "Config", "Editor" },
				{ "Events", Json::array() },
				{ "Expect", Json::array() },
				{ "FinalTick", 10 },
				{ "FinalStateHash", "0000000000000001" },
			};
			const Result<VfsPath> file = VfsPath::Create("project", DriftedReplay);
			REQUIRE(file.has_value());
			const std::string text = replay.dump(1, '\t');
			REQUIRE(editor.WriteProjectFile(*file, AsBytes(text)).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
		}

		// Exports the open project in Release into its default directory, for testing when `testing`, and returns the report.
		ExportReport ExportPackage(EditorContext& editor, const std::filesystem::path& binaryRoot, bool testing)
		{
			ExportSpecification specification;
			specification.Configuration = ExportConfiguration::Release;
			specification.BinaryRoot = binaryRoot;
			specification.Testing = testing;
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			std::optional<Result<ExportReport>> outcome;
			const bool resolved = Test::WaitUntil([&exporter, &outcome]()
			{
				outcome = (*exporter)->Poll();
				return outcome.has_value();
			});
			REQUIRE(resolved);
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			return std::move(**outcome);
		}

		// The manifest path of an exported package (in the bundle's Resources on macOS).
		std::filesystem::path ManifestOf(const ExportReport& report)
		{
#if defined(ENGINE_PLATFORM_MACOS)
			return report.OutputDirectory / (std::string(GameName) + ".app") / "Contents" / "Resources" / "Game.json";
#else
			return report.OutputDirectory / "Game.json";
#endif
		}

		// Step 6a's run of the exported Release `package` without a graphics device, as if step 7 then moved it to `outputDirectory`.
		ExportFeatureTestRun MakeRun(const ExportReport& package, const std::filesystem::path& outputDirectory)
		{
			return ExportFeatureTestRun{
				.Executable = package.Executable,
				.StagedManifest = ManifestOf(package),
				.OutputDirectory = outputDirectory,
				.Name = std::string(GameName),
				.Configuration = ExportConfiguration::Release,
				.HasGraphicsDevice = false,
			};
		}

		// Polls `job` until it resolves.
		std::optional<Result<ExportFeatureTestOutcome>> PollToEnd(IExportFeatureTestJob& job)
		{
			std::optional<Result<ExportFeatureTestOutcome>> outcome;
			const bool resolved = Test::WaitUntil([&job, &outcome]()
			{
				outcome = job.Poll();
				return outcome.has_value();
			}, RunBound);
			if (!resolved)
				return std::nullopt;
			return outcome;
		}

		// {replay path: finalStateHash} of the "replay:<path>" suite rows of the FeatureTest report `report`, as the games
		// suite's Dist half reads its own run's (Scripts/Lib/exports.py read_replay_hashes).
		std::map<std::string, std::string> ReadReplayHashes(const std::filesystem::path& report)
		{
			constexpr std::string_view ReplayPrefix = "replay:";
			const Result<std::string> text = FileSystem::ReadText(report);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const Result<Json> document = JsonReader::Parse(*text);
			REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
			const Result<JsonReader> suites = JsonReader(*document).GetMember("suites");
			REQUIRE_MESSAGE(suites.has_value(), suites.error().ToString());
			std::map<std::string, std::string> hashes;
			for (size_t index = 0; index < suites->GetArraySize().value_or(0); ++index)
			{
				const Result<JsonReader> row = suites->GetElement(index);
				REQUIRE(row.has_value());
				const std::string suite = row->ReadMember<std::string>("suite").value_or(std::string());
				if (suite.starts_with(ReplayPrefix))
					hashes[suite.substr(ReplayPrefix.size())] = row->ReadMember<std::string>("finalStateHash").value_or(std::string());
			}
			return hashes;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ExportFeatureTest: the run reports every replay's observed state hash, and a failing run is no error")
		{
			// Decision 10: the replays sorted by path, each with the hash the package's own --feature-test run reads and
			// whether it passed; a run that fails (a replay whose recorded hash differs) reports its exit code and failed rows
			// instead of failing the export; the report lies in the package's testing user data, where step 7 moves it.
			Test::EditorTestFixture fixture("ExportFeatureTest");
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			const std::string recordedHash = RecordReplay(editor);
			WriteDriftedReplay(editor);
			ApplySettings(editor, R"({"Testing":{"Replays":["Assets/Tests/*.replay"]}})");
			const Test::TempDirectory binaries("ExportFeatureTestBinaries");
			const ExportReport package = ExportPackage(editor, MakeBinaryRoot(binaries), true);

			const Test::TempDirectory moved("ExportFeatureTestMoved");
			Result<Scope<IExportFeatureTestJob>> job = StartExportFeatureTest(MakeRun(package, moved.GetPath()));
			REQUIRE_MESSAGE(job.has_value(), job.error().ToString());
			const std::optional<Result<ExportFeatureTestOutcome>> outcome = PollToEnd(**job);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const ExportFeatureTestReport& report = (*outcome)->Report;
			CHECK((*outcome)->Warnings.empty());
			CHECK(report.Ran);
			CHECK(report.ExitCode != 0);
			CHECK_FALSE(report.Passed);
			REQUIRE(report.Replays.size() == 2);
			CHECK(report.Replays[0].Path == DriftedReplay);
			CHECK_FALSE(report.Replays[0].Passed);
			CHECK(report.Replays[0].StateHash.size() == 16);
			CHECK(report.Replays[0].StateHash != "0000000000000001");
			CHECK(report.Replays[1].Path == RecordedReplay);
			CHECK(report.Replays[1].Passed);
			CHECK(report.Replays[1].StateHash == recordedHash);

			// The report, below the run's working directory (its testing user data), is named for the package's place after the
			// move, and lies at the same place in the staged package.
			const std::filesystem::path relative = report.ReportPath.lexically_relative(moved.GetPath());
			CHECK(relative == std::filesystem::path("bin") / "TestUserData" / std::filesystem::path(GameName) / "bin" / "TestResults" / "FeatureTest-Release.json");
			CHECK(FileSystem::Exists(package.OutputDirectory / relative));

			// The package's own --feature-test run, as the games suite's Dist half runs it, reads the same hashes.
			const Test::TempDirectory working("ExportFeatureTestOwnRun");
			const Result<ProcessResult> own = Process::Run(
				{ .Executable = package.Executable, .Arguments = { "--headless", "--renderer", "none", "--feature-test" }, .WorkingDirectory = working.GetPath() },
				RunBound);
			REQUIRE_MESSAGE(own.has_value(), own.error().ToString());
			CHECK(own->ExitCode == report.ExitCode);
			const std::map<std::string, std::string> ownHashes = ReadReplayHashes(working.GetPath() / "bin" / "TestResults" / "FeatureTest-Release.json");
			std::map<std::string, std::string> reported;
			for (const ExportFeatureTestReplay& replay : report.Replays)
				reported[replay.Path] = replay.StateHash;
			CHECK(reported == ownHashes);
		}

		TEST_CASE("ExportFeatureTest: a Dist run from an editor without a graphics device does not run and warns")
		{
			// Decision 10: a Dist Runtime has no --renderer none, so without a device nothing runs (here, an executable that
			// does not exist is never started): Ran false, no replays, one warning, and the same outcome when polled again.
			const Test::TempDirectory package("ExportFeatureTestDist");
			const ExportFeatureTestRun run{
				.Executable = package.GetPath() / "Missing.exe",
				.StagedManifest = package.GetPath() / "Game.json",
				.OutputDirectory = package.GetPath() / "Moved",
				.Name = std::string(GameName),
				.Configuration = ExportConfiguration::Dist,
				.HasGraphicsDevice = false,
			};
			Result<Scope<IExportFeatureTestJob>> job = StartExportFeatureTest(run);
			REQUIRE_MESSAGE(job.has_value(), job.error().ToString());
			for (int poll = 0; poll < 2; ++poll)
			{
				const std::optional<Result<ExportFeatureTestOutcome>> outcome = (*job)->Poll();
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				CHECK_FALSE((*outcome)->Report.Ran);
				CHECK((*outcome)->Report.Replays.empty());
				CHECK((*outcome)->Report.ReportPath.empty());
				CHECK((*outcome)->Warnings.size() == 1);
			}
		}

		TEST_CASE("ExportFeatureTest: a run that cannot start fails")
		{
			const Test::TempDirectory package("ExportFeatureTestMissing");
			const ExportFeatureTestRun run{
				.Executable = package.GetPath() / "Missing.exe",
				.StagedManifest = package.GetPath() / "Game.json",
				.OutputDirectory = package.GetPath() / "Moved",
				.Name = std::string(GameName),
				.Configuration = ExportConfiguration::Release,
				.HasGraphicsDevice = false,
			};
			const Result<Scope<IExportFeatureTestJob>> job = StartExportFeatureTest(run);
			REQUIRE_FALSE(job.has_value());
			CHECK(job.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("ExportFeatureTest: a run that writes no report fails, whatever report an earlier run left")
		{
			// A plain export's game refuses --feature-test (its Game.json says Testing false) and writes no report; an
			// earlier run's report at its place never passes for this run's.
			Test::EditorTestFixture fixture("ExportFeatureTestNoReport");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportFeatureTestNoReportBinaries");
			const ExportReport package = ExportPackage(fixture.GetEditor(), MakeBinaryRoot(binaries), false);
			const std::filesystem::path earlier =
				GetTestingUserDataRoot(ManifestOf(package)) / std::filesystem::path(GameName) / "bin" / "TestResults" / "FeatureTest-Release.json";
			WriteFile(earlier, R"({"mode":"Release","passed":true,"cases":[],"suites":[]})");

			const Test::TempDirectory moved("ExportFeatureTestNoReportMoved");
			Result<Scope<IExportFeatureTestJob>> job = StartExportFeatureTest(MakeRun(package, moved.GetPath()));
			REQUIRE_MESSAGE(job.has_value(), job.error().ToString());
			const std::optional<Result<ExportFeatureTestOutcome>> outcome = PollToEnd(**job);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			CHECK(outcome->error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(FileSystem::Exists(earlier));
		}
	}

}
