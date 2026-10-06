#include "TestsPCH.h"

#include "EditorCore/Project/ProjectValidator.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Json ParseValidatorJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	// Two primary cameras in the open scene, and a build scene that does not exist.
	static void MakeTwoProblems(EditorContext& editor)
	{
		{
			SceneEdit edit(editor, "Cameras");
			for (const std::string_view name : { "CameraA", "CameraB" })
			{
				const Entity camera = editor.GetScene().CreateEntity(name);
				const Json primary = ParseValidatorJson(R"({"Primary":true})");
				REQUIRE(ComponentAccess::AddComponent(camera, "Camera", &primary).has_value());
			}
			REQUIRE(edit.Commit().has_value());
		}
		Result<Scope<ProjectSettingsCommand>> settings = ProjectSettingsCommand::CreateFromPatch(editor,
			ParseValidatorJson(R"({"Export":{"BuildScenes":["Assets/Scenes/Main.scene","Assets/Scenes/Gone.scene"]}})"), "Build scenes");
		REQUIRE(settings.has_value());
		REQUIRE(editor.Execute(std::move(*settings)).has_value());
	}

	static const ProjectDiagnostic* FindDiagnostic(const ValidationReport& report, std::string_view code)
	{
		for (const ProjectDiagnostic& diagnostic : report.Diagnostics)
		{
			if (diagnostic.Code == code)
				return &diagnostic;
		}
		return nullptr;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ProjectValidator: multiple primary cameras are reported and fixed keeping the first" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ValidatorCameras");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			MakeTwoProblems(fixture.GetEditor());

			const Result<ValidationReport> report = ProjectValidator::Validate(fixture.GetEditor(), ValidationScope::Scene);
			REQUIRE(report.has_value());
			const ProjectDiagnostic* cameras = FindDiagnostic(*report, SceneMultiplePrimaryCamerasCode);
			REQUIRE(cameras != nullptr);
			CHECK(cameras->Severity == DiagnosticSeverity::Error);
			CHECK(cameras->AutoFixable);
			CHECK(cameras->File == "Assets/Scenes/Main.scene");
			CHECK(FindDiagnostic(*report, BuildSceneMissingCode) == nullptr); // scope "scene" leaves the settings alone

			const Result<FixReport> fixed = ProjectValidator::Fix(fixture.GetEditor(), ValidationScope::Scene, FixSelection{ .All = true, .IdsOrCodes = {} });
			REQUIRE(fixed.has_value());
			CHECK(fixed->Fixed == std::vector<std::string>{ cameras->Id });
			CHECK(FindDiagnostic(fixed->After, SceneMultiplePrimaryCamerasCode) == nullptr);
			const Result<Json> first = ComponentAccess::GetComponentJson(fixture.GetEditor().GetScene().FindEntityByPath("/CameraA"), "Camera");
			REQUIRE(first.has_value());
			CHECK((*first)["Primary"] == Json(true));
		}

		TEST_CASE("ProjectValidator: diagnostic ids are stable and depend only on the code, location and subject" * doctest::skip(true))
		{
			const std::string id = ProjectValidator::MakeDiagnosticId(SceneMultiplePrimaryCamerasCode, "Assets/Scenes/Main.scene", "", "Camera", "Primary");
			CHECK(id.starts_with("SCENE_MULTIPLE_PRIMARY_CAMERAS-"));
			CHECK(id.size() == std::string_view("SCENE_MULTIPLE_PRIMARY_CAMERAS-").size() + 12);
			CHECK(id == ProjectValidator::MakeDiagnosticId(SceneMultiplePrimaryCamerasCode, "Assets/Scenes/Main.scene", "", "Camera", "Primary"));
			CHECK(id != ProjectValidator::MakeDiagnosticId(SceneMultiplePrimaryCamerasCode, "Assets/Scenes/Other.scene", "", "Camera", "Primary"));
			CHECK(id != ProjectValidator::MakeDiagnosticId(SceneMultiplePrimaryCamerasCode, "Assets/Scenes/Main.scene", "", "Camera", "Primary", "x"));

			Test::EditorTestFixture fixture("ValidatorIds");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			MakeTwoProblems(fixture.GetEditor());
			const Result<ValidationReport> first = ProjectValidator::Validate(fixture.GetEditor(), ValidationScope::Project);
			const Result<ValidationReport> second = ProjectValidator::Validate(fixture.GetEditor(), ValidationScope::Project);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			REQUIRE(first->Diagnostics.size() == second->Diagnostics.size());
			for (size_t index = 0; index < first->Diagnostics.size(); ++index)
				CHECK(first->Diagnostics[index].Id == second->Diagnostics[index].Id);
		}

		TEST_CASE("ProjectValidator: two missing build scenes have distinct ids, and fixing one keeps the other's id" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ValidatorTwoMissing");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Result<Scope<ProjectSettingsCommand>> settings = ProjectSettingsCommand::CreateFromPatch(editor,
				ParseValidatorJson(R"({"Export":{"BuildScenes":["Assets/Scenes/Main.scene","Assets/Scenes/GoneA.scene","Assets/Scenes/GoneB.scene"]}})"),
				"Build scenes");
			REQUIRE(settings.has_value());
			REQUIRE(editor.Execute(std::move(*settings)).has_value());

			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(report.has_value());
			std::vector<std::string> missing;
			for (const ProjectDiagnostic& diagnostic : report->Diagnostics)
			{
				if (diagnostic.Code == BuildSceneMissingCode)
					missing.push_back(diagnostic.Id);
			}
			REQUIRE(missing.size() == 2);
			CHECK(missing[0] != missing[1]);

			// Fixing the first removes its entry, which moves the second one up the list: its id must not change.
			const Result<FixReport> fixed = ProjectValidator::Fix(editor, ValidationScope::Project, FixSelection{ .All = false, .IdsOrCodes = { missing[0] } });
			REQUIRE(fixed.has_value());
			CHECK(fixed->Fixed == std::vector<std::string>{ missing[0] });
			const ProjectDiagnostic* remaining = FindDiagnostic(fixed->After, BuildSceneMissingCode);
			REQUIRE(remaining != nullptr);
			CHECK(remaining->Id == missing[1]);
		}

		TEST_CASE("ProjectValidator: fixes inside an open transaction join it" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ValidatorJoin");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			MakeTwoProblems(editor);
			const size_t undoCount = editor.GetHistory().GetUndoCount();
			EditorTransaction batch(editor, "Batch"); // an op project.validate {fix: true} of edit.batch
			const Result<FixReport> fixed = ProjectValidator::Fix(editor, ValidationScope::Project, FixSelection{ .All = true, .IdsOrCodes = {} });
			REQUIRE(fixed.has_value());
			CHECK_FALSE(fixed->Fixed.empty());
			CHECK(fixed->UndoIndex == 0);
			CHECK(batch.GetCommandCount() >= 1);
			CHECK(batch.Commit() != 0);
			CHECK(editor.GetHistory().GetUndoCount() == undoCount + 1);
		}

		TEST_CASE("ProjectValidator: fixing selected ids leaves the other diagnostics, in one undoable command" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ValidatorSelected");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			MakeTwoProblems(editor);
			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(report.has_value());
			const ProjectDiagnostic* missing = FindDiagnostic(*report, BuildSceneMissingCode);
			REQUIRE(missing != nullptr);
			CHECK(missing->AutoFixable);

			const size_t undoCount = editor.GetHistory().GetUndoCount();
			const Result<FixReport> fixed =
				ProjectValidator::Fix(editor, ValidationScope::Project, FixSelection{ .All = false, .IdsOrCodes = { missing->Id } });
			REQUIRE(fixed.has_value());
			CHECK(fixed->Fixed == std::vector<std::string>{ missing->Id });
			CHECK(FindDiagnostic(fixed->After, BuildSceneMissingCode) == nullptr);
			CHECK(FindDiagnostic(fixed->After, SceneMultiplePrimaryCamerasCode) != nullptr);
			CHECK(editor.GetProject().GetSettings().Export.BuildScenes == std::vector<std::string>{ "Assets/Scenes/Main.scene" });
			CHECK(editor.GetHistory().GetUndoCount() == undoCount + 1);
			CHECK(fixed->UndoIndex == editor.GetHistory().GetCurrentSequence());

			CHECK(editor.GetHistory().Undo(editor) == 1u);
			CHECK(editor.GetProject().GetSettings().Export.BuildScenes.size() == 2);

			const Result<FixReport> unknown =
				ProjectValidator::Fix(editor, ValidationScope::Project, FixSelection{ .All = false, .IdsOrCodes = { "NOT_A_CODE" } });
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("ProjectValidator: structural defects of scene files are reported under the validator codes" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ValidatorFiles");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const std::array<std::pair<std::string_view, std::string_view>, 3> fixtures = { {
				{ "Scenes/Invalid/DuplicateIds.scene", EntityDuplicateIdCode },
				{ "Scenes/Invalid/ParentCycle.scene", SceneInvalidHierarchyCode },
				{ "Scenes/Invalid/DuplicateUniqueComponent.scene", SceneDuplicateUniqueComponentCode },
			} };
			for (const auto& [file, code] : fixtures)
			{
				const Result<std::string> text = Test::ReadTestDataText(file);
				REQUIRE(text.has_value());
				const Result<VfsPath> path = VfsPath::Create("project", std::format("Assets/Scenes/{}", std::filesystem::path(file).filename().string()));
				REQUIRE(path.has_value());
				REQUIRE(editor.WriteProjectFile(*path, std::as_bytes(std::span(text->data(), text->size()))).has_value());
			}
			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(report.has_value());
			for (const auto& [file, code] : fixtures)
			{
				INFO(std::string(file));
				const ProjectDiagnostic* diagnostic = FindDiagnostic(*report, code);
				REQUIRE(diagnostic != nullptr);
				CHECK_FALSE(diagnostic->AutoFixable);
				CHECK(diagnostic->Hint.contains("repair"));
			}
		}

		TEST_CASE("ProjectValidator: a missing start scene is an error that cannot be fixed" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ValidatorStartScene");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			Result<Scope<ProjectSettingsCommand>> settings =
				ProjectSettingsCommand::CreateFromPatch(editor, ParseValidatorJson(R"({"StartScene":"Assets/Scenes/Main.scene"})"), "Start");
			REQUIRE(settings.has_value());
			REQUIRE(editor.Execute(std::move(*settings)).has_value());
			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(report.has_value());
			const ProjectDiagnostic* start = FindDiagnostic(*report, BuildStartSceneMissingCode);
			REQUIRE(start != nullptr);
			CHECK(start->Severity == DiagnosticSeverity::Error);
			CHECK_FALSE(start->AutoFixable);
			CHECK(report->ErrorCount >= 1);

			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			const Result<ValidationReport> after = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(after.has_value());
			CHECK(FindDiagnostic(*after, BuildStartSceneMissingCode) == nullptr);
		}

		TEST_CASE("ProjectValidator: validating needs an open project and, for the scene scope, an open scene" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ValidatorState");
			CHECK(ProjectValidator::Validate(fixture.GetEditor(), ValidationScope::Project).error().GetCode() == ErrorCode::InvalidState);
			fixture.CreateAndOpenProject();
			CHECK(ProjectValidator::Validate(fixture.GetEditor(), ValidationScope::Scene).error().GetCode() == ErrorCode::InvalidState);
			CHECK(ProjectValidator::Validate(fixture.GetEditor(), ValidationScope::Project).has_value());
		}

		TEST_CASE("ProjectValidator: MapLoadCode follows ADR 0006 decision 35")
		{
			CHECK(ProjectValidator::MapLoadCode(SceneDuplicateIdCode) == EntityDuplicateIdCode);
			CHECK(ProjectValidator::MapLoadCode(SceneDanglingParentCode) == SceneInvalidHierarchyCode);
			CHECK(ProjectValidator::MapLoadCode(SceneParentCycleCode) == SceneInvalidHierarchyCode);
			CHECK(ProjectValidator::MapLoadCode(SceneDuplicateUniqueComponentCode) == "SCENE_DUPLICATE_UNIQUE_COMPONENT");
			CHECK(ProjectValidator::MapLoadCode(SceneNonCanonicalOrderCode) == "SCENE_NONCANONICAL_ORDER");
			CHECK(ProjectValidator::MapLoadCode(SceneInconsistentPrefabLinkCode) == "SCENE_INCONSISTENT_PREFAB_LINK");
			CHECK(ProjectValidator::MapLoadCode(SceneUnknownComponentCode).empty());
			CHECK(ProjectValidator::MapLoadCode(SceneUnknownKeyCode).empty());
			CHECK(ProjectValidator::MapLoadCode(SceneInvalidComponentCode).empty());
		}

		TEST_CASE("ProjectValidator: GetCodes lists the codes the M4 validator reports, each once")
		{
			const std::span<const std::string_view> codes = ProjectValidator::GetCodes();
			CHECK(codes.size() == 14);
			std::vector<std::string_view> sorted(codes.begin(), codes.end());
			std::sort(sorted.begin(), sorted.end());
			CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
			CHECK(std::find(codes.begin(), codes.end(), BuildSceneMissingCode) != codes.end());
			CHECK(std::find(codes.begin(), codes.end(), AssetImportFailedCode) != codes.end());
		}
	}

}
