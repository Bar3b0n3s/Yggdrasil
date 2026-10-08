#include "TestsPCH.h"

#include "EditorCore/Project/ProjectValidator.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentRegistration.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AssetTestFixture.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Test {

		// Registry component "ValidatorTarget": a user component with an EntityRef field. M4's built-in components have none
		// outside the prefab components, whose references the structural checks own.
		struct ValidatorTargetComponent
		{
			UUID Target;
		};

		// Registry component "ValidatorFollower": requires ValidatorTarget. Every built-in requirement in M4 is Transform,
		// which every entity has, so a missing requirement needs a component of the test's own.
		struct ValidatorFollowerComponent
		{
			float Speed = 1.0f;
		};

	}

	// The editor's types plus ValidatorTarget and ValidatorFollower (EditorTestFixture's registration hook).
	static void RegisterValidatorTestTypes(TypeRegistry& registry)
	{
		RegisterEditorMethodTypes(registry);
		RegisterComponent<Test::ValidatorTargetComponent>(registry, "ValidatorTarget", "Refers to another entity of the scene (validator tests).")
			.Category("Tests")
			.Version(1)
			.Field("Target", &Test::ValidatorTargetComponent::Target, "The entity it refers to.");
		RegisterComponent<Test::ValidatorFollowerComponent>(registry, "ValidatorFollower", "Follows its ValidatorTarget (validator tests).")
			.Category("Tests")
			.Version(1)
			.Requires<Test::ValidatorTargetComponent>()
			.Field("Speed", &Test::ValidatorFollowerComponent::Speed, "How fast it follows, in metres per second.", { .Min = 0.0, .Unit = "m/s" });
	}

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

	// The diagnostic of `code` reported for the project file `file`, or null.
	static const ProjectDiagnostic* FindFileDiagnostic(const ValidationReport& report, std::string_view file, std::string_view code)
	{
		for (const ProjectDiagnostic& diagnostic : report.Diagnostics)
		{
			if (diagnostic.File == file && diagnostic.Code == code)
				return &diagnostic;
		}
		return nullptr;
	}

	// Every diagnostic as "<file> <code> <entity> <component>.<field>", for failure messages.
	static std::string DescribeDiagnostics(const ValidationReport& report)
	{
		std::string text;
		for (const ProjectDiagnostic& diagnostic : report.Diagnostics)
			text += std::format("{} {} {} {}.{}: {}\n", diagnostic.File, diagnostic.Code, diagnostic.Entity, diagnostic.Component, diagnostic.Field, diagnostic.Message);
		return text;
	}

	// Writes `text` to the project file `path` (project-relative).
	static void WriteValidatorFile(EditorContext& editor, std::string_view path, std::string_view text)
	{
		const Result<VfsPath> file = VfsPath::Create("project", path);
		REQUIRE(file.has_value());
		REQUIRE(editor.WriteProjectFile(*file, std::as_bytes(std::span(text.data(), text.size()))).has_value());
	}

	// The entity named `name` in a scene document, or null.
	static Json* FindDocumentEntity(Json& document, std::string_view name)
	{
		for (Json& entity : document["Entities"])
		{
			if (entity["Name"] == Json(name))
				return &entity;
		}
		return nullptr;
	}

	// The Camera component of the entity at `path` in the open scene.
	static Json GetCameraJson(EditorContext& editor, std::string_view path)
	{
		Result<Json> camera = ComponentAccess::GetComponentJson(editor.GetScene().FindEntityByPath(path), "Camera");
		REQUIRE_MESSAGE(camera.has_value(), camera.error().ToString());
		return std::move(*camera);
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ProjectValidator: multiple primary cameras are reported and fixed keeping the first")
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

		TEST_CASE("ProjectValidator: diagnostic ids depend only on the code, location and subject")
		{
			const std::string id = ProjectValidator::MakeDiagnosticId(SceneMultiplePrimaryCamerasCode, "Assets/Scenes/Main.scene", "", "Camera", "Primary");
			CHECK(id.starts_with("SCENE_MULTIPLE_PRIMARY_CAMERAS-"));
			CHECK(id.size() == std::string_view("SCENE_MULTIPLE_PRIMARY_CAMERAS-").size() + 12);
			// The frozen format (ADR 0008 decision 17): the first 12 hex digits of XXH64 over "code|file|entity|component|field|subject".
			const std::string digest = std::format("{:016x}", XXH64("SCENE_MULTIPLE_PRIMARY_CAMERAS|Assets/Scenes/Main.scene||Camera|Primary|"));
			CHECK(id == "SCENE_MULTIPLE_PRIMARY_CAMERAS-" + digest.substr(0, 12));
			CHECK(id == ProjectValidator::MakeDiagnosticId(SceneMultiplePrimaryCamerasCode, "Assets/Scenes/Main.scene", "", "Camera", "Primary"));
			CHECK(id != ProjectValidator::MakeDiagnosticId(SceneMultiplePrimaryCamerasCode, "Assets/Scenes/Other.scene", "", "Camera", "Primary"));
			CHECK(id != ProjectValidator::MakeDiagnosticId(SceneMultiplePrimaryCamerasCode, "Assets/Scenes/Main.scene", "", "Camera", "Primary", "x"));
			CHECK(id != ProjectValidator::MakeDiagnosticId(SceneNoPrimaryCameraCode, "Assets/Scenes/Main.scene", "", "Camera", "Primary"));
			CHECK(ProjectValidator::MakeDiagnosticId(BuildSceneMissingCode, "Game.eproj", "", "", "Export.BuildScenes", "Assets/Scenes/A.scene")
				!= ProjectValidator::MakeDiagnosticId(BuildSceneMissingCode, "Game.eproj", "", "", "Export.BuildScenes", "Assets/Scenes/B.scene"));
		}

		TEST_CASE("ProjectValidator: diagnostic ids are stable across validations")
		{
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

		TEST_CASE("ProjectValidator: two missing build scenes have distinct ids, and fixing one keeps the other's id")
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

		TEST_CASE("ProjectValidator: fixes inside an open transaction join it")
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

		TEST_CASE("ProjectValidator: fixing selected ids leaves the other diagnostics, in one undoable command")
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

		TEST_CASE("ProjectValidator: structural defects of scene files are reported under the validator codes")
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

		TEST_CASE("ProjectValidator: a scene without a Primary camera is a warning, fixable only when it has exactly one camera")
		{
			Test::EditorTestFixture fixture("ValidatorNoPrimary");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			{
				SceneEdit edit(editor, "Player");
				static_cast<void>(scene.CreateEntity("Player"));
				REQUIRE(edit.Commit().has_value());
			}
			Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(report.has_value());
			const ProjectDiagnostic* none = FindDiagnostic(*report, SceneNoPrimaryCameraCode);
			REQUIRE(none != nullptr);
			CHECK(none->Severity == DiagnosticSeverity::Warning);
			CHECK_FALSE(none->AutoFixable); // no camera to make Primary
			CHECK(none->File == "Assets/Scenes/Main.scene");
			CHECK(none->Component == "Camera");
			CHECK(none->Field == "Primary");
			CHECK(none->Entity.empty());

			{
				SceneEdit edit(editor, "Camera");
				const Json notPrimary = ParseValidatorJson(R"({"Primary":false})");
				REQUIRE(ComponentAccess::AddComponent(scene.CreateEntity("Camera"), "Camera", &notPrimary).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(report.has_value());
			const ProjectDiagnostic* one = FindDiagnostic(*report, SceneNoPrimaryCameraCode);
			REQUIRE(one != nullptr);
			CHECK(one->AutoFixable);
			const std::string id = one->Id;
			const size_t undoCount = editor.GetHistory().GetUndoCount();
			const Result<FixReport> fixed = ProjectValidator::Fix(editor, ValidationScope::Scene, FixSelection{ .All = false, .IdsOrCodes = { id } });
			REQUIRE_MESSAGE(fixed.has_value(), fixed.error().ToString());
			CHECK(fixed->Fixed == std::vector<std::string>{ id });
			CHECK(FindDiagnostic(fixed->After, SceneNoPrimaryCameraCode) == nullptr);
			CHECK(GetCameraJson(editor, "/Camera")["Primary"] == Json(true));
			CHECK(editor.GetHistory().GetUndoCount() == undoCount + 1);
			REQUIRE(editor.GetHistory().Undo(editor) == 1u);
			CHECK(GetCameraJson(editor, "/Camera")["Primary"] == Json(false));

			{
				SceneEdit edit(editor, "Second camera");
				const Json notPrimary = ParseValidatorJson(R"({"Primary":false})");
				REQUIRE(ComponentAccess::AddComponent(scene.CreateEntity("Spare"), "Camera", &notPrimary).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(report.has_value());
			const ProjectDiagnostic* two = FindDiagnostic(*report, SceneNoPrimaryCameraCode);
			REQUIRE(two != nullptr);
			CHECK_FALSE(two->AutoFixable); // which of two cameras renders is the author's choice
			CHECK(two->Id == id);          // the same code at the same location keeps its id
		}

		TEST_CASE("ProjectValidator: a reference to an entity that is gone is reported, and the fix clears it in one undo step")
		{
			Test::EditorTestFixture fixture("ValidatorDangling", {}, &RegisterValidatorTestTypes);
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			UUID target;
			UUID follower;
			{
				SceneEdit edit(editor, "Follower");
				const Entity targetEntity = scene.CreateEntity("Target");
				const Entity followerEntity = scene.CreateEntity("Follower");
				target = targetEntity.GetUUID();
				follower = followerEntity.GetUUID();
				const Json reference = ParseValidatorJson(std::format(R"({{"Target":"{}"}})", target.ToString()));
				REQUIRE(ComponentAccess::AddComponent(followerEntity, "ValidatorTarget", &reference).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(report.has_value());
			CHECK(FindDiagnostic(*report, EntityDanglingReferenceCode) == nullptr);

			{
				SceneEdit edit(editor, "Destroy the target");
				scene.DestroyEntity(scene.FindEntityByID(target));
				REQUIRE(edit.Commit().has_value());
			}
			report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(report.has_value());
			const ProjectDiagnostic* dangling = FindDiagnostic(*report, EntityDanglingReferenceCode);
			REQUIRE_MESSAGE(dangling != nullptr, DescribeDiagnostics(*report));
			CHECK(dangling->Severity == DiagnosticSeverity::Warning);
			CHECK(dangling->AutoFixable);
			CHECK(dangling->File == "Assets/Scenes/Main.scene");
			CHECK(dangling->Entity == follower.ToString());
			CHECK(dangling->Component == "ValidatorTarget");
			CHECK(dangling->Field == "Target");
			CHECK(dangling->Message.contains(target.ToString()));

			const size_t undoCount = editor.GetHistory().GetUndoCount();
			const Result<FixReport> fixed =
				ProjectValidator::Fix(editor, ValidationScope::Scene, FixSelection{ .All = false, .IdsOrCodes = { std::string(EntityDanglingReferenceCode) } });
			REQUIRE_MESSAGE(fixed.has_value(), fixed.error().ToString());
			CHECK(fixed->Fixed.size() == 1);
			CHECK(FindDiagnostic(fixed->After, EntityDanglingReferenceCode) == nullptr);
			CHECK(editor.GetHistory().GetUndoCount() == undoCount + 1);
			const auto readTarget = [&editor, follower]()
			{
				const Result<Json> component = ComponentAccess::GetComponentJson(editor.GetScene().FindEntityByID(follower), "ValidatorTarget");
				REQUIRE(component.has_value());
				return (*component)["Target"];
			};
			CHECK(readTarget().is_null()); // the invalid UUID: no entity
			REQUIRE(editor.GetHistory().Undo(editor) == 1u);
			CHECK(readTarget() == Json(target.ToString()));
		}

		TEST_CASE("ProjectValidator: scene files report order, prefab links, component problems and unloadable files under the validator codes")
		{
			Test::EditorTestFixture fixture("ValidatorFileCodes", {}, &RegisterValidatorTestTypes);
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			for (const auto& [file, target] : std::array<std::pair<std::string_view, std::string_view>, 2>{ {
					 { "Scenes/Invalid/ChildBeforeParent.scene", "Assets/Scenes/ChildBeforeParent.scene" },
					 { "Scenes/Invalid/PrefabLinkMissingRoot.scene", "Assets/Scenes/PrefabLinkMissingRoot.scene" },
				 } })
			{
				const Result<std::string> text = Test::ReadTestDataText(file);
				REQUIRE(text.has_value());
				WriteValidatorFile(editor, target, *text);
			}

			// Component problems, made from a valid scene document: a body, a character and a follower with default values.
			{
				SceneEdit edit(editor, "Bodies");
				REQUIRE(ComponentAccess::AddComponent(scene.CreateEntity("Body"), "RigidBody", nullptr).has_value());
				REQUIRE(ComponentAccess::AddComponent(scene.CreateEntity("Character"), "CharacterController", nullptr).has_value());
				const Entity follower = scene.CreateEntity("Follower");
				REQUIRE(ComponentAccess::AddComponent(follower, "ValidatorTarget", nullptr).has_value());
				REQUIRE(ComponentAccess::AddComponent(follower, "ValidatorFollower", nullptr).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			const Result<std::string> saved = SceneSerializer::SaveToString(scene);
			REQUIRE(saved.has_value());
			Json valid = ParseValidatorJson(*saved);
			const auto writeVariant = [&editor, &valid](std::string_view path, std::string_view entity, const auto& change)
			{
				Json document = valid;
				Json* changed = FindDocumentEntity(document, entity);
				REQUIRE(changed != nullptr);
				change(document, *changed);
				WriteValidatorFile(editor, path, document.dump(1, '\t'));
			};
			writeVariant("Assets/Scenes/OutOfRange.scene", "Body", [](Json& /*document*/, Json& body)
			{
				body["Components"]["RigidBody"]["Friction"] = -1.0;
			});
			writeVariant("Assets/Scenes/Conflict.scene", "Body", [](Json& document, Json& body)
			{
				body["Components"]["CharacterController"] = (*FindDocumentEntity(document, "Character"))["Components"]["CharacterController"];
			});
			writeVariant("Assets/Scenes/Requirement.scene", "Follower", [](Json& /*document*/, Json& follower)
			{
				follower["Components"].erase("ValidatorTarget");
			});
			WriteValidatorFile(editor, "Assets/Scenes/Unreadable.scene", "{\"Format\": \"Scene\", ");
			const std::string bodyId = JsonReader((*FindDocumentEntity(valid, "Body"))["ID"]).ReadString().value_or(std::string());
			const std::string followerId = JsonReader((*FindDocumentEntity(valid, "Follower"))["ID"]).ReadString().value_or(std::string());

			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(report.has_value());
			INFO(DescribeDiagnostics(*report));
			struct Expected
			{
				std::string_view File;
				std::string_view Code;
				DiagnosticSeverity Severity = DiagnosticSeverity::Error;
				std::string_view Entity;
				std::string_view Component;
				std::string_view Field;
			};
			const std::array<Expected, 6> expectations = { {
				{ "Assets/Scenes/ChildBeforeParent.scene", SceneNonCanonicalOrderCode, DiagnosticSeverity::Warning, "4d00000000000002", "", "" },
				{ "Assets/Scenes/PrefabLinkMissingRoot.scene", SceneInconsistentPrefabLinkCode, DiagnosticSeverity::Error, "4d00000000000001", "", "" },
				{ "Assets/Scenes/OutOfRange.scene", ComponentFieldOutOfRangeCode, DiagnosticSeverity::Error, bodyId, "RigidBody", "Friction" },
				{ "Assets/Scenes/Conflict.scene", ComponentConflictCode, DiagnosticSeverity::Error, bodyId, "CharacterController", "" },
				{ "Assets/Scenes/Requirement.scene", ComponentMissingRequirementCode, DiagnosticSeverity::Error, followerId, "ValidatorFollower", "" },
				{ "Assets/Scenes/Unreadable.scene", AssetImportFailedCode, DiagnosticSeverity::Error, "", "", "" },
			} };
			for (const Expected& expected : expectations)
			{
				INFO(std::string(expected.File));
				const ProjectDiagnostic* diagnostic = FindFileDiagnostic(*report, expected.File, expected.Code);
				REQUIRE(diagnostic != nullptr);
				CHECK(diagnostic->Severity == expected.Severity);
				CHECK_FALSE(diagnostic->AutoFixable); // files that are not open are fixed by opening them (M6: file-edit commands)
				CHECK(diagnostic->Entity == expected.Entity);
				CHECK(diagnostic->Component == expected.Component);
				CHECK(diagnostic->Field == expected.Field);
				CHECK_FALSE(diagnostic->Hint.empty());
			}
		}

		TEST_CASE("ProjectValidator: a missing start scene is an error that cannot be fixed")
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

		TEST_CASE("ProjectValidator: validating needs an open project and, for the scene scope, an open scene")
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

		TEST_CASE("ProjectValidator: GetCodes lists the codes the validator reports, each once")
		{
			const std::span<const std::string_view> codes = ProjectValidator::GetCodes();
			// M4's 14 codes, and M6's 11: the asset codes but the runtime-only ASSET_UPLOAD_FAILED, and PREFAB_MISSING_ASSET;
			// M11's 10 physics codes.
			CHECK(codes.size() == 25 + PhysicsDiagnosticCodes.size());
			std::vector<std::string_view> sorted(codes.begin(), codes.end());
			std::sort(sorted.begin(), sorted.end());
			CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
			CHECK(std::find(codes.begin(), codes.end(), BuildSceneMissingCode) != codes.end());
			CHECK(std::find(codes.begin(), codes.end(), AssetImportFailedCode) != codes.end());
		}

		TEST_CASE("ProjectValidator: the asset scan diagnostics are reported under their codes and fixed in one undo step")
		{
			Test::EditorTestFixture fixture("ValidatorAssetScan");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			// A copy-pasted texture pair (duplicate handle) and an orphan .meta, written outside the editor's knowledge.
			const std::string meta = R"({
	"Format": "AssetMeta",
	"Version": 1,
	"Handle": "1111222233334444",
	"Type": "Texture",
	"Importer": "Texture",
	"ImporterVersion": 1,
	"Settings": {
		"Usage": "Color",
		"GenerateMips": true
	},
	"SubAssets": []
}
)";
			WriteValidatorFile(editor, "Assets/A.png", "not decoded by the scan");
			WriteValidatorFile(editor, "Assets/A.png.meta", meta);
			WriteValidatorFile(editor, "Assets/B.png", "not decoded by the scan");
			WriteValidatorFile(editor, "Assets/B.png.meta", meta);
			WriteValidatorFile(editor, "Assets/Gone.png.meta", meta);
			REQUIRE(editor.GetAssets().Refresh().has_value());

			Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			INFO(DescribeDiagnostics(*report));
			const ProjectDiagnostic* duplicate = FindFileDiagnostic(*report, "Assets/B.png.meta", AssetDuplicateHandleCode);
			REQUIRE(duplicate != nullptr);
			CHECK(duplicate->AutoFixable);
			CHECK(duplicate->Asset == "1111222233334444");
			const ProjectDiagnostic* orphan = FindFileDiagnostic(*report, "Assets/Gone.png.meta", AssetOrphanMetaCode);
			REQUIRE(orphan != nullptr);
			CHECK(orphan->Severity == DiagnosticSeverity::Warning);

			Result<FixReport> fixed = ProjectValidator::Fix(editor, ValidationScope::Project, { .All = true, .IdsOrCodes = {} });
			REQUIRE_MESSAGE(fixed.has_value(), fixed.error().ToString());
			CHECK(fixed->Fixed.size() == 2);
			CHECK(fixed->UndoIndex != 0);
			CHECK(FindDiagnostic(fixed->After, AssetDuplicateHandleCode) == nullptr);
			CHECK(FindDiagnostic(fixed->After, AssetOrphanMetaCode) == nullptr);
			// The orphan went to the trash, never deleted (§12.3); undo restores both files.
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			Result<ValidationReport> undone = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(undone.has_value());
			CHECK(FindDiagnostic(*undone, AssetDuplicateHandleCode) != nullptr);
			CHECK(FindDiagnostic(*undone, AssetOrphanMetaCode) != nullptr);
		}

		TEST_CASE("ProjectValidator: a .meta whose importer disagrees with its source is rewritten by the fix, keeping its handle")
		{
			Test::EditorTestFixture fixture("ValidatorTypeMismatch");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			// A material's .meta beside a texture (a .meta copied onto the wrong file): ASSET_TYPE_MISMATCH, an Error that
			// blocks play and export until the .meta names the Texture importer (§7.2, §7.3).
			const std::string meta = R"({
	"Format": "AssetMeta",
	"Version": 1,
	"Handle": "5555666677778888",
	"Type": "Material",
	"Importer": "Material",
	"ImporterVersion": 1,
	"Settings": null,
	"SubAssets": []
}
)";
			const auto projectPath = [](std::string_view relative)
			{
				Result<VfsPath> path = VfsPath::Create("project", relative);
				REQUIRE(path.has_value());
				return *path;
			};
			const auto readText = [&editor, &projectPath](std::string_view relative)
			{
				Result<Buffer> bytes = editor.GetVfs().ReadFile(projectPath(relative));
				REQUIRE(bytes.has_value());
				return std::string(AsStringView(*bytes));
			};
			// Written by another program; the refresh imports the new asset with the importer its .meta names, which fails.
			REQUIRE(editor.GetVfs().WriteFileAtomic(projectPath("Assets/Wood.png.meta"), AsBytes(meta)).has_value());
			REQUIRE(editor.GetVfs().WriteFileAtomic(projectPath("Assets/Wood.png"), Test::MakeTestPng(4, 4)).has_value());
			const Test::ExpectLog importFailure(LogLevel::Error, "ASSET_IMPORT_FAILED Assets/Wood.png");
			REQUIRE(editor.GetAssets().Refresh().has_value());

			Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			INFO(DescribeDiagnostics(*report));
			const ProjectDiagnostic* mismatch = FindFileDiagnostic(*report, "Assets/Wood.png.meta", AssetTypeMismatchCode);
			REQUIRE(mismatch != nullptr);
			CHECK(mismatch->Severity == DiagnosticSeverity::Error);
			CHECK(mismatch->AutoFixable);

			Result<FixReport> fixed = ProjectValidator::Fix(editor, ValidationScope::Project, { .All = false, .IdsOrCodes = { std::string(AssetTypeMismatchCode) } });
			REQUIRE_MESSAGE(fixed.has_value(), fixed.error().ToString());
			CHECK(fixed->Fixed.size() == 1);
			CHECK(fixed->UndoIndex != 0);
			CHECK(FindDiagnostic(fixed->After, AssetTypeMismatchCode) == nullptr);
			// The Texture importer's .meta with the same handle and its complete default settings; the texture imports.
			const Result<AssetMetadata> rewritten = ParseAssetMetadata(readText("Assets/Wood.png.meta"));
			REQUIRE(rewritten.has_value());
			CHECK(rewritten->Handle == AssetHandle(0x5555666677778888ull));
			CHECK(rewritten->Type == AssetType::Texture);
			CHECK(rewritten->Importer == "Texture");
			REQUIRE(rewritten->Settings.Get().is_object());
			CHECK(rewritten->Settings.Get().contains("Usage"));
			CHECK(editor.GetAssets().Load(AssetHandle(0x5555666677778888ull)).has_value());

			// One undo step restores the old .meta.
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(readText("Assets/Wood.png.meta") == meta);
		}

		TEST_CASE("ProjectValidator: a reference to an unregistered asset is ASSET_MISSING")
		{
			Test::EditorTestFixture fixture("ValidatorAssetMissing");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				SceneEdit edit(editor, "Add Renderer");
				Entity entity = editor.GetScene().CreateEntity("Box");
				REQUIRE(ComponentAccess::AddComponent(entity, "MeshRenderer", nullptr).has_value());
				REQUIRE(ComponentAccess::PatchComponentJson(entity, "MeshRenderer", ParseValidatorJson(R"({"Mesh": "7777000077770000"})")).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			const ProjectDiagnostic* missing = FindDiagnostic(*report, AssetMissingCode);
			REQUIRE(missing != nullptr);
			CHECK(missing->Severity == DiagnosticSeverity::Error);
			CHECK(missing->Component == "MeshRenderer");
			CHECK(missing->Field == "Mesh");
			CHECK(missing->Asset == "7777000077770000");
			// A built-in reference is never missing.
			{
				SceneEdit edit(editor, "Use Cube");
				REQUIRE(ComponentAccess::PatchComponentJson(editor.GetScene().FindEntityByPath("/Box"), "MeshRenderer",
					ParseValidatorJson(R"({"Mesh": "0000000000000101"})"))
						.has_value());
				REQUIRE(edit.Commit().has_value());
			}
			Result<ValidationReport> fixedReport = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(fixedReport.has_value());
			CHECK(FindDiagnostic(*fixedReport, AssetMissingCode) == nullptr);
		}

		TEST_CASE("ProjectValidator: an instance of an unregistered prefab is PREFAB_MISSING_ASSET")
		{
			Test::EditorTestFixture fixture("ValidatorPrefabMissing");
			fixture.CreateAndOpenProject();
			Result<std::string> scene = Test::ReadTestDataText("Scenes/AllComponents.scene");
			REQUIRE(scene.has_value());
			// AllComponents.scene holds a prefab instance whose prefab asset this project does not have.
			WriteValidatorFile(fixture.GetEditor(), "Assets/Scenes/Instances.scene", *scene);
			Result<ValidationReport> report = ProjectValidator::Validate(fixture.GetEditor(), ValidationScope::Project);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			const ProjectDiagnostic* missing = FindFileDiagnostic(*report, "Assets/Scenes/Instances.scene", PrefabMissingAssetCode);
			REQUIRE(missing != nullptr);
			CHECK(missing->Severity == DiagnosticSeverity::Warning);
			CHECK_FALSE(missing->AutoFixable);
		}

		TEST_CASE("ProjectValidator: GetCodes lists the M6 asset codes")
		{
			const std::span<const std::string_view> codes = ProjectValidator::GetCodes();
			for (const std::string_view code : GetAssetDiagnosticCodes())
			{
				CAPTURE(std::string(code));
				const bool listed = std::find(codes.begin(), codes.end(), code) != codes.end();
				// Every asset code but the runtime-only upload failure is a validator code.
				CHECK(listed == (code != AssetUploadFailedCode));
			}
			CHECK(std::find(codes.begin(), codes.end(), PrefabMissingAssetCode) != codes.end());
		}

		TEST_CASE("ProjectValidator: GetCodes lists the M11 physics codes right after PREFAB_MISSING_ASSET")
		{
			const std::span<const std::string_view> codes = ProjectValidator::GetCodes();
			const auto prefab = std::find(codes.begin(), codes.end(), PrefabMissingAssetCode);
			REQUIRE(prefab != codes.end());
			// §13.7 lists the physics codes right after PREFAB_MISSING_ASSET, in PhysicsDiagnosticCodes' order.
			REQUIRE(static_cast<size_t>(codes.end() - prefab) > PhysicsDiagnosticCodes.size());
			for (size_t index = 0; index < PhysicsDiagnosticCodes.size(); ++index)
			{
				CAPTURE(std::string(PhysicsDiagnosticCodes[index]));
				CHECK(*(prefab + 1 + static_cast<std::ptrdiff_t>(index)) == PhysicsDiagnosticCodes[index]);
			}
		}

		// M11: the physics checks of Scene/PhysicsValidation.h through project.validate (Docs/Decisions/0014-m11-decisions.md
		// decision 15): the diagnostics of the open scene under their codes, and the fix of PHYSICS_ADJACENT_STATIC_BODIES.
		TEST_CASE("ProjectValidator: physics diagnostics of the open scene are reported under their codes")
		{
			Test::EditorTestFixture fixture("ValidatorPhysics");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				SceneEdit edit(editor, "Physics problems");
				const Entity locked = editor.GetScene().CreateEntity("Locked");
				const Json body = ParseValidatorJson(R"({"Type":"Dynamic","LockTranslation":[true,true,true],"LockRotation":[true,true,true]})");
				REQUIRE(ComponentAccess::AddComponent(locked, "RigidBody", &body).has_value());
				REQUIRE(ComponentAccess::AddComponent(locked, "BoxCollider", nullptr).has_value());
				const Entity stray = editor.GetScene().CreateEntity("Stray");
				const Json strayBody = ParseValidatorJson(R"({"Layer":"NoSuchLayer"})");
				REQUIRE(ComponentAccess::AddComponent(stray, "RigidBody", &strayBody).has_value());
				REQUIRE(ComponentAccess::AddComponent(stray, "SphereCollider", nullptr).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(report.has_value());
			INFO(DescribeDiagnostics(*report));
			const ProjectDiagnostic* locked = FindDiagnostic(*report, PhysicsAllDofsLockedCode);
			REQUIRE(locked != nullptr);
			CHECK(locked->Severity == DiagnosticSeverity::Error);
			CHECK(locked->Entity == editor.GetScene().FindEntityByPath("/Locked").GetUUID().ToString());
			CHECK(locked->Component == "RigidBody");
			CHECK(locked->File == "Assets/Scenes/Main.scene");
			CHECK_FALSE(locked->AutoFixable);
			const ProjectDiagnostic* layer = FindDiagnostic(*report, PhysicsUnknownLayerCode);
			REQUIRE(layer != nullptr);
			CHECK(layer->Field == "Layer");
		}

		TEST_CASE("ProjectValidator: fixing PHYSICS_ADJACENT_STATIC_BODIES adds a Static RigidBody to the common parent in one undo step")
		{
			Test::EditorTestFixture fixture("ValidatorAdjacentStatic");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				SceneEdit edit(editor, "Track");
				const Entity track = editor.GetScene().CreateEntity("Track");
				for (const std::string_view translation : { R"({"Translation":[0,0,0]})", R"({"Translation":[1,0,0]})" })
				{
					const Entity piece = editor.GetScene().CreateEntity("Piece", track);
					const Json transform = ParseValidatorJson(translation);
					REQUIRE(ComponentAccess::PatchComponentJson(piece, "Transform", transform).has_value());
					REQUIRE(ComponentAccess::AddComponent(piece, "BoxCollider", nullptr).has_value());
				}
				REQUIRE(edit.Commit().has_value());
			}
			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(report.has_value());
			const ProjectDiagnostic* adjacent = FindDiagnostic(*report, PhysicsAdjacentStaticBodiesCode);
			REQUIRE(adjacent != nullptr);
			CHECK(adjacent->Severity == DiagnosticSeverity::Warning);
			CHECK(adjacent->AutoFixable);

			const size_t undoCount = editor.GetHistory().GetUndoCount();
			const Result<FixReport> fixed =
				ProjectValidator::Fix(editor, ValidationScope::Scene, FixSelection{ .All = false, .IdsOrCodes = { std::string(PhysicsAdjacentStaticBodiesCode) } });
			REQUIRE_MESSAGE(fixed.has_value(), fixed.error().ToString());
			CHECK(fixed->Fixed == std::vector<std::string>{ adjacent->Id });
			CHECK(FindDiagnostic(fixed->After, PhysicsAdjacentStaticBodiesCode) == nullptr);
			CHECK(editor.GetHistory().GetUndoCount() == undoCount + 1);
			const Result<Json> body = ComponentAccess::GetComponentJson(editor.GetScene().FindEntityByPath("/Track"), "RigidBody");
			REQUIRE(body.has_value());
			CHECK((*body)["Type"] == Json("Static"));
			REQUIRE(editor.GetHistory().Undo(editor) == 1u);
			CHECK_FALSE(ComponentAccess::GetComponentJson(editor.GetScene().FindEntityByPath("/Track"), "RigidBody").has_value());
		}

		TEST_CASE("ProjectValidator: fixing nested adjacent groups gives only the outermost common parent a RigidBody")
		{
			// Level holds two groups of two touching pieces, and the groups touch each other: the groups' pairs name the groups,
			// the pair across them names Level. One Static RigidBody on Level makes all four pieces one compound.
			Test::EditorTestFixture fixture("ValidatorNestedAdjacent");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				SceneEdit edit(editor, "Level");
				Scene& scene = editor.GetScene();
				const Entity level = scene.CreateEntity("Level");
				float x = 0.0f;
				for (const std::string_view group : { "GroupA", "GroupB" })
				{
					const Entity parent = scene.CreateEntity(group, level);
					for (int index = 0; index < 2; ++index, x += 1.0f)
					{
						const Entity piece = scene.CreateEntity("Piece", parent);
						Json transform = Json::object();
						transform["Translation"] = Json::array({ x, 0.0f, 0.0f });
						REQUIRE(ComponentAccess::PatchComponentJson(piece, "Transform", transform).has_value());
						REQUIRE(ComponentAccess::AddComponent(piece, "BoxCollider", nullptr).has_value());
					}
				}
				REQUIRE(edit.Commit().has_value());
			}
			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Scene);
			REQUIRE(report.has_value());
			INFO(DescribeDiagnostics(*report));
			std::vector<std::string> adjacent;
			for (const ProjectDiagnostic& diagnostic : report->Diagnostics)
			{
				if (diagnostic.Code == PhysicsAdjacentStaticBodiesCode)
				{
					CHECK(diagnostic.AutoFixable);
					adjacent.push_back(diagnostic.Id);
				}
			}
			REQUIRE(adjacent.size() == 3);

			const Result<FixReport> fixed = ProjectValidator::Fix(editor, ValidationScope::Scene, FixSelection{ .All = true, .IdsOrCodes = {} });
			REQUIRE_MESSAGE(fixed.has_value(), fixed.error().ToString());
			CHECK(fixed->Fixed.size() == 3);
			CHECK(FindDiagnostic(fixed->After, PhysicsAdjacentStaticBodiesCode) == nullptr);
			const Scene& scene = editor.GetScene();
			const Result<Json> body = ComponentAccess::GetComponentJson(scene.FindEntityByPath("/Level"), "RigidBody");
			REQUIRE(body.has_value());
			CHECK((*body)["Type"] == Json("Static"));
			CHECK_FALSE(scene.FindEntityByPath("/Level/GroupA").HasComponent<RigidBodyComponent>());
			CHECK_FALSE(scene.FindEntityByPath("/Level/GroupB").HasComponent<RigidBodyComponent>());
		}

		TEST_CASE("ProjectValidator: physics diagnostics of a scene file that is not open are reported without a fix")
		{
			Test::EditorTestFixture fixture("ValidatorPhysicsFile");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				SceneEdit edit(editor, "Track");
				const Entity track = editor.GetScene().CreateEntity("Track");
				for (const std::string_view translation : { R"({"Translation":[0,0,0]})", R"({"Translation":[1,0,0]})" })
				{
					const Entity piece = editor.GetScene().CreateEntity("Piece", track);
					const Json transform = ParseValidatorJson(translation);
					REQUIRE(ComponentAccess::PatchComponentJson(piece, "Transform", transform).has_value());
					REQUIRE(ComponentAccess::AddComponent(piece, "BoxCollider", nullptr).has_value());
				}
				REQUIRE(edit.Commit().has_value());
			}
			// The track goes into a second scene file; the open scene loses it again.
			const std::span<const UUID> pieces = editor.GetScene().FindEntityByPath("/Track").GetChildren();
			REQUIRE(pieces.size() == 2);
			const std::string first = pieces[0].ToString();
			const std::string second = pieces[1].ToString();
			const Result<std::string> saved = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(saved.has_value());
			WriteValidatorFile(editor, "Assets/Scenes/Track.scene", *saved);
			REQUIRE(editor.GetHistory().Undo(editor) == 1u);

			const Result<ValidationReport> report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(report.has_value());
			INFO(DescribeDiagnostics(*report));
			CHECK(FindFileDiagnostic(*report, "Assets/Scenes/Main.scene", PhysicsAdjacentStaticBodiesCode) == nullptr);
			const ProjectDiagnostic* adjacent = FindFileDiagnostic(*report, "Assets/Scenes/Track.scene", PhysicsAdjacentStaticBodiesCode);
			REQUIRE(adjacent != nullptr);
			CHECK(adjacent->Severity == DiagnosticSeverity::Warning);
			CHECK_FALSE(adjacent->AutoFixable);
			// On the piece first in canonical order; the id is made from the file, that piece and the other one (the subject),
			// so it is the same in every run.
			CHECK(adjacent->Entity == first);
			CHECK(adjacent->Id == ProjectValidator::MakeDiagnosticId(PhysicsAdjacentStaticBodiesCode, "Assets/Scenes/Track.scene", first, "", "", second));
		}
	}

}
