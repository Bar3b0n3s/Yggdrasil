#include "TestsPCH.h"

#include "EditorCore/Automation/ExportMethods.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExportBinaryRoot.h"
#include "Support/TempDirectory.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// project.export in process (Architecture §13.5, §14.2): the param checks, the pending operation and its cancellation by a
// disconnect. The exports themselves are tested by EditorCore/Export/ExporterTests.cpp and Tests/Automation/test_export.py.

namespace Engine {

	namespace {

		void WriteTestFile(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, AsBytes(text), { .KeepBackup = false }).has_value());
		}

		// A bin directory with this build's real Runtime as Scripts/Build.py stages it, the notices file
		// (Support/ExportBinaryRoot.h, so M15's exporter requirements hold) and one shader. The exports here never run it.
		std::filesystem::path MakeBinaryRoot(const Test::TempDirectory& directory)
		{
			const std::filesystem::path root = Test::MakeExportBinaryRoot(directory);
			WriteTestFile(root / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) / "Shaders" / "Test" / "VSMain.spv", "spirv");
			return root;
		}

		void MakeExportable(EditorContext& editor)
		{
			const Result<Json> patch = JsonReader::Parse(R"({"StartScene":"Assets/Scenes/Main.scene","Export":{"BuildScenes":["Assets/Scenes/Main.scene"]}})");
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Export Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
		}

		AutomationServerSpecification MakeExportServerSpecification(const std::filesystem::path& binaryRoot)
		{
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.ExportBinaryRoot = binaryRoot;
			return specification;
		}

		// The response to project.export {params}, pumped until it arrives: an export whose feature test runs the exported game
		// outlasts the pumps of AutomationTestClient::Request, so the wait is bounded by time, a failure bound only.
		Json RequestExportUntilAnswered(Test::AutomationTestClient& client, const Json& params)
		{
			const int64_t id = client.Submit("project.export", params);
			AutomationServer& server = client.GetServer();
			const ClientId requester = client.GetClient();
			Json response;
			const bool answered = Test::WaitUntil([&server, requester, id, &response]()
			{
				server.Pump();
				for (Json& message : server.TakeInProcessResponses(requester))
				{
					if (message.contains("id") && message["id"] == Json(id))
					{
						response = std::move(message);
						return true;
					}
				}
				return false;
			}, std::chrono::seconds(300));
			REQUIRE(answered);
			return response;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ExportMethods: project.export accepts testing true and false, and a testing export reports its suites and replays")
		{
			// M15 (ADR 0024 decision 6): testing is accepted either way; a testing export resolves with testing true, its suite
			// count and its replay count, in its own default directory, and a plain export with testing false.
			Test::EditorTestFixture fixture("ExportMethods");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			MakeExportable(fixture.GetEditor());
			EditorContext& editor = fixture.GetEditor();
			const Result<VfsPath> suite = VfsPath::Create("project", "Assets/Tests/Smoke.test.luau");
			REQUIRE(suite.has_value());
			constexpr std::string_view SuiteText = R"(--!strict
return Test.Suite("Smoke", function()
	Test.Case("passes", function() end)
end)
)";
			REQUIRE(editor.WriteProjectFile(*suite, AsBytes(SuiteText)).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const Result<Json> patch = JsonReader::Parse(R"({"Testing":{"Suites":[{"Script":"Assets/Tests/Smoke.test.luau","Scene":"Assets/Scenes/Main.scene"}]}})");
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Testing Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());

			const Test::TempDirectory binaries("ExportBinaries");
			Test::AutomationTestClient client(editor, MakeExportServerSpecification(MakeBinaryRoot(binaries)));
			const Result<Json> testing = client.Call("project.export", Json{ { "config", "release" }, { "testing", true } });
			REQUIRE_MESSAGE(testing.has_value(), testing.error().ToString());
			CHECK((*testing)["testing"] == Json(true));
			CHECK((*testing)["testSuiteCount"] == Json(1));
			CHECK((*testing)["replayCount"] == Json(0));
			const std::filesystem::path output =
				fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("TestProject", ExportConfiguration::Release, true));
			CHECK((*testing)["outputDirectory"] == Json(FileSystem::PathToUtf8(output)));

			const Result<Json> plain = client.Call("project.export", Json{ { "config", "release" }, { "testing", false } });
			REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
			CHECK((*plain)["testing"] == Json(false));
			CHECK((*plain)["testSuiteCount"] == Json(0));
			CHECK((*plain)["replayCount"] == Json(0));
			CHECK((*plain)["outputDirectory"]
				== Json(FileSystem::PathToUtf8(fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("TestProject", ExportConfiguration::Release)))));
		}

		TEST_CASE("ExportMethods: project.export refuses output directories outside Build")
		{
			Test::AutomationFixture fixture("ExportMethods");
			const Json outside = fixture.Request("project.export", Json{ { "config", "release" }, { "outDir", "Assets/Game" } });
			CHECK(outside["error"]["code"] == Json(-32602));
			CHECK(outside["error"]["data"]["issues"][0]["pointer"] == Json("/outDir"));

			const Json escaping = fixture.Request("project.export", Json{ { "config", "release" }, { "outDir", "Build/../Assets" } });
			CHECK(escaping["error"]["code"] == Json(-32602));
			CHECK(escaping["error"]["data"]["issues"][0]["pointer"] == Json("/outDir"));
		}

		TEST_CASE("ExportMethods: project.export needs its config and supports no dry run")
		{
			Test::AutomationFixture fixture("ExportMethods");
			const Json missing = fixture.Request("project.export", Json::object());
			CHECK(missing["error"]["code"] == Json(-32602));
			const Json dryRun = fixture.Request("project.export", Json{ { "config", "dist" }, { "dryRun", true } });
			CHECK(dryRun["error"]["code"] == Json(-32009));
		}

		// The review of W19.1 (Docs/Decisions/0030-m19-contract.md decision 10): the MCP bridge waits for a call as long as its
		// method's timeoutSeconds, and after cooking a testing export runs the smoke test (at most 300 s, Exporter.cpp) and
		// then the package's feature test (at most 1800 s, ExportFeatureTest.cpp), so the declared 900 s dropped the result of
		// an export whose feature test ran long.
		TEST_CASE("ExportMethods: project.export declares a timeout that outlasts its smoke test and its feature test")
		{
			constexpr uint32_t SmokeTestBoundSeconds = 300;
			constexpr uint32_t FeatureTestBoundSeconds = 1800;
			Test::EditorTestFixture fixture("ExportTimeout");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterExportMethods(methods);
			methods.Freeze();
			const MethodDescriptor* method = methods.Find("project.export");
			REQUIRE(method != nullptr);
			CHECK(method->Specification.TimeoutSeconds > SmokeTestBoundSeconds + FeatureTestBoundSeconds);
		}

		TEST_CASE("ExportMethods: project.export is Unsupported in an editor without build outputs")
		{
			// The editor passes <repository>/bin (EditorApp); a server without it cannot export.
			Test::AutomationFixture fixture("ExportMethods");
			const Json response = fixture.Request("project.export", Json{ { "config", "Release" } });
			CHECK(response["error"]["code"] == Json(-32009));
			CHECK(response["error"]["data"]["errorCode"] == Json("Unsupported"));
		}

		TEST_CASE("ExportMethods: project.export resolves with the validation failure of an unexportable project")
		{
			Test::EditorTestFixture fixture("ExportMethods");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const Test::TempDirectory binaries("ExportBinaries");
			Test::AutomationTestClient client(fixture.GetEditor(), MakeExportServerSpecification(MakeBinaryRoot(binaries)));
			const Json response = client.Request("project.export", Json{ { "config", "Release" } });
			CHECK(response["error"]["code"] == Json(-32003));
			CHECK(response["error"]["data"]["issues"][0]["pointer"] == Json("/StartScene"));
		}

		TEST_CASE("ExportMethods: project.export resolves with the export report")
		{
			Test::EditorTestFixture fixture("ExportMethods");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			MakeExportable(fixture.GetEditor());
			const Test::TempDirectory binaries("ExportBinaries");
			Test::AutomationTestClient client(fixture.GetEditor(), MakeExportServerSpecification(MakeBinaryRoot(binaries)));
			const Result<Json> result = client.Call("project.export", Json{ { "config", "release" }, { "outDir", "Build/Custom/Game" } });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			const std::filesystem::path output = fixture.GetProjectRoot() / "Build" / "Custom" / "Game";
			CHECK((*result)["outputDirectory"] == Json(FileSystem::PathToUtf8(output)));
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK((*result)["executable"] == Json(FileSystem::PathToUtf8(output / "TestProject.exe")));
#else
			CHECK((*result)["executable"] == Json(FileSystem::PathToUtf8(output / "TestProject")));
#endif
			CHECK((*result)["config"] == Json("Release"));
			CHECK((*result)["smokeTestRan"] == Json(false));
			CHECK((*result)["gameAssetCount"] == Json(1));   // the scene
			CHECK((*result)["engineEntryCount"] == Json(1)); // the shader
			const Json& files = (*result)["files"];
			REQUIRE(files.is_array());
			bool listsManifest = false;
			for (const Json& file : files)
			{
				const Result<std::string> hash = JsonReader(file["hash"]).ReadString();
				REQUIRE(hash.has_value());
				CHECK(hash->size() == 16);
				listsManifest = listsManifest || file["path"] == Json("Game.json");
			}
			CHECK(listsManifest);
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(output / "Game.json", error));
		}

		TEST_CASE("ExportMethods: project.export writes under the export root the editor was started with")
		{
			// Editor --export-root (§13.4; ADR 0024 decision 3): AutomationServerSpecification::ExportRoot replaces the project's
			// Build/ directory, and nothing is written under the project.
			Test::EditorTestFixture fixture("ExportMethods");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			MakeExportable(fixture.GetEditor());
			const Test::TempDirectory binaries("ExportBinaries");
			const Test::TempDirectory exports("ExportRoot");
			AutomationServerSpecification specification = MakeExportServerSpecification(MakeBinaryRoot(binaries));
			specification.ExportRoot = exports.GetPath();
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			const Result<Json> result = client.Call("project.export", Json{ { "config", "release" } });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			const std::filesystem::path output = exports.GetPath() / (std::string(Exporter::GetPlatformName()) + "-Release") / "TestProject";
			CHECK((*result)["outputDirectory"] == Json(FileSystem::PathToUtf8(output)));
			const Result<std::string> executable = JsonReader((*result)["executable"]).ReadString();
			REQUIRE(executable.has_value());
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(FileSystem::PathFromUtf8(*executable), error));
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", error));
		}

		TEST_CASE("ExportMethods: project.export reports the testing, resource and phase-timing fields")
		{
			// M15 (ADR 0024 decisions 2, 4 and 6; stream B adds the fields to ProjectExportResult with their descriptions).
			Test::EditorTestFixture fixture("ExportMethods");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			MakeExportable(fixture.GetEditor());
			const Test::TempDirectory binaries("ExportBinaries");
			Test::AutomationTestClient client(fixture.GetEditor(), MakeExportServerSpecification(MakeBinaryRoot(binaries)));
			const Result<Json> result = client.Call("project.export", Json{ { "config", "release" }, { "testing", false } });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			for (const char* member : { "testing", "testSuiteCount", "replayCount", "resourcesPatched", "phaseTimings" })
			{
				CAPTURE(member);
				REQUIRE(result->contains(member));
			}
			CHECK((*result)["testing"] == Json(false));
			CHECK((*result)["testSuiteCount"] == Json(0));
			CHECK((*result)["replayCount"] == Json(0));
			// The executable's resources are written on Windows (the version resource) and macOS (Info.plist); Linux
			// executables carry none (Exporter.h step 4a, ExportReport::ResourcesPatched).
#if defined(ENGINE_PLATFORM_WINDOWS) || defined(ENGINE_PLATFORM_MACOS)
			CHECK((*result)["resourcesPatched"] == Json(true));
#else
			CHECK((*result)["resourcesPatched"] == Json(false));
#endif
			// One timing per phase entered, in phase order: every phase from Validate to MoveToOutput, a phase with nothing
			// to do on this host (Sign off macOS) included (ADR 0024 decision 2), but SmokeTest only when the smoke test ran,
			// and smokeTestExitCode null without one (M16 friction S2-06: it was 0, beside a SmokeTest timing).
			CHECK((*result)["smokeTestRan"] == Json(false));
			REQUIRE(result->contains("smokeTestExitCode"));
			CHECK((*result)["smokeTestExitCode"].is_null());
			const Json& timings = (*result)["phaseTimings"];
			REQUIRE(timings.is_array());
			std::vector<std::string> phases;
			for (const Json& timing : timings)
			{
				REQUIRE(timing.contains("phase"));
				REQUIRE(timing.contains("seconds"));
				CHECK(timing["seconds"].is_number());
				CHECK(JsonReader(timing["seconds"]).ReadDouble().value_or(-1.0) >= 0.0);
				phases.push_back(JsonReader(timing["phase"]).ReadString().value_or(std::string()));
			}
			CHECK(phases == std::vector<std::string>{ "Validate", "Cook", "WritePaks", "CopyRuntime", "PatchResources", "WriteManifest", "Sign", "MoveToOutput" });
		}

		// M16 friction S2-06: the report of an export whose smoke test ran carries its exit code, and of one without the code
		// null (MakeProjectExportResult is what project.export and Editor --export-report both write).
		TEST_CASE("ExportMethods: the export result reports the smoke test's exit code only when it ran")
		{
			ExportReport report;
			report.PhaseTimings = { { .Phase = ExportPhase::Validate, .Seconds = 0.5 }, { .Phase = ExportPhase::MoveToOutput, .Seconds = 0.25 } };
			const ProjectExportResult notRun = MakeProjectExportResult(report);
			CHECK_FALSE(notRun.SmokeTestRan);
			CHECK(notRun.SmokeTestExitCode.IsNull());

			report.SmokeTestRan = true;
			report.SmokeTestExitCode = 0;
			report.PhaseTimings.insert(report.PhaseTimings.begin() + 1, { .Phase = ExportPhase::SmokeTest, .Seconds = 2.0 });
			const ProjectExportResult ran = MakeProjectExportResult(report);
			CHECK(ran.SmokeTestRan);
			CHECK(ran.SmokeTestExitCode.Get() == Json(0));
			REQUIRE(ran.PhaseTimings.size() == 3);
			CHECK(ran.PhaseTimings[1].Phase == ExportPhase::SmokeTest);
		}

		TEST_CASE("ExportMethods: a disconnect cancels project.export and removes what it wrote")
		{
			Test::EditorTestFixture fixture("ExportMethods");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			MakeExportable(fixture.GetEditor());
			const Test::TempDirectory binaries("ExportBinaries");
			Test::AutomationTestClient owner(fixture.GetEditor(), MakeExportServerSpecification(MakeBinaryRoot(binaries)));
			AutomationServer& server = owner.GetServer();
			const ClientId other = server.ConnectInProcess("other");
			server.SubmitInProcess(other, RpcRequest{
											  .Id = Json(1),
											  .IsNotification = false,
											  .Method = "project.export",
											  .Params = Json{ { "config", "Release" } },
											  .TranscriptLine = std::nullopt,
										  });
			// Pumped until the export writes into its staging directory below Build/ (the paks come first).
			const std::filesystem::path build = fixture.GetProjectRoot() / "Build";
			const bool writing = Test::WaitUntil([&server, &build]()
			{
				server.Pump();
				std::error_code error;
				return std::filesystem::exists(build, error);
			});
			REQUIRE(writing);
			CHECK(server.TakeInProcessResponses(other).empty());
			server.DisconnectInProcess(other);
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(build, error));
		}

		// Docs/Decisions/0030-m19-contract.md decision 10 (A-DISTSELF): a testing export with featureTest runs the staged
		// game's --feature-test (by its fixed path, with --renderer none from an editor without a device) and reports the run
		// and every replay's observed state hash, sorted by path, the hashes the games suite's Dist half compares; the report
		// lies inside the exported package, and phaseTimings lists FeatureTest. featureTest needs testing.
		TEST_CASE("ExportMethods: a testing export with featureTest reports every replay's state hash from the exported game")
		{
			Test::EditorTestFixture fixture("ExportFeatureTest");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			MakeExportable(fixture.GetEditor());
			EditorContext& editor = fixture.GetEditor();
			const Test::TempDirectory binaries("ExportBinaries");
			Test::AutomationTestClient client(editor, MakeExportServerSpecification(MakeBinaryRoot(binaries)));
			REQUIRE(client.Call("scene.save", Json::object()).has_value());
			REQUIRE(client.Call("input.record", Json{ { "action", "start" } }).has_value());
			REQUIRE(client.Call("play.step", Json{ { "ticks", 30 }, { "render", "none" } }).has_value());
			const Result<Json> recorded = client.Call("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Idle.replay" } });
			REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
			REQUIRE(client.Call("play.stop", Json::object()).has_value());
			const Result<Json> patch = JsonReader::Parse(R"({"Testing":{"Replays":["Assets/Tests/*.replay"]}})");
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Testing Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());

			const Result<Json> refused = client.Call("project.export", Json{ { "config", "release" }, { "featureTest", true } });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);

			const Json response = RequestExportUntilAnswered(client, Json{ { "config", "release" }, { "testing", true }, { "featureTest", true } });
			REQUIRE_MESSAGE(response.contains("result"), response.dump());
			const Json& exported = response["result"];
			const Json& run = exported["featureTest"];
			CHECK(run["ran"] == Json(true));
			CHECK(run["exitCode"] == Json(0));
			CHECK(run["passed"] == Json(true));
			const std::string reportPath = JsonReader(run["reportPath"]).ReadString().value_or("");
			const std::string outputDirectory = JsonReader(exported["outputDirectory"]).ReadString().value_or("");
			CHECK(reportPath.starts_with(outputDirectory));
			CHECK(FileSystem::Exists(FileSystem::PathFromUtf8(reportPath)));
			REQUIRE(run["replays"].size() == 1);
			CHECK(run["replays"][0]["path"] == Json("Assets/Tests/Idle.replay"));
			CHECK(run["replays"][0]["stateHash"] == (*recorded)["stateHash"]);
			CHECK(run["replays"][0]["passed"] == Json(true));
			bool listed = false;
			for (const Json& timing : exported["phaseTimings"])
				listed = listed || timing["phase"] == Json("FeatureTest");
			CHECK(listed);
		}
	}

}
