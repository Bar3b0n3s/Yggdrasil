#include "TestsPCH.h"

#include "EditorCore/Automation/ProjectMethods.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"
#include "Support/Utf8Path.h"

namespace Engine {

	static Json ParseProjectJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ProjectMethods: Basic3D creates a playable starter scene through the typed method")
		{
			Test::TempDirectory directory("Basic3DMethod");
			REQUIRE(FileSystem::CreateDirectories(directory / "User").has_value());
			auto engine = EngineContext::Create({ .WorkerCount = 0, .UserDataDirectory = directory / "User", .EngineResourcesDirectory = Test::GetRepositoryRoot() / "Resources", .EngineCacheDirectory = directory / "EngineCache", .RegisterTypes = &RegisterEditorMethodTypes });
			REQUIRE(engine.has_value());
			auto editor = EditorContext::Create(**engine, { .IdGeneratorState = Test::EditorTestIdState, .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources/Templates/Projects", .ReadOnlyCacheRoot = directory / "ReadOnly" });
			REQUIRE(editor.has_value());
			Test::AutomationTestClient client(**editor);
			const auto root = directory / "Basic";
			const auto created = client.Call("project.create", Json{ { "path", Test::PathToUtf8(root) }, { "name", "Basic" }, { "template", "Basic3D" } });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			REQUIRE(client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }).has_value());
			const auto validated = client.Call("project.validate", Json{ { "scope", "scene" } });
			REQUIRE_MESSAGE(validated.has_value(), validated.error().ToString());
			INFO(validated->dump());
			CHECK((*validated)["errorCount"] == Json(0));
			const auto camera = client.Call("entity.get", Json{ { "entity", "/Camera" } });
			REQUIRE(camera.has_value());
			CHECK((*editor)->GetProject().GetSettings().StartScene == "Assets/Scenes/Main.scene");
		}

