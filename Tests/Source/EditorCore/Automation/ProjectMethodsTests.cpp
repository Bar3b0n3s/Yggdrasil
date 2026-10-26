#include "TestsPCH.h"

#include "EditorCore/Automation/ProjectMethods.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/StrictnessEditor.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"
#include "Support/Utf8Path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	static Json ParseProjectJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	static VfsPath ProjectScenePath(std::string_view text)
	{
		Result<VfsPath> path = VfsPath::Parse(text);
		REQUIRE_MESSAGE(path.has_value(), path.error().ToString());
		return *path;
	}

	// Writes an empty scene named after `path` and opens it as an additional open scene (EditorContext::AddScene, M20).
	static OpenSceneId AddProjectOpenScene(EditorContext& editor, std::string_view path)
	{
		const VfsPath file = ProjectScenePath(path);
		Scope<Scene> scene = editor.CreateScene(std::string(file.GetStem()));
		const Result<std::string> text = SceneSerializer::SaveToString(*scene);
		REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
		REQUIRE(editor.WriteProjectFile(file, std::as_bytes(std::span(text->data(), text->size()))).has_value());
		const Result<OpenSceneId> added = editor.AddScene(std::move(scene), file);
		REQUIRE_MESSAGE(added.has_value(), added.error().ToString());
		return *added;
	}

	// Creates an entity named `name` in the addressed scene as one undoable edit; returns its id.
	static UUID CreateProjectEdit(EditorContext& editor, std::string name)
	{
		SceneEdit edit(editor, "Create " + name);
		const UUID id = editor.GetScene().CreateEntity(std::move(name)).GetUUID();
		REQUIRE(edit.Commit().has_value());
		return id;
	}

	// The warnings of a method result; null when it has none.
	static Json ReadWarnings(const Json& result)
	{
		return result.contains("warnings") ? result["warnings"] : Json();
	}

	// Whether one of `warnings` (a JSON array of strings) starts with `prefix` and contains `text`.
	static bool HasWarning(const Json& warnings, std::string_view prefix, std::string_view text)
	{
		return std::ranges::any_of(warnings, [prefix, text](const Json& warning)
		{
			const std::string line = JsonReader(warning).ReadString().value_or(std::string());
			return line.starts_with(prefix) && line.contains(text);
		});
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
			// The root .luaurc the template copies is engine-consumed and recorded too (ADR 0026 decision 13, G5).
			const ProvenanceEntry* luaurc = provenance->Find(".luaurc");
			REQUIRE(luaurc != nullptr);
			CHECK(luaurc->Method == "project.create");
			const Result<Buffer> luaurcBytes = FileSystem::ReadFile(root / ".luaurc");
			REQUIRE(luaurcBytes.has_value());
			CHECK(luaurc->Hash == XXH64(*luaurcBytes));

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

		// Docs/Decisions/0030-m19-contract.md "Integration of W19.2": a non-strict editor dropped a member the settings do not
		// have with a warning nothing read, so a mistyped key was lost without a word.
		TEST_CASE("ProjectMethods: project.setSettings drops a member the settings do not have and names it in its warnings")
		{
			Test::AutomationFixture setup("ProjectSetSettingsUnknown");
			const Result<Json> patched = setup.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Window":{"Titel":"Tetris","Width":800},"Bogus":true}})"));
			REQUIRE_MESSAGE(patched.has_value(), patched.error().ToString());
			CHECK((*patched)["settings"]["Window"]["Width"] == Json(800));
			CHECK((*patched)["settings"]["Window"]["Title"] == Json("Game"));
			const Json warnings = ReadWarnings(*patched);
			REQUIRE(warnings.is_array());
			CHECK(warnings.size() == 2);
			CHECK(HasWarning(warnings, "/patch/Window/Titel: ", "'Title'"));
			CHECK(HasWarning(warnings, "/patch/Bogus: ", "Bogus"));
			const std::string written = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "TestProject.eproj").value_or("");
			CHECK(written.contains("800"));
			CHECK_FALSE(written.contains("Titel"));
			CHECK_FALSE(written.contains("Bogus"));

			// A dry run names them too; a patch of known members has none.
			const Result<Json> dryRun = setup.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Physics":{"Gravty":[0,-1,0]}},"dryRun":true})"));
			REQUIRE_MESSAGE(dryRun.has_value(), dryRun.error().ToString());
			CHECK(HasWarning(ReadWarnings(*dryRun), "/patch/Physics/Gravty: ", "'Gravity'"));
			const Result<Json> known = setup.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Window":{"Title":"Tetris"}}})"));
			REQUIRE_MESSAGE(known.has_value(), known.error().ToString());
			CHECK(ReadWarnings(*known) == Json::array());
		}

		TEST_CASE("ProjectMethods: a --strict editor's project.setSettings refuses a member the settings do not have")
		{
			Test::TempDirectory directory("ProjectSetSettingsStrict");
			Test::StrictnessEditor strict(directory.GetPath(), true);
			strict.CreateAndOpenProject("StrictProject");
			Test::AutomationTestClient client(strict.GetEditor());
			const uint32_t width = strict.GetEditor().GetProject().GetSettings().Window.Width;
			REQUIRE(width != 800);
			const Result<Json> refused = client.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Window":{"Titel":"Tetris","Width":800}}})"));
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			CHECK(std::ranges::any_of(refused.error().GetIssues(), [](const ErrorIssue& issue)
			{
				return issue.JsonPointer == "/patch/Window/Titel";
			}));
			CHECK(strict.GetEditor().GetProject().GetSettings().Window.Width == width);

			const Result<Json> known = client.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Window":{"Width":800}}})"));
			REQUIRE_MESSAGE(known.has_value(), known.error().ToString());
			CHECK(ReadWarnings(*known) == Json::array());
			CHECK(strict.GetEditor().GetProject().GetSettings().Window.Width == 800);
		}

		// A null member resets a field, and the merge erased one that names no field before the read saw it, so a mistyped
		// reset was dropped without a warning, and a --strict editor accepted it.
		TEST_CASE("ProjectMethods: project.setSettings names a null member the settings do not have and a --strict editor refuses it")
		{
			{
				Test::AutomationFixture setup("ProjectSetSettingsUnknownNull");
				const Result<Json> patched =
					setup.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Window":{"Titel":null},"Input":{"Actions":{"Jump":{"Bindings":["Key.C"],"Bindngs":null}}},"Bogus":null}})"));
				REQUIRE_MESSAGE(patched.has_value(), patched.error().ToString());
				CHECK((*patched)["settings"]["Window"]["Title"] == Json("Game"));
				CHECK((*patched)["settings"]["Input"]["Actions"]["Jump"]["Bindings"] == Json::array({ "Key.C" }));
				const Json warnings = ReadWarnings(*patched);
				REQUIRE(warnings.is_array());
				CHECK(warnings.size() == 3);
				CHECK(HasWarning(warnings, "/patch/Window/Titel: ", "'Title'"));
				CHECK(HasWarning(warnings, "/patch/Input/Actions/Jump/Bindngs: ", "'Bindings'"));
				CHECK(HasWarning(warnings, "/patch/Bogus: ", "Bogus"));
				const std::string written = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "TestProject.eproj").value_or("");
				CHECK_FALSE(written.contains("Titel"));
				CHECK_FALSE(written.contains("Bindngs"));
				CHECK_FALSE(written.contains("Bogus"));

				// A null member that names a field still resets it, and one that names an action deletes it, without a warning.
				const Result<Json> reset = setup.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Window":{"Title":null},"Input":{"Actions":{"Jump":null}}}})"));
				REQUIRE_MESSAGE(reset.has_value(), reset.error().ToString());
				CHECK(ReadWarnings(*reset) == Json::array());
				CHECK_FALSE((*reset)["settings"]["Input"]["Actions"].contains("Jump"));
			}

			Test::TempDirectory directory("ProjectSetSettingsStrictNull");
			Test::StrictnessEditor strict(directory.GetPath(), true);
			strict.CreateAndOpenProject("StrictProject");
			Test::AutomationTestClient client(strict.GetEditor());
			const std::string title = strict.GetEditor().GetProject().GetSettings().Window.Title;
			const Result<Json> refused = client.Call("project.setSettings", ParseProjectJson(R"({"patch":{"Window":{"Titel":null,"Title":"Tetris"}}})"));
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			CHECK(std::ranges::any_of(refused.error().GetIssues(), [](const ErrorIssue& issue)
			{
				return issue.JsonPointer == "/patch/Window/Titel";
			}));
			CHECK(strict.GetEditor().GetProject().GetSettings().Window.Title == title);
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

		TEST_CASE("ProjectMethods: project.upgrade rewrites .meta, .material, .sfx and .replay files canonically and records them")
		{
			Test::AutomationFixture setup("ProjectUpgradeAssetFormats", false);
			REQUIRE(setup.Call("asset.create", ParseProjectJson(R"({"type": "material", "path": "Assets/Materials/Red.material", "values": {"Roughness": 0.45}})"))
					.has_value());
			REQUIRE(setup.Call("asset.create", ParseProjectJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Blip.sfx",
				"values": {"Seed": 3, "Layers": [{"Wave": "Square", "Notes": ["A3:0.05"]}]}})"))
					.has_value());
			const Result<std::string> replay = Test::ReadTestDataText("SettingsObservation/Project/Assets/Tests/Replays/Main.replay");
			REQUIRE(replay.has_value());
			const std::filesystem::path root = setup.GetEditor().GetProject().GetRoot();
			REQUIRE(FileSystem::CreateDirectories(root / "Assets/Replays").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(root / "Assets/Replays/Main.replay", AsBytes(*replay), { .KeepBackup = false }).has_value());

			// Each file as another writer leaves it: the same document, minified, so only a canonical rewrite restores the bytes
			// the editor wrote (§6; Docs/Decisions/0026-m16-contract.md decision 12).
			const std::string_view files[] = { "Assets/Audio/Blip.sfx", "Assets/Audio/Blip.sfx.meta", "Assets/Materials/Red.material",
				"Assets/Materials/Red.material.meta", "Assets/Replays/Main.replay" };
			std::vector<std::string> canonical;
			for (const std::string_view file : files)
			{
				const Result<std::string> text = FileSystem::ReadText(root / file);
				REQUIRE(text.has_value());
				canonical.push_back(*text);
				const Json document = ParseProjectJson(*text);
				const std::string minified = document.dump();
				REQUIRE(FileSystem::WriteFileAtomic(root / file, AsBytes(minified), { .KeepBackup = false }).has_value());
			}

			Result<Json> upgraded = setup.Call("project.upgrade", Json::object());
			REQUIRE_MESSAGE(upgraded.has_value(), upgraded.error().ToString());
			CHECK((*upgraded)["changedFiles"] == ParseProjectJson(R"(["Assets/Audio/Blip.sfx", "Assets/Audio/Blip.sfx.meta", "Assets/Materials/Red.material",
				"Assets/Materials/Red.material.meta", "Assets/Replays/Main.replay"])"));
			const ProvenanceRecorder* provenance = setup.GetEditor().GetProvenance();
			REQUIRE(provenance != nullptr);
			for (size_t index = 0; index < std::size(files); ++index)
			{
				CAPTURE(std::string(files[index]));
				CHECK(FileSystem::ReadText(root / files[index]).value_or("") == canonical[index]);
				const ProvenanceEntry* entry = provenance->Find(files[index]);
				REQUIRE(entry != nullptr);
				CHECK(entry->Method == "project.upgrade");
			}

			Result<Json> again = setup.Call("project.upgrade", Json::object());
			REQUIRE(again.has_value());
			CHECK((*again)["changedFiles"] == Json::array());
		}

		// M16 FX-4: Replay version 2 adds the state-hash checkpoints, version 3 the start fingerprint (Docs/Decisions/
		// 0030-m19-contract.md decision 2). project.upgrade rewrites a version 1 replay in the current version with no
		// checkpoints and a null fingerprint (the bytes of Tests/Data/Formats/Replay/v1.upgraded.replay), records it, and then
		// leaves it alone.
		TEST_CASE("ProjectMethods: project.upgrade migrates a version 1 replay to version 3 and records it")
		{
			Test::AutomationFixture setup("ProjectUpgradeReplay", false);
			const Result<std::string> v1 = Test::ReadTestDataText("Formats/Replay/v1.replay");
			const Result<std::string> expected = Test::ReadTestDataText("Formats/Replay/v1.upgraded.replay");
			REQUIRE(v1.has_value());
			REQUIRE(expected.has_value());
			const std::filesystem::path root = setup.GetEditor().GetProject().GetRoot();
			REQUIRE(FileSystem::CreateDirectories(root / "Assets/Replays").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(root / "Assets/Replays/Old.replay", AsBytes(*v1), { .KeepBackup = false }).has_value());

			Result<Json> dryRun = setup.Call("project.upgrade", Json{ { "dryRun", true } });
			REQUIRE_MESSAGE(dryRun.has_value(), dryRun.error().ToString());
			CHECK((*dryRun)["changedFiles"] == ParseProjectJson(R"(["Assets/Replays/Old.replay"])"));
			CHECK(FileSystem::ReadText(root / "Assets/Replays/Old.replay").value_or("") == *v1);

			Result<Json> upgraded = setup.Call("project.upgrade", Json::object());
			REQUIRE_MESSAGE(upgraded.has_value(), upgraded.error().ToString());
			CHECK((*upgraded)["changedFiles"] == ParseProjectJson(R"(["Assets/Replays/Old.replay"])"));
			CHECK(FileSystem::ReadText(root / "Assets/Replays/Old.replay").value_or("") == *expected);
			const ProvenanceRecorder* provenance = setup.GetEditor().GetProvenance();
			REQUIRE(provenance != nullptr);
			const ProvenanceEntry* entry = provenance->Find("Assets/Replays/Old.replay");
			REQUIRE(entry != nullptr);
			CHECK(entry->Method == "project.upgrade");

			Result<Json> again = setup.Call("project.upgrade", Json::object());
			REQUIRE(again.has_value());
			CHECK((*again)["changedFiles"] == Json::array());
		}

		// M16 friction S3-09: project.upgrade listed the files it rewrote without saying why. changes names each file with
		// the reason its canonical form differs: an older format version, or only the canonical form.
		TEST_CASE("ProjectMethods: project.upgrade names the reason for each file it rewrites")
		{
			Test::AutomationFixture setup("ProjectUpgradeReasons", false);
			const Result<std::string> scene = Test::ReadTestDataText("Formats/Scene/v0.scene");
			const Result<std::string> replay = Test::ReadTestDataText("Formats/Replay/v1.replay");
			REQUIRE(scene.has_value());
			REQUIRE(replay.has_value());
			REQUIRE(setup.Call("asset.create", ParseProjectJson(R"({"type": "material", "path": "Assets/Materials/Red.material", "values": {"Roughness": 0.45}})"))
					.has_value());
			const std::filesystem::path root = setup.GetEditor().GetProject().GetRoot();
			REQUIRE(FileSystem::CreateDirectories(root / "Assets/Replays").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(root / "Assets/Replays/Old.replay", AsBytes(*replay), { .KeepBackup = false }).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(root / "Assets/Scenes/Old.scene", AsBytes(*scene), { .KeepBackup = false }).has_value());
			// The material as another writer leaves it: the same document, minified.
			const Result<std::string> material = FileSystem::ReadText(root / "Assets/Materials/Red.material");
			REQUIRE(material.has_value());
			REQUIRE(FileSystem::WriteFileAtomic(root / "Assets/Materials/Red.material", AsBytes(ParseProjectJson(*material).dump()), { .KeepBackup = false })
					.has_value());

			const Json expected = ParseProjectJson(R"([
				{"path": "Assets/Materials/Red.material", "reason": "canonical form"},
				{"path": "Assets/Replays/Old.replay", "reason": "format version 1 to 3"},
				{"path": "Assets/Scenes/Old.scene", "reason": "format version 0 to 1"}])");
			Result<Json> dryRun = setup.Call("project.upgrade", Json{ { "dryRun", true } });
			REQUIRE_MESSAGE(dryRun.has_value(), dryRun.error().ToString());
			CHECK((*dryRun)["changes"] == expected);
			Result<Json> upgraded = setup.Call("project.upgrade", Json::object());
			REQUIRE_MESSAGE(upgraded.has_value(), upgraded.error().ToString());
			CHECK((*upgraded)["changes"] == expected);
			CHECK((*upgraded)["changedFiles"] == ParseProjectJson(R"(["Assets/Materials/Red.material", "Assets/Replays/Old.replay", "Assets/Scenes/Old.scene"])"));
			Result<Json> again = setup.Call("project.upgrade", Json::object());
			REQUIRE(again.has_value());
			CHECK((*again)["changes"] == Json::array());
		}

		// M16 review R16: the canonical writers drop a member their reader does not know, and the importers refuse such a file.
		// project.upgrade reads materials, sound effects and replays as their importers do, so the member fails the upgrade at
		// its file and pointer instead of being dropped silently by a rewrite that would make the file importable.
		TEST_CASE("ProjectMethods: project.upgrade fails at an unknown member of a material, sound effect or replay instead of dropping it")
		{
			Test::AutomationFixture setup("ProjectUpgradeUnknownMembers", false);
			REQUIRE(setup.Call("asset.create", ParseProjectJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Blip.sfx",
				"values": {"Seed": 3, "Layers": [{"Wave": "Square", "Notes": ["A3:0.05"]}]}})"))
					.has_value());
			const std::filesystem::path root = setup.GetEditor().GetProject().GetRoot();
			const Result<std::string> sound = FileSystem::ReadText(root / "Assets/Audio/Blip.sfx");
			const Result<std::string> replay = Test::ReadTestDataText("SettingsObservation/Project/Assets/Tests/Replays/Main.replay");
			REQUIRE(sound.has_value());
			REQUIRE(replay.has_value());
			REQUIRE(FileSystem::Remove(root / "Assets/Audio/Blip.sfx").has_value());
			REQUIRE(FileSystem::CreateDirectories(root / "Assets/Replays").has_value());

			struct UnknownMemberCase
			{
				std::string_view File;
				std::string Text;
				std::string_view Member;
			};
			Json typoSound = ParseProjectJson(*sound);
			typoSound["Volumme"] = 0.5;
			Json typoReplay = ParseProjectJson(*replay);
			typoReplay["Expcet"] = Json::array();
			const UnknownMemberCase cases[] = {
				{ .File = "Assets/Materials/Typo.material", .Text = R"({"Format": "Material", "Version": 1, "Roughnes": 0.3})", .Member = "Roughnes" },
				{ .File = "Assets/Audio/Typo.sfx", .Text = typoSound.dump(1, '\t'), .Member = "Volumme" },
				{ .File = "Assets/Replays/Typo.replay", .Text = typoReplay.dump(1, '\t'), .Member = "Expcet" },
			};
			for (const UnknownMemberCase& item : cases)
			{
				CAPTURE(std::string(item.File));
				const std::filesystem::path file = root / item.File;
				REQUIRE(FileSystem::CreateDirectories(file.parent_path()).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(item.Text), { .KeepBackup = false }).has_value());
				for (const bool dryRun : { true, false })
				{
					CAPTURE(dryRun);
					const Json response = setup.Request("project.upgrade", dryRun ? Json{ { "dryRun", true } } : Json::object());
					REQUIRE_MESSAGE(response.contains("error"), response.dump());
					const Json& data = response["error"]["data"];
					CAPTURE(data.dump());
					CHECK(data["errorCode"] == Json("Validation"));
					CHECK(data["location"]["file"] == Json(std::string(item.File)));
					CHECK(data["location"]["pointer"] == Json(std::format("/{}", item.Member)));
					CHECK(data["contexts"].dump().contains(std::format("while upgrading '{}'", item.File)));
				}
				// The file keeps its member and its bytes, and nothing records it.
				CHECK(FileSystem::ReadText(file).value_or(std::string()) == item.Text);
				REQUIRE(setup.GetEditor().GetProvenance() != nullptr);
				CHECK(setup.GetEditor().GetProvenance()->Find(item.File) == nullptr);
				REQUIRE(FileSystem::Remove(file).has_value());
			}
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

		// Docs/Decisions/0031-m20-contract.md decision 14 (E-SCENES): the methods that act on every open scene.

		// Decision 14: project.save and session.shutdown {save} write every dirty open scene, in tab order, whichever tab is
		// active, and savedFiles lists them in that order.
		TEST_CASE("ProjectMethods: project.save and session.shutdown save write every dirty open scene in tab order")
		{
			Test::AutomationFixture setup("OpenScenesSaveAll");
			EditorContext& editor = setup.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(main != NoOpenScene);
			CreateProjectEdit(editor, "InMain");
			const OpenSceneId second = AddProjectOpenScene(editor, "project://Assets/Scenes/Second.scene");
			CreateProjectEdit(editor, "InSecond");
			REQUIRE(editor.ActivateScene(main).has_value());

			const Json inTabOrder = Json::array({ "Assets/Scenes/Main.scene", "Assets/Scenes/Second.scene" });
			const Result<Json> saved = setup.Call("project.save", Json::object());
			REQUIRE_MESSAGE(saved.has_value(), saved.error().ToString());
			CHECK((*saved)["savedFiles"] == inTabOrder);
			CHECK_FALSE(editor.IsAnySceneDirty());
			const std::filesystem::path root = setup.GetEditorFixture().GetProjectRoot();
			CHECK(FileSystem::ReadText(root / "Assets/Scenes/Main.scene").value_or(std::string()).contains("InMain"));
			CHECK(FileSystem::ReadText(root / "Assets/Scenes/Second.scene").value_or(std::string()).contains("InSecond"));

			{
				Result<Scope<EditorSceneTargetScope>> scope = EditorSceneTargetScope::Begin(editor, second);
				REQUIRE(scope.has_value());
				CreateProjectEdit(editor, "LaterInSecond");
			}
			CreateProjectEdit(editor, "LaterInMain");
			const Result<Json> shutdown = setup.Call("session.shutdown", Json{ { "save", true } });
			REQUIRE_MESSAGE(shutdown.has_value(), shutdown.error().ToString());
			CHECK((*shutdown)["saved"] == Json(true));
			CHECK((*shutdown)["savedFiles"] == inTabOrder);
			CHECK_FALSE(editor.IsAnySceneDirty());
			CHECK(editor.GetShutdownRequest() == std::optional<int>(0));
		}

		// Decision 14: a dirty open scene refuses what would lose or ignore its changes even when it is not the active one
		// (project.upgrade, test.run of a suite that plays it, session.shutdown without save or force), and project.save
		// refuses a dirty scene that was never saved before it writes anything.
		TEST_CASE("ProjectMethods: a dirty open scene that is not active refuses project.upgrade, test.run and a shutdown without save")
		{
			Test::AutomationFixture setup("OpenScenesRefusals");
			EditorContext& editor = setup.GetEditor();
			const Json suite{ { "Script", "Assets/Tests/Bound.test.luau" }, { "Scene", "Assets/Scenes/Main.scene" }, { "Modes", Json::array({ "Editor" }) } };
			REQUIRE(setup.Call("script.write", Json{ { "path", "Assets/Tests/Bound.test.luau" }, { "source", "return Test.Suite('Bound', function()\nTest.Case('runs', function() Test.Expect(true) end)\nend)" } }));
			REQUIRE(setup.Call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ suite }) } } } } } }));
			const OpenSceneId main = editor.GetActiveSceneId();
			CreateProjectEdit(editor, "InMain");
			AddProjectOpenScene(editor, "project://Assets/Scenes/Second.scene");
			REQUIRE_FALSE(editor.IsSceneDirty());
			REQUIRE(editor.IsAnySceneDirty());

			const auto refusal = [&setup](std::string_view method, const Json& params)
			{
				const Json response = setup.Request(method, params);
				REQUIRE_MESSAGE(response.contains("error"), response.dump());
				CHECK(response["error"]["data"]["errorCode"] == Json("InvalidState"));
				return JsonReader(response["error"]["data"]).ReadMember<std::string>("detail").value_or(std::string());
			};
			CHECK(refusal("project.upgrade", Json::object()).contains("'Main'"));
			CHECK(refusal("session.shutdown", Json::object()).contains("'Main'"));
			CHECK(refusal("test.run", Json::object()).contains("'Assets/Scenes/Main.scene' is open with unsaved changes"));
			CHECK_FALSE(editor.GetShutdownRequest().has_value());

			// A dirty scene never saved refuses project.save before any scene is written.
			const Result<OpenSceneId> untitled = editor.AddScene(editor.CreateScene("Untitled"), std::nullopt);
			REQUIRE(untitled.has_value());
			CreateProjectEdit(editor, "Unsaved");
			const std::filesystem::path mainFile = setup.GetEditorFixture().GetProjectRoot() / "Assets/Scenes/Main.scene";
			const Result<std::string> mainBefore = FileSystem::ReadText(mainFile);
			REQUIRE(mainBefore.has_value());
			CHECK(refusal("project.save", Json::object()).contains("'Untitled'"));
			CHECK(FileSystem::ReadText(mainFile).value_or(std::string()) == *mainBefore);
			CHECK(editor.DescribeOpenScene(main)->Dirty);
		}

		// Decision 14: project.upgrade reloads every open scene whose file it rewrote, each in its tab position, and the scene
		// active before stays active.
		TEST_CASE("ProjectMethods: project.upgrade reloads every open scene whose file it rewrote and keeps the active scene")
		{
			Test::AutomationFixture setup("OpenScenesUpgradeReload");
			EditorContext& editor = setup.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			const OpenSceneId second = AddProjectOpenScene(editor, "project://Assets/Scenes/Second.scene");
			REQUIRE(editor.ActivateScene(main).has_value());
			// Another program writes both files out of their canonical form.
			const std::filesystem::path root = setup.GetEditorFixture().GetProjectRoot();
			for (const std::string_view file : { "Assets/Scenes/Main.scene", "Assets/Scenes/Second.scene" })
			{
				const Result<std::string> text = FileSystem::ReadText(root / file);
				REQUIRE(text.has_value());
				const Result<Json> document = JsonReader::Parse(*text);
				REQUIRE(document.has_value());
				const std::string reformatted = document->dump(4);
				REQUIRE(FileSystem::WriteFileAtomic(root / file, AsBytes(reformatted), {}).has_value());
			}

			const Result<Json> upgraded = setup.Call("project.upgrade", Json::object());
			REQUIRE_MESSAGE(upgraded.has_value(), upgraded.error().ToString());
			const Json& changed = (*upgraded)["changedFiles"];
			CHECK(std::find(changed.begin(), changed.end(), Json("Assets/Scenes/Main.scene")) != changed.end());
			CHECK(std::find(changed.begin(), changed.end(), Json("Assets/Scenes/Second.scene")) != changed.end());
			// Both were read again (a scene read again gets a new id) in their places, and Main is still active.
			const std::vector<OpenSceneId> open = editor.GetOpenScenes();
			REQUIRE(open.size() == 2);
			CHECK(open[0] != main);
			CHECK(open[1] != second);
			CHECK(editor.GetActiveSceneId() == open[0]);
			CHECK(editor.GetScene().GetName() == "Main");
			CHECK(editor.DescribeOpenScene(open[1])->Path == std::optional<VfsPath>(ProjectScenePath("project://Assets/Scenes/Second.scene")));
			CHECK_FALSE(editor.IsAnySceneDirty());
		}
	}

}
