#include "TestsPCH.h"

#include "EditorCore/Automation/ExportMethods.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/TempDirectory.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <utility>

// project.export in process (Architecture §13.5, §14.2): the param checks, the pending operation and its cancellation by a
// disconnect. The exports themselves are tested by EditorCore/Export/ExporterTests.cpp and Tests/Automation/test_export.py.

namespace Engine {

	namespace {

		void WriteTestFile(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, AsBytes(text), { .KeepBackup = false }).has_value());
		}

		// A bin directory with a Release Runtime the export copies (never runs), its CRT on Windows and one shader.
		std::filesystem::path MakeBinaryRoot(const Test::TempDirectory& directory)
		{
			const std::filesystem::path root = directory / "bin";
			const std::filesystem::path output = root / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release);
#if defined(ENGINE_PLATFORM_WINDOWS)
			WriteTestFile(output / "Runtime" / "Runtime.exe", "runtime");
			WriteTestFile(output / "Runtime" / "Redist" / "vcruntime140.dll", "crt");
#else
			WriteTestFile(output / "Runtime" / "Runtime", "runtime");
#endif
			WriteTestFile(output / "Shaders" / "Test" / "VSMain.spv", "spirv");
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

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ExportMethods: project.export refuses testing exports and output directories outside Build")
		{
			Test::AutomationFixture fixture("ExportMethods");
			const Json testing = fixture.Request("project.export", Json{ { "config", "release" }, { "testing", true } });
			CHECK(testing["error"]["code"] == Json(-32009));
			CHECK(testing["error"]["data"]["issues"][0]["pointer"] == Json("/testing"));

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
	}

}
