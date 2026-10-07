#include "TestsPCH.h"

#include "EditorCore/EditorContext.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static VfsPath MakeEditorPath(std::string_view text)
	{
		Result<VfsPath> path = VfsPath::Parse(text);
		REQUIRE(path.has_value());
		return *path;
	}

	static std::span<const std::byte> AsEditorBytes(std::string_view text)
	{
		return std::as_bytes(std::span(text.data(), text.size()));
	}

	namespace {

		// The open scene "Main" as another program rewrites it in the SceneChangedOnDisk tests: another seed and one entity,
		// so both its content and its size differ from what EditorTestFixture::CreateAndOpenScene wrote.
		constexpr std::string_view ExternalMainScene = R"({
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
)";

		size_t CountEditorEvents(EditorContext& editor, EngineEventType type)
		{
			const EngineEventType types[] = { type };
			return editor.GetEngine().GetEventLog().Read(0, types, 100).Events.size();
		}

		// A command whose Undo fails, as a settings command's does when its file cannot be written.
		class UndoFailsCommand final : public Command
		{
		public:
			Status Execute(EditorContext& /*context*/) override { return {}; }
			Status Undo(EditorContext& /*context*/) override { return MakeError(ErrorCode::Io, "the file cannot be written"); }
			std::string_view GetLabel() const override { return "Write Settings"; }
			bool ChangesScene() const override { return false; }
			size_t GetMemorySize() const override { return 16; }
		};

	}

	// Creates one entity in its own SceneEdit and returns its id.
	static UUID CreateTrackedEntity(EditorContext& editor, std::string_view name)
	{
		SceneEdit edit(editor, std::format("Create '{}'", name));
		const UUID id = editor.GetScene().CreateEntity(name).GetUUID();
		REQUIRE(edit.Commit().has_value());
		return id;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorContext: starts in the launcher state without a project or scene")
		{
			Test::EditorTestFixture fixture("EditorLauncher");
			EditorContext& editor = fixture.GetEditor();
			CHECK_FALSE(editor.HasProject());
			CHECK_FALSE(editor.HasScene());
			CHECK_FALSE(editor.GetScenePath().has_value());
			CHECK(editor.GetSelection().empty());
			CHECK(editor.GetTransaction() == nullptr);
			CHECK_FALSE(editor.IsDryRun());
			CHECK(editor.GetCommandOrigin() == CommandOrigin::User);
			CHECK(editor.GetWriteAttribution().Method == "ui");
			CHECK_FALSE(editor.GetShutdownRequest().has_value());
			CHECK(&editor.GetTypeRegistry() == &fixture.GetEngine().GetTypeRegistry());
			CHECK(&editor.GetVfs() == &fixture.GetEngine().GetVfs());
		}

		TEST_CASE("EditorContext: without a given id generator state each editor seeds its own from the OS")
		{
			Test::EditorTestFixture fixture("EditorSeed");
			Result<Scope<EditorContext>> first = EditorContext::Create(fixture.GetEngine(), {});
			Result<Scope<EditorContext>> second = EditorContext::Create(fixture.GetEngine(), {});
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			CHECK_FALSE((*first)->GetSpecification().IdGeneratorState.has_value());
			// 64-bit ids from two independently seeded generators: equal only with probability 2^-64.
			CHECK((*first)->GetIdGenerator().Next() != (*second)->GetIdGenerator().Next());
			// A given state is reproducible.
			CHECK(fixture.GetEditor().GetSpecification().IdGeneratorState == Test::EditorTestIdState);
		}

		TEST_CASE("EditorContext: the revision grows with every mutation and never repeats across scenes")
		{
			Test::EditorTestFixture fixture("EditorRevision");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			CHECK(editor.GetRevision() == 0);
			fixture.CreateAndOpenScene();
			const uint64_t opened = editor.GetRevision();
			static_cast<void>(CreateTrackedEntity(editor, "A"));
			const uint64_t edited = editor.GetRevision();
			CHECK(edited > opened);

			// A new Scene object restarts its own revision, but the editor's keeps growing, so an ifRevision taken in the first
			// scene can never match the second (no ABA).
			editor.SetScene(editor.CreateScene("Other"), MakeEditorPath("project://Assets/Scenes/Other.scene"));
			CHECK(editor.GetRevision() > edited);
			editor.CloseScene();
			const uint64_t closed = editor.GetRevision();
			CHECK(closed > edited);
			editor.SetScene(editor.CreateScene("Third"), std::nullopt);
			CHECK(editor.GetRevision() > closed);
		}

		TEST_CASE("EditorContext: new scenes use the context's registry and id generator")
		{
			Test::EditorTestFixture fixture("EditorCreateScene");
			Scope<Scene> scene = fixture.GetEditor().CreateScene("Main");
			REQUIRE(scene != nullptr);
			CHECK(scene->GetName() == "Main");
			CHECK(&scene->GetTypeRegistry() == &fixture.GetEditor().GetTypeRegistry());
			CHECK(&scene->GetUUIDGenerator() == &fixture.GetEditor().GetIdGenerator());
			CHECK_FALSE(scene->IsRuntime());
		}

		TEST_CASE("EditorContext: opening a project mounts project:// and cache://")
		{
			Test::EditorTestFixture fixture("EditorOpen");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			CHECK(editor.HasProject());
			CHECK_FALSE(editor.IsReadOnly());
			CHECK(editor.GetVfs().IsMounted("project"));
			CHECK(editor.GetVfs().IsMounted("cache"));
			CHECK(editor.GetVfs().Exists(MakeEditorPath("project://TestProject.eproj")));
			CHECK(editor.GetProject().GetSettings().Name == "TestProject");
			CHECK(editor.GetProvenance() != nullptr);

			editor.CloseProject();
			CHECK_FALSE(editor.HasProject());
			CHECK_FALSE(editor.GetVfs().IsMounted("project"));
		}

		TEST_CASE("EditorContext: a second project while one is open is InvalidState")
		{
			Test::EditorTestFixture fixture("EditorSecondProject");
			fixture.CreateAndOpenProject("First");
			const Result<CreatedProject> second = ProjectManager::CreateProject(
				{ .Directory = fixture.GetProjectRoot("Second"), .Name = "Second", .Template = ProjectTemplate::Empty, .TemplatesDirectory = fixture.GetEditor().GetSpecification().TemplatesDirectory },
				fixture.GetEngine().GetTypeRegistry());
			REQUIRE(second.has_value());
			Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(second->ProjectFile, {}, fixture.GetEngine().GetTypeRegistry());
			REQUIRE(project.has_value());
			const Status opened = fixture.GetEditor().OpenProject(std::move(*project));
			REQUIRE_FALSE(opened.has_value());
			CHECK(opened.error().GetCode() == ErrorCode::InvalidState);
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == "First");
		}

		TEST_CASE("EditorContext: WriteProjectFile records provenance for Assets/ and the .eproj only")
		{
			Test::EditorTestFixture fixture("EditorProvenance");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			WriteAttribution attribution;
			attribution.Method = "scene.save";
			attribution.RequestId = VariantValue(Json(17));
			attribution.Client = "engine-mcp";
			attribution.TranscriptLine = 42;
			editor.SetWriteAttribution(attribution);
			CHECK(editor.GetCommandOrigin() == CommandOrigin::Agent);

			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Scenes/Main.scene"), AsEditorBytes("{}\n")).has_value());
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Library/Cache/x.bin"), AsEditorBytes("x")).has_value());
			editor.SetWriteAttribution(std::nullopt);

			const ProvenanceRecorder* provenance = editor.GetProvenance();
			REQUIRE(provenance != nullptr);
			const ProvenanceEntry* entry = provenance->Find("Assets/Scenes/Main.scene");
			REQUIRE(entry != nullptr);
			CHECK(entry->Hash == XXH64(std::string_view("{}\n")));
			CHECK(entry->Method == "scene.save");
			CHECK(entry->RequestId.Get() == Json(17));
			CHECK(entry->Client == "engine-mcp");
			CHECK(entry->TranscriptLine == 42u);
			CHECK(provenance->Find("Library/Cache/x.bin") == nullptr);
			CHECK(FileSystem::Exists(fixture.GetProjectRoot() / "Automation/Provenance.json"));
		}

		TEST_CASE("EditorContext: a read-only project refuses writes and commands with PermissionDenied")
		{
			Test::EditorTestFixture fixture("EditorReadOnly");
			fixture.CreateAndOpenProject();
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			fixture.GetEditor().CloseProject();

			Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(projectFile,
				{ .ReadOnly = true, .StrictUnknowns = false, .ReadOnlyCacheDirectory = fixture.GetDirectory() / "ReadOnlyCache" },
				fixture.GetEngine().GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(fixture.GetEditor().OpenProject(std::move(*project)).has_value());
			CHECK(fixture.GetEditor().IsReadOnly());
			CHECK(fixture.GetEditor().GetProvenance() == nullptr);
			const Status write = fixture.GetEditor().WriteProjectFile(MakeEditorPath("project://Assets/A.scene"), AsEditorBytes("{}"));
			REQUIRE_FALSE(write.has_value());
			CHECK(write.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK_FALSE(FileSystem::Exists(fixture.GetProjectRoot() / "Assets/A.scene"));
		}

		TEST_CASE("EditorContext: SetScene clears the history and the selection")
		{
			Test::EditorTestFixture fixture("EditorSetScene");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const UUID id = CreateTrackedEntity(editor, "Selected");
			editor.SetSelection({ id, id, UUID(0xdead) });
			CHECK(editor.GetSelection().size() == 1);
			CHECK(editor.GetHistory().CanUndo());

			editor.SetScene(editor.CreateScene("Other"), MakeEditorPath("project://Assets/Scenes/Other.scene"));
			CHECK(editor.GetSelection().empty());
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK_FALSE(editor.IsSceneDirty());
			const std::optional<VfsPath>& path = editor.GetScenePath();
			REQUIRE(path.has_value());
			CHECK(path->GetPath() == "Assets/Scenes/Other.scene");
		}

		TEST_CASE("EditorContext: commands executed while serving a request are agent commands")
		{
			Test::EditorTestFixture fixture("EditorOrigin");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			WriteAttribution attribution;
			attribution.Method = "entity.create";
			attribution.Client = "test";
			editor.SetWriteAttribution(attribution);
			static_cast<void>(CreateTrackedEntity(editor, "ByAgent"));
			editor.SetWriteAttribution(std::nullopt);
			static_cast<void>(CreateTrackedEntity(editor, "ByUser"));
			const std::vector<CommandHistoryEntry> entries = editor.GetHistory().GetEntries(10);
			REQUIRE(entries.size() == 2);
			CHECK(entries[0].Origin == CommandOrigin::Agent);
			CHECK(entries[0].Label.starts_with("[agent] "));
			CHECK(entries[1].Origin == CommandOrigin::User);
		}

		TEST_CASE("EditorTransaction: commit records one composite undo step")
		{
			Test::EditorTestFixture fixture("EditorTransactionCommit");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			EditorTransaction transaction(editor, "Scaffold");
			CHECK(editor.GetTransaction() == &transaction);
			static_cast<void>(CreateTrackedEntity(editor, "A"));
			static_cast<void>(CreateTrackedEntity(editor, "B"));
			CHECK(transaction.GetCommandCount() == 2);
			const uint64_t undoIndex = transaction.Commit();
			CHECK(undoIndex == editor.GetHistory().GetCurrentSequence());
			CHECK(editor.GetTransaction() == nullptr);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK(editor.GetScene().GetEntityCount() == 2);
			CHECK(editor.GetHistory().Undo(editor) == 1u);
			CHECK(editor.GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EditorTransaction: a transaction opened inside another joins it")
		{
			Test::EditorTestFixture fixture("EditorTransactionJoin");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			EditorTransaction outer(editor, "Batch");
			static_cast<void>(CreateTrackedEntity(editor, "A"));
			{
				EditorTransaction fixes(editor, "Fix Diagnostics"); // ProjectValidator::Fix inside edit.batch
				CHECK(fixes.IsJoined());
				CHECK(editor.GetTransaction() == &outer);
				static_cast<void>(CreateTrackedEntity(editor, "B"));
				CHECK(fixes.Commit() == 0); // joined: the outer transaction records it
			}
			{
				EditorTransaction abandoned(editor, "Abandoned");
				static_cast<void>(CreateTrackedEntity(editor, "C"));
				REQUIRE(abandoned.Rollback().has_value()); // undoes only its own command
			}
			CHECK(editor.GetScene().GetEntityCount() == 2);
			CHECK(outer.GetCommandCount() == 2);
			const uint64_t undoIndex = outer.Commit();
			CHECK(undoIndex == editor.GetHistory().GetCurrentSequence());
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK(editor.GetHistory().Undo(editor) == 1u);
			CHECK(editor.GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EditorTransaction: a rollback that meets a failing Undo records what stays applied")
		{
			Test::EditorTestFixture fixture("EditorTransactionRollbackFailure");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				EditorTransaction transaction(editor, "Batch");
				static_cast<void>(CreateTrackedEntity(editor, "A"));
				REQUIRE(editor.Execute(CreateScope<UndoFailsCommand>()).has_value());
				static_cast<void>(CreateTrackedEntity(editor, "B"));
				const Status rolledBack = transaction.Rollback();
				REQUIRE_FALSE(rolledBack.has_value());
				CHECK(rolledBack.error().GetCode() == ErrorCode::Io);
				CHECK(rolledBack.error().ToString().contains("rolling back 'Batch'"));
			}
			// B was undone; the failing command and A stay applied, recorded as one undo step.
			CHECK(editor.GetScene().GetEntityCount() == 1);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK(editor.GetHistory().GetUndoLabel() == "Batch (partially rolled back)");
		}

		TEST_CASE("EditorTransaction: a rollback failing in the destructor is logged and recorded")
		{
			Test::EditorTestFixture fixture("EditorTransactionDestructorFailure");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				const Test::ExpectLog expected(LogLevel::Error, "Rolling back the abandoned transaction 'Batch' failed");
				EditorTransaction transaction(editor, "Batch");
				REQUIRE(editor.Execute(CreateScope<UndoFailsCommand>()).has_value());
			}
			CHECK(editor.GetTransaction() == nullptr);
			CHECK(editor.GetHistory().GetUndoLabel() == "Batch (partially rolled back)");
		}

		TEST_CASE("EditorTransaction: destroying an uncommitted transaction rolls back")
		{
			Test::EditorTestFixture fixture("EditorTransactionRollback");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				EditorTransaction transaction(editor, "Abandoned");
				static_cast<void>(CreateTrackedEntity(editor, "A"));
				static_cast<void>(CreateTrackedEntity(editor, "B"));
			}
			CHECK(editor.GetScene().GetEntityCount() == 0);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("EditorDryRunScope: scene mutations leave the revision, history and selection unchanged")
		{
			Test::EditorTestFixture fixture("EditorDryRunScene");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const UUID selected = CreateTrackedEntity(editor, "Selected");
			editor.SetSelection({ selected });
			const uint64_t revision = editor.GetRevision();
			const size_t undoCount = editor.GetHistory().GetUndoCount();
			UUID wouldBe;
			{
				Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE_MESSAGE(dryRun.has_value(), dryRun.error().ToString());
				CHECK(editor.IsDryRun());
				wouldBe = CreateTrackedEntity(editor, "WouldBe");
				CHECK(editor.GetScene().GetEntityCount() == 2);
			}
			CHECK_FALSE(editor.IsDryRun());
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == undoCount);
			CHECK(editor.GetScene().GetEntityCount() == 1);
			CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ selected });
			// The id the dry run reported is the one the real call creates next.
			CHECK(CreateTrackedEntity(editor, "Real") == wouldBe);
		}

		TEST_CASE("EditorDryRunScope: file writes stay in the overlay and record no provenance or events")
		{
			Test::EditorTestFixture fixture("EditorDryRunFiles");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const uint64_t nextEvent = fixture.GetEngine().GetEventLog().GetNextSeq();
			const ProvenanceRecorder* provenance = editor.GetProvenance();
			REQUIRE(provenance != nullptr);
			const size_t provenanceEntries = provenance->GetEntries().size();
			{
				Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun.has_value());
				REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Materials/Red.material"), AsEditorBytes("{}")).has_value());
				CHECK(editor.GetVfs().Exists(MakeEditorPath("project://Assets/Materials/Red.material")));
				editor.AppendEvent(EngineEvent{ .Seq = 0, .Tick = std::nullopt, .Type = EngineEventType::EntityCreated, .Id = UUID(1), .Path = {}, .Name = "x", .Message = {}, .Dirty = false });
				CHECK((*dryRun)->GetChangedPaths() == std::vector<std::string>{ "Assets/Materials/Red.material" });
			}
			CHECK_FALSE(FileSystem::Exists(fixture.GetProjectRoot() / "Assets/Materials/Red.material"));
			REQUIRE(editor.GetProvenance() != nullptr);
			CHECK(editor.GetProvenance()->GetEntries().size() == provenanceEntries);
			CHECK(fixture.GetEngine().GetEventLog().GetNextSeq() == nextEvent);
		}

		TEST_CASE("EditorContext: RequestShutdown keeps the first exit code")
		{
			Test::EditorTestFixture fixture("EditorShutdown");
			fixture.GetEditor().RequestShutdown(0);
			fixture.GetEditor().RequestShutdown(1);
			CHECK(fixture.GetEditor().GetShutdownRequest() == 0);
		}

		TEST_CASE("EditorContext: a read-only project refuses scene edits and rolls them back")
		{
			Test::EditorTestFixture fixture("EditorReadOnlyEdit");
			fixture.CreateAndOpenProject();
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			fixture.GetEditor().CloseProject();
			Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(projectFile,
				{ .ReadOnly = true, .StrictUnknowns = false, .ReadOnlyCacheDirectory = fixture.GetDirectory() / "ReadOnlyCache" },
				fixture.GetEngine().GetTypeRegistry());
			REQUIRE(project.has_value());
			EditorContext& editor = fixture.GetEditor();
			REQUIRE(editor.OpenProject(std::move(*project)).has_value());
			editor.SetScene(editor.CreateScene("Viewed"), std::nullopt);

			Result<uint64_t> committed = MakeError(ErrorCode::Unknown, "not committed");
			{
				SceneEdit edit(editor, "Create 'Refused'");
				static_cast<void>(editor.GetScene().CreateEntity("Refused"));
				committed = edit.Commit();
			}
			REQUIRE_FALSE(committed.has_value());
			CHECK(committed.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK(editor.GetScene().GetEntityCount() == 0);
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK_FALSE(editor.IsSceneDirty());
		}

		TEST_CASE("EditorContext: a repaired scene stays dirty until it is saved")
		{
			Test::EditorTestFixture fixture("EditorRepairedDirty");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			editor.SetScene(editor.CreateScene("Repaired"), MakeEditorPath("project://Assets/Scenes/Repaired.scene"), true);
			CHECK(editor.IsSceneDirty());
			static_cast<void>(CreateTrackedEntity(editor, "A"));
			CHECK(editor.GetHistory().Undo(editor) == 1u);
			CHECK(editor.IsSceneDirty()); // undoing the edit does not undo the repair
			editor.MarkSceneSaved(MakeEditorPath("project://Assets/Scenes/Saved.scene"));
			CHECK_FALSE(editor.IsSceneDirty());
			REQUIRE(editor.GetScenePath().has_value());
			CHECK(editor.GetScenePath()->GetPath() == "Assets/Scenes/Saved.scene");
		}

		TEST_CASE("EditorContext: WriteProjectFile creates missing folders")
		{
			Test::EditorTestFixture fixture("EditorWriteFolders");
			fixture.CreateAndOpenProject();
			REQUIRE(fixture.GetEditor().WriteProjectFile(MakeEditorPath("project://Assets/Levels/World1/Notes.txt"), AsEditorBytes("notes")).has_value());
			const Result<std::string> text = FileSystem::ReadText(fixture.GetProjectRoot() / "Assets/Levels/World1/Notes.txt");
			REQUIRE(text.has_value());
			CHECK(*text == "notes");
		}

		TEST_CASE("EditorContext: SetScene reports a SceneOpened event with the scene path")
		{
			Test::EditorTestFixture fixture("EditorSceneOpenedEvent");
			fixture.CreateAndOpenProject();
			const uint64_t cursor = fixture.GetEngine().GetEventLog().GetNextSeq();
			fixture.CreateAndOpenScene("Assets/Scenes/Level.scene");
			const EventReadResult events = fixture.GetEngine().GetEventLog().Read(cursor);
			REQUIRE(events.Events.size() == 1);
			CHECK(events.Events[0].Type == EngineEventType::SceneOpened);
			CHECK(events.Events[0].Path == "Assets/Scenes/Level.scene");
		}

		TEST_CASE("EditorDryRunScope: settings changes and scene replacements are discarded")
		{
			Test::EditorTestFixture fixture("EditorDryRunSettings");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const std::string title = editor.GetProject().GetSettings().Window.Title;
			const Result<std::string> fileBefore = FileSystem::ReadText(fixture.GetProjectRoot() / "TestProject.eproj");
			REQUIRE(fileBefore.has_value());
			const Result<Json> patch = JsonReader::Parse(R"({"Window":{"Title":"Dry"}})");
			REQUIRE(patch.has_value());
			{
				Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun.has_value());
				Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Set Title");
				REQUIRE(command.has_value());
				CHECK(editor.Execute(std::move(*command)) == 0u);
				CHECK(editor.GetProject().GetSettings().Window.Title == "Dry");
				CHECK((*dryRun)->GetChangedPaths() == std::vector<std::string>{ "TestProject.eproj" });
				editor.SetScene(editor.CreateScene("Replaced"), std::nullopt);
				CHECK(editor.GetScene().GetName() == "Replaced");
			}
			CHECK(editor.GetProject().GetSettings().Window.Title == title);
			CHECK(FileSystem::ReadText(fixture.GetProjectRoot() / "TestProject.eproj") == *fileBefore);
			CHECK(editor.GetScene().GetName() == "Main");
			REQUIRE(editor.GetScenePath().has_value());
			CHECK(editor.GetScenePath()->GetPath() == "Assets/Scenes/Main.scene");
			CHECK_FALSE(editor.GetHistory().CanUndo());
		}

		TEST_CASE("EditorDryRunScope: a transaction inside a dry run records into the sandbox history")
		{
			Test::EditorTestFixture fixture("EditorDryRunTransaction");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun.has_value());
				EditorTransaction transaction(editor, "Batch");
				static_cast<void>(CreateTrackedEntity(editor, "A"));
				static_cast<void>(CreateTrackedEntity(editor, "B"));
				CHECK(transaction.Commit() == 0);
				CHECK(editor.GetHistory().GetUndoCount() == 1);
				CHECK(editor.IsSceneDirty());
			}
			CHECK(editor.GetScene().GetEntityCount() == 0);
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK_FALSE(editor.IsSceneDirty());
		}

		TEST_CASE("EditorDryRunScope: needs a project and keeps a dirty scene dirty")
		{
			Test::EditorTestFixture fixture("EditorDryRunDirty");
			{
				const Result<Scope<EditorDryRunScope>> launcher = EditorDryRunScope::Begin(fixture.GetEditor());
				REQUIRE_FALSE(launcher.has_value());
				CHECK(launcher.error().GetCode() == ErrorCode::InvalidState);
			}
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			static_cast<void>(CreateTrackedEntity(editor, "Unsaved"));
			const Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
			REQUIRE(dryRun.has_value());
			CHECK(editor.IsSceneDirty());
			CHECK_FALSE(editor.GetHistory().CanUndo());
		}

		TEST_CASE("HotReload: a changed open scene raises SceneChangedOnDisk and is not reloaded" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SceneChangedOnDisk");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			editor.Update(0.0);
			{
				SceneEdit edit(editor, "Add Unsaved");
				static_cast<void>(editor.GetScene().CreateEntity("Unsaved"));
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(editor.IsSceneDirty());
			const uint64_t revision = editor.GetRevision();

			// Another program rewrites the scene file (a git checkout): polled, debounced, never applied to the open scene. The
			// content differs from what the editor wrote (another seed, one entity), and so does its size, so detection never
			// depends on the file system's modification-time resolution.
			const std::string external(ExternalMainScene);
			REQUIRE(FileSystem::WriteFileAtomic(editor.GetProject().GetRoot() / "Assets/Scenes/Main.scene", AsBytes(external)).has_value());
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
			{
				editor.Update(seconds);
				static_cast<void>(editor.GetEngine().GetMainThreadQueue().Drain());
			}
			CHECK(editor.IsSceneChangedOnDisk());
			CHECK(editor.GetScene().FindEntityByPath("/Unsaved").IsValid());
			CHECK(editor.GetRevision() == revision);
			const EngineEventType types[] = { EngineEventType::SceneChangedOnDisk };
			const EventReadResult events = editor.GetEngine().GetEventLog().Read(0, types, 10);
			REQUIRE(events.Events.size() == 1);
			CHECK(events.Events.front().Path == "Assets/Scenes/Main.scene");
			CHECK(events.Events.front().Dirty);

			// Saving (or reloading) the scene clears the flag.
			REQUIRE(editor.WriteProjectFile(*editor.GetScenePath(), AsBytes(external)).has_value());
			editor.MarkSceneSaved(*editor.GetScenePath());
			CHECK_FALSE(editor.IsSceneChangedOnDisk());
		}

		TEST_CASE("EditorContext: an external scene change found by a refresh raises SceneChangedOnDisk once" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SceneChangedRefresh");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			editor.Update(0.0);
			const AssetHandle scene = editor.GetAssets().Resolve("Assets/Scenes/Main.scene").value_or(AssetHandle());
			REQUIRE(editor.GetAssets().Load(scene).has_value());
			REQUIRE(editor.GetAssets().GetVersion(scene) == 1);

			// An external rewrite, seen first by the refresh every path-taking automation call makes (here
			// project.refreshAssets), before any poll: race rule 3 holds at once.
			REQUIRE(FileSystem::WriteFileAtomic(editor.GetProject().GetRoot() / "Assets/Scenes/Main.scene", AsEditorBytes(ExternalMainScene)).has_value());
			Result<AssetRefreshReport> refreshed = editor.GetAssets().Refresh();
			REQUIRE_MESSAGE(refreshed.has_value(), refreshed.error().ToString());
			CHECK(refreshed->Changed == std::vector<AssetHandle>{ scene });
			CHECK(editor.IsSceneChangedOnDisk());
			CHECK(CountEditorEvents(editor, EngineEventType::SceneChangedOnDisk) == 1);

			// The refresh marked the path known on the watcher: later polls report nothing, so the change is reimported and
			// published once.
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
			{
				editor.Update(seconds);
				static_cast<void>(editor.GetEngine().GetMainThreadQueue().Drain());
			}
			editor.GetAssets().WaitIdle();
			CHECK(editor.GetAssets().GetVersion(scene) == 2);
			CHECK(CountEditorEvents(editor, EngineEventType::AssetReloaded) == 1);
			CHECK(CountEditorEvents(editor, EngineEventType::SceneChangedOnDisk) == 1);
		}

		TEST_CASE("EditorContext: the asset manager's own .meta writes keep provenance in step" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ManagerProvenance");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			// A glTF with its closure, written by another program; the refresh and the import write the .meta files
			// themselves (new sources, dependency metas, the sub-asset rewrite), never through WriteProjectFile.
			for (const char* file : { "Textured.gltf", "Textured.bin", "Textures/Checker.png" })
			{
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(std::string("Assets/Gltf/") + file));
				REQUIRE(bytes.has_value());
				const VfsPath path = MakeEditorPath(std::string("project://Assets/Models/") + file);
				REQUIRE(editor.GetVfs().CreateDirectories(path.GetParent()).has_value());
				REQUIRE(editor.GetVfs().WriteFileAtomic(path, *bytes).has_value());
			}
			editor.SetWriteAttribution(WriteAttribution{
				.Method = "project.refreshAssets",
				.RequestId = VariantValue(Json(7)),
				.Client = "test",
				.TranscriptLine = 3,
			});
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const AssetHandle gltf = editor.GetAssets().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
			REQUIRE(editor.GetAssets().Load(gltf).has_value());
			editor.SetWriteAttribution(std::nullopt);

			const ProvenanceRecorder* provenance = editor.GetProvenance();
			REQUIRE(provenance != nullptr);
			for (const char* meta : { "Assets/Models/Textured.gltf.meta", "Assets/Models/Textured.bin.meta", "Assets/Models/Textures/Checker.png.meta" })
			{
				CAPTURE(std::string(meta));
				const ProvenanceEntry* entry = provenance->Find(meta);
				REQUIRE(entry != nullptr);
				CHECK(entry->Method == "project.refreshAssets");
			}
			// §13.12 rule 1: every recorded hash is the hash of the file as it is now (the .meta rewritten with the glTF's
			// sub-assets included).
			for (const ProvenanceEntry& entry : provenance->GetEntries())
			{
				CAPTURE(entry.Path);
				Result<Buffer> bytes = editor.GetVfs().ReadFile(MakeEditorPath("project://" + entry.Path));
				REQUIRE(bytes.has_value());
				CHECK(XXH64(*bytes) == entry.Hash);
			}
		}

		TEST_CASE("EditorContext: the asset manager is injected into the engine context and opened with the project" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("EditorAssets");
			EditorContext& editor = fixture.GetEditor();
			CHECK(fixture.GetEngine().GetAssetManager() == &editor.GetAssets());
			CHECK_FALSE(editor.GetAssets().HasProject());
			fixture.CreateAndOpenProject();
			CHECK(editor.GetAssets().HasProject());
			editor.CloseProject();
			CHECK_FALSE(editor.GetAssets().HasProject());
		}

		TEST_CASE("EditorContext: moving and removing project files keeps provenance in step" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("EditorMoveRemove");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const VfsPath from = VfsPath::Create("project", "Assets/Red.material").value_or(VfsPath());
			const VfsPath to = VfsPath::Create("project", "Assets/Materials/Red.material").value_or(VfsPath());
			REQUIRE(editor.WriteProjectFile(from, AsBytes(std::string_view(R"({"Format": "Material", "Version": 1})"))).has_value());
			REQUIRE(editor.MoveProjectFile(from, to).has_value());
			CHECK(editor.GetProvenance()->Find("Assets/Red.material") == nullptr);
			CHECK(editor.GetProvenance()->Find("Assets/Materials/Red.material") != nullptr);
			REQUIRE(editor.RemoveProjectFile(to).has_value());
			CHECK(editor.GetProvenance()->Find("Assets/Materials/Red.material") == nullptr);
			CHECK_FALSE(editor.GetVfs().Exists(to));
			REQUIRE(editor.CreateProjectDirectory(VfsPath::Create("project", "Assets/Empty").value_or(VfsPath())).has_value());
			CHECK(editor.GetVfs().Exists(VfsPath::Create("project", "Assets/Empty").value_or(VfsPath())));
		}
	}

}