		TEST_CASE("ProjectMethods: project.create creates, opens and records the .eproj")
		{
			Test::EditorTestFixture fixture("ProjectCreateMethod");
			Test::AutomationTestClient client(fixture.GetEditor());
			const std::filesystem::path root = fixture.GetDirectory() / "Tetris";
			Result<Json> created = client.Call("project.create", Json{ { "path", Test::PathToUtf8(root) }, { "name", "Tetris" }, { "template", "empty" } });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK((*created)["project"]["name"] == Json("Tetris"));
			CHECK((*created)["project"]["readOnly"] == Json(false));
			CHECK((*created)["createdFiles"].dump().contains("Tetris.eproj"));
			CHECK(fixture.GetEditor().HasProject());
			const ProvenanceRecorder* provenance = fixture.GetEditor().GetProvenance();
			REQUIRE(provenance != nullptr);
			const ProvenanceEntry* entry = provenance->Find("Tetris.eproj");
			REQUIRE(entry != nullptr);
			CHECK(entry->Method == "project.create");
			CHECK(entry->Client == "test");

			const Result<Json> second = client.Call("project.create", Json{ { "path", Test::PathToUtf8(fixture.GetDirectory() / "Other") }, { "name", "Other" } });
			REQUIRE_FALSE(second.has_value());
			CHECK(second.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("ProjectMethods: project.open opens an existing project and reports load warnings")
		{
			Test::EditorTestFixture fixture("ProjectOpenMethod");
			fixture.CreateAndOpenProject("Game");
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			REQUIRE(fixture.GetEditor().CloseProject());
			Test::AutomationTestClient client(fixture.GetEditor());
			Result<Json> opened = client.Call("project.open", Json{ { "path", Test::PathToUtf8(projectFile.parent_path()) } });
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			CHECK((*opened)["project"]["name"] == Json("Game"));
			CHECK((*opened)["warnings"] == Json::array());
			CHECK(client.Call("project.open", Json{ { "path", Test::PathToUtf8(projectFile) } }).error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("ProjectMethods: project.open logs a load warning in the scene loads' format")
		{
			Test::EditorTestFixture fixture("ProjectOpenWarning");
			fixture.CreateAndOpenProject("Game");
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			REQUIRE(fixture.GetEditor().CloseProject());
			Json document = ParseProjectJson(FileSystem::ReadText(projectFile).value_or(std::string()));
			document["FutureSetting"] = 1;
			const std::string text = document.dump(1, '\t');
			REQUIRE(FileSystem::WriteFileAtomic(projectFile, std::as_bytes(std::span(text.data(), text.size()))).has_value());

			Test::AutomationTestClient client(fixture.GetEditor());
			// "'<file>' <pointer>: <message> (<code>)", like Utils::LogLoadDiagnostics for scene files.
			const Test::ExpectLog logged(LogLevel::Warn, std::format("'{}' /FutureSetting: ", FileSystem::PathToUtf8(projectFile)));
			Result<Json> opened = client.Call("project.open", Json{ { "path", Test::PathToUtf8(projectFile) } });
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			REQUIRE((*opened)["warnings"].size() == 1);
			CHECK((*opened)["warnings"][0].dump().contains("/FutureSetting"));
		}

		TEST_CASE("ProjectMethods: project.info, project.save and project.getSettings report the project")
		{
			Test::AutomationFixture setup("ProjectInfoMethod");
			Result<Json> info = setup.Call("project.info", Json::object());
			REQUIRE(info.has_value());
			CHECK((*info)["project"]["name"] == Json("TestProject"));
			CHECK((*info)["scene"]["open"] == Json(true));
			CHECK((*info)["scene"]["path"] == Json("Assets/Scenes/Main.scene"));
			CHECK((*info)["scene"]["dirty"] == Json(false));

			REQUIRE(setup.Call("entity.create", Json{ { "name", "Board" } }).has_value());
			Result<Json> saved = setup.Call("project.save", Json::object());
			REQUIRE(saved.has_value());
			CHECK((*saved)["savedFiles"] == ParseProjectJson(R"(["Assets/Scenes/Main.scene"])"));
			CHECK_FALSE(setup.GetEditor().IsSceneDirty());

			Result<Json> settings = setup.Call("project.getSettings", Json::object());
			REQUIRE(settings.has_value());
			CHECK((*settings)["settings"]["Name"] == Json("TestProject"));
			CHECK((*settings)["settings"]["Simulation"]["FixedHz"] == Json(60));
		}

		TEST_CASE("ProjectMethods: project.setSettings applies a merge patch as one undoable command")
		{
			Test::AutomationFixture setup("ProjectSetSettingsMethod");
			Result<Json> patched = setup.Call("project.setSettings",
				ParseProjectJson(R"({"patch":{"Window":{"Title":"Tetris"},"Input":{"Actions":{"Hold":{"Type":"button","Bindings":["Key.C"]}}}}})"));
			REQUIRE_MESSAGE(patched.has_value(), patched.error().ToString());
			CHECK((*patched)["settings"]["Window"]["Title"] == Json("Tetris"));
			CHECK((*patched)["settings"]["Input"]["Actions"]["Hold"]["Type"] == Json("Button")); // echoed canonically
			CHECK((*patched)["undoIndex"] != Json(0));
			CHECK(FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "TestProject.eproj").value_or("").contains("\"Tetris\""));

			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(setup.GetEditor().GetProject().GetSettings().Window.Title == "Game");

			const Result<Json> invalid = setup.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Simulation":{"FixedHz":0}}})"));
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("ProjectMethods: project.validate reports diagnostics and fixes only the selected ids")
		{
			Test::AutomationFixture setup("ProjectValidateMethod");
			REQUIRE(setup.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Export":{"BuildScenes":["Assets/Scenes/Gone.scene"]}}})")).has_value());
			REQUIRE(setup.Call("entity.create", ParseProjectJson(R"({"name":"A","components":{"Camera":{"Primary":true}}})")).has_value());
			REQUIRE(setup.Call("entity.create", ParseProjectJson(R"({"name":"B","components":{"Camera":{"Primary":true}}})")).has_value());

			Result<Json> report = setup.Call("project.validate", Json::object());
			REQUIRE(report.has_value());
			std::string missingId;
			for (Json& diagnostic : (*report)["diagnostics"])
			{
				if (diagnostic["code"] == Json("BUILD_SCENE_MISSING"))
					missingId = JsonReader(diagnostic["id"]).ReadString().value_or(std::string());
			}
			REQUIRE_FALSE(missingId.empty());
			Result<Json> fixed = setup.Call("project.validate", Json{ { "fix", Json::array({ missingId }) } });
			REQUIRE(fixed.has_value());
			CHECK((*fixed)["fixed"] == Json::array({ missingId }));
			CHECK((*fixed)["diagnostics"].dump().contains("SCENE_MULTIPLE_PRIMARY_CAMERAS"));
			CHECK_FALSE((*fixed)["diagnostics"].dump().contains("BUILD_SCENE_MISSING"));

			const Result<Json> malformed = setup.Call("project.validate", Json{ { "fix", 3 } });
			REQUIRE_FALSE(malformed.has_value());
			CHECK(malformed.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("ProjectMethods: project.upgrade rewrites a v0 scene and records provenance")
		{
			Test::AutomationFixture setup("ProjectUpgradeMethod", false);
			const Result<std::string> v0 = Test::ReadTestDataText("Formats/Scene/v0.scene");
			REQUIRE(v0.has_value());
			const std::filesystem::path file = setup.GetEditorFixture().GetProjectRoot() / "Assets/Scenes/Old.scene";
			REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(v0->data(), v0->size()))).has_value());

			Result<Json> dryRun = setup.Call("project.upgrade", Json{ { "dryRun", true } });
			REQUIRE(dryRun.has_value());
			CHECK((*dryRun)["changedFiles"] == ParseProjectJson(R"(["Assets/Scenes/Old.scene"])"));
			CHECK(FileSystem::ReadText(file).value_or("") == *v0);

			Result<Json> upgraded = setup.Call("project.upgrade", Json::object());
			REQUIRE(upgraded.has_value());
			CHECK((*upgraded)["changedFiles"] == ParseProjectJson(R"(["Assets/Scenes/Old.scene"])"));
			const Result<std::string> expected = Test::ReadTestDataText("Formats/Scene/v0.upgraded.scene");
			REQUIRE(expected.has_value());
			CHECK(FileSystem::ReadText(file).value_or("") == *expected);
			const ProvenanceRecorder* provenance = setup.GetEditor().GetProvenance();
			REQUIRE(provenance != nullptr);
			const ProvenanceEntry* entry = provenance->Find("Assets/Scenes/Old.scene");
			REQUIRE(entry != nullptr);
			CHECK(entry->Method == "project.upgrade");

			Result<Json> again = setup.Call("project.upgrade", Json::object());
			REQUIRE(again.has_value());
			CHECK((*again)["changedFiles"] == Json::array());
		}

		TEST_CASE("ProjectMethods: project.refreshAssets registers an externally written file")
		{
			Test::AutomationFixture setup("ProjectRefreshAssets", false);
			// Written outside the editor (a user's file manager): the registry learns about it only by a scan.
			const std::string material = R"({"Format": "Material", "Version": 1, "Roughness": 0.3})";
			REQUIRE(FileSystem::WriteFileAtomic(setup.GetEditor().GetProject().GetRoot() / "Assets/External.material", AsBytes(material)).has_value());
			Result<Json> refreshed = setup.Call("project.refreshAssets", Json::object());
			REQUIRE_MESSAGE(refreshed.has_value(), refreshed.error().ToString());
			CHECK((*refreshed)["createdMetas"] == Json::array({ "Assets/External.material.meta" }));
			CHECK((*refreshed)["added"].size() == 1);
			Result<Json> properties = setup.Call("asset.getProperties", Json{ { "asset", "Assets/External.material" } });
			REQUIRE(properties.has_value());
			CHECK((*properties)["values"]["Roughness"] == Json(0.3f));
			// Nothing new: a second refresh reports nothing added.
			Result<Json> again = setup.Call("project.refreshAssets", Json::object());
			REQUIRE(again.has_value());
			CHECK((*again)["added"].empty());
		}

		TEST_CASE("ProjectMethods: project.save writes only the scene because asset edits write through")
		{
			Test::AutomationFixture setup("ProjectSaveAssets");
			REQUIRE(setup.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Red.material" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Dirty" } }).has_value());
			Result<Json> saved = setup.Call("project.save", Json::object());
			REQUIRE(saved.has_value());
			CHECK((*saved)["savedFiles"] == Json::array({ "Assets/Scenes/Main.scene" }));
		}
	}

}
