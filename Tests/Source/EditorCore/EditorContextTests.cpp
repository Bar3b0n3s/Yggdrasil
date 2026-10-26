#include "TestsPCH.h"

#include "EditorCore/EditorContext.h"

#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptOverrideProject.h"
#include "Support/StrictnessEditor.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

	// A prefab of a nesting graph written by hand, as a file can hold one: `name`.prefab (asset `handle`, root `rootID`)
	// whose root holds one child, an instance of the prefab `nested` (whose root is `nestedPrefabRoot`) rooted at `innerID`,
	// written with its .meta through the editor's write path.
	static void WriteNestingPrefab(EditorContext& editor, std::string_view name, AssetHandle handle, UUID rootID, UUID innerID, AssetHandle nested,
		UUID nestedPrefabRoot)
	{
		constexpr std::string_view Transform = R"("Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] })";
		const std::string document = std::format(R"({{
	"Format": "Prefab", "Version": 1, "Name": "{0}", "Root": "{1}",
	"ComponentVersions": {{ "Transform": 1, "Prefab": 2, "PrefabLink": 1 }},
	"Entities": [
		{{ "ID": "{1}", "Name": "{0}", "Parent": null, "Active": true, "Tags": [], "Components": {{ {5} }} }},
		{{ "ID": "{2}", "Name": "Inner", "Parent": "{1}", "Active": true, "Tags": [], "Components": {{ {5},
			"Prefab": {{ "Prefab": "{3}", "Overrides": [] }},
			"PrefabLink": {{ "PrefabEntityID": "{4}", "InstanceRoot": "{2}", "OuterPrefabEntityID": null }} }} }}
	]
}}
)",
			name, rootID.ToString(), innerID.ToString(), nested.ToString(), nestedPrefabRoot.ToString(), Transform);
		const std::string meta = std::format(
			R"({{ "Format": "AssetMeta", "Version": 1, "Handle": "{}", "Type": "Prefab", "Importer": "Prefab", "ImporterVersion": 1, "Settings": null, "SubAssets": [] }})",
			handle.ToString());
		const std::string path = std::format("project://Assets/Prefabs/{}.prefab", name);
		REQUIRE(editor.WriteProjectFile(MakeEditorPath(path), AsEditorBytes(document)).has_value());
		REQUIRE(editor.WriteProjectFile(MakeEditorPath(path + ".meta"), AsEditorBytes(meta)).has_value());
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

		void WriteStrictnessFile(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, AsBytes(text), { .KeepBackup = false }).has_value());
		}

		// A prefab whose root carries "Hovercraft", a component no registry knows.
		constexpr std::string_view UnknownComponentPrefab = R"({
	"Format": "Prefab",
	"Version": 1,
	"Name": "Crate",
	"Root": "00000000000c0001",
	"ComponentVersions": {
		"Transform": 1
	},
	"Entities": [
		{
			"ID": "00000000000c0001",
			"Name": "Crate",
			"Parent": null,
			"Active": true,
			"Tags": [],
			"Components": {
				"Transform": {
					"Translation": [0, 0, 0],
					"Rotation": [0, 0, 0, 1],
					"Scale": [1, 1, 1]
				},
				"Hovercraft": {
					"Lift": 3
				}
			}
		}
	]
}
)";

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
		TEST_CASE("EditorContext: a refused close retains the project lock mounts and scene for retry")
		{
			uint32_t calls = 0;
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const auto projectFile = editor.GetProject().GetProjectFile();
			Scene* const scene = &editor.GetScene();
			editor.SetLifecycleCallbacks({ .BeforeProjectClose = [&calls]() -> Status
			{
				++calls;
				if (calls == 1)
					return MakeError(ErrorCode::InvalidState, "writer lease is active");
				return {};
			} });
			const auto refused = editor.CloseProject();
			REQUIRE_FALSE(refused);
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			CHECK(calls == 1);
			CHECK(editor.HasProject());
			CHECK(&editor.GetScene() == scene);
			CHECK(editor.GetVfs().IsMounted("project"));
			CHECK(editor.GetVfs().IsMounted("cache"));
			CHECK_FALSE(ProjectManager::OpenProject(projectFile, {}, editor.GetTypeRegistry()));
			editor.SetLifecycleCallbacks({});
			REQUIRE(editor.CloseProject());
			CHECK_FALSE(editor.HasProject());
			CHECK_FALSE(editor.GetVfs().IsMounted("project"));
			CHECK_FALSE(editor.GetVfs().IsMounted("cache"));
			CHECK(ProjectManager::OpenProject(projectFile, {}, editor.GetTypeRegistry()));
		}

		TEST_CASE("EditorContext: scene-save cleanup runs after the durable save and is excluded from dry runs")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			{
				SceneEdit edit(editor, "Add entity");
				static_cast<void>(editor.GetScene().CreateEntity("SavedEntity"));
				REQUIRE(edit.Commit());
			}
			CHECK(editor.IsSceneDirty());
			uint32_t calls = 0;
			editor.SetLifecycleCallbacks({ .AfterSceneSaved = [&calls, &editor]() -> Status
			{
				++calls;
				CHECK_FALSE(editor.IsSceneDirty());
				REQUIRE(editor.GetScenePath());
				const auto saved = editor.GetVfs().ReadText(*editor.GetScenePath());
				REQUIRE(saved);
				CHECK(saved->contains("SavedEntity"));
				return MakeError(ErrorCode::Io, "cannot remove recovery generation");
			} });
			const auto path = MakeEditorPath("project://Assets/Scenes/Saved.scene");
			const auto saved = Utils::SaveOpenScene(editor, path);
			REQUIRE_FALSE(saved);
			CHECK(saved.error().GetCode() == ErrorCode::Io);
			CHECK(saved.error().ToString().contains("the scene was saved"));
			CHECK(calls == 1);
			CHECK_FALSE(editor.IsSceneDirty());
			{
				auto dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun);
				REQUIRE(editor.MarkSceneSaved(path));
				CHECK(calls == 1);
			}
			editor.SetLifecycleCallbacks({});
			REQUIRE(Utils::SaveOpenScene(editor, path));
		}

		TEST_CASE("EditorContext: denying agent mutations rolls back edits and rejects every project write")
		{
			Test::EditorTestFixture fixture("EditorAgentPolicy");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const VfsPath source = MakeEditorPath("project://Assets/source.txt");
			const VfsPath moved = MakeEditorPath("project://Assets/moved.txt");
			const VfsPath folder = MakeEditorPath("project://Assets/DeniedFolder");
			REQUIRE(editor.WriteProjectFile(source, AsEditorBytes("original")).has_value());
			editor.SetAgentMutationsDenied(true);
			CHECK(editor.AreAgentMutationsDenied());
			editor.SetWriteAttribution(WriteAttribution{ .Method = "project.validate" });
			const uint64_t count = editor.GetScene().GetEntityCount();
			{
				SceneEdit edit(editor, "Optional fix");
				const UUID id = editor.GetScene().CreateEntity("Denied").GetUUID();
				const auto result = edit.Commit();
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::PermissionDenied);
				CHECK_FALSE(editor.GetScene().FindEntityByID(id).IsValid());
			}
			CHECK(editor.GetScene().GetEntityCount() == count);
			CHECK_FALSE(editor.GetHistory().CanUndo());
			const Status results[] = {
				editor.WriteProjectFile(source, AsEditorBytes("changed")),
				editor.MoveProjectFile(source, moved),
				editor.RemoveProjectFile(source),
				editor.CreateProjectDirectory(folder),
			};
			for (const Status& result : results)
			{
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::PermissionDenied);
			}
			CHECK(editor.GetVfs().ReadText(source) == "original");
			CHECK_FALSE(editor.GetVfs().Exists(moved));
			CHECK_FALSE(editor.GetVfs().Exists(folder));
			editor.SetWriteAttribution(std::nullopt);
			CHECK(CreateTrackedEntity(editor, "Human").IsValid());
			REQUIRE(editor.WriteProjectFile(source, AsEditorBytes("human")).has_value());
			editor.SetWriteAttribution(WriteAttribution{ .Method = "entity.create" });
			editor.SetAgentMutationsDenied(false);
			CHECK(CreateTrackedEntity(editor, "AllowedAgent").IsValid());
		}

		TEST_CASE("EditorContext: denying agent mutations still allows disposable dry runs")
		{
			Test::EditorTestFixture fixture("EditorPolicyDryRun");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const VfsPath path = MakeEditorPath("project://Assets/sandbox.txt");
			const uint64_t count = editor.GetScene().GetEntityCount();
			editor.SetAgentMutationsDenied(true);
			editor.SetWriteAttribution(WriteAttribution{ .Method = "edit.batch" });
			{
				auto dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun.has_value());
				CHECK(CreateTrackedEntity(editor, "Sandboxed").IsValid());
				REQUIRE(editor.WriteProjectFile(path, AsEditorBytes("sandbox")).has_value());
				CHECK(editor.GetVfs().Exists(path));
			}
			CHECK(editor.GetScene().GetEntityCount() == count);
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK_FALSE(editor.GetVfs().Exists(path));
			CHECK(editor.AreAgentMutationsDenied());
		}

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

			REQUIRE(editor.CloseProject());
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
			REQUIRE(fixture.GetEditor().CloseProject());

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
			REQUIRE(fixture.GetEditor().CloseProject());
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
			REQUIRE(editor.MarkSceneSaved(MakeEditorPath("project://Assets/Scenes/Saved.scene")));
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

		TEST_CASE("HotReload: a changed open scene raises SceneChangedOnDisk and is not reloaded")
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
			REQUIRE(editor.MarkSceneSaved(*editor.GetScenePath()));
			CHECK_FALSE(editor.IsSceneChangedOnDisk());
		}

		TEST_CASE("EditorContext: an external scene change found by a refresh raises SceneChangedOnDisk once")
		{
			Test::EditorTestFixture fixture("SceneChangedRefresh");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			editor.Update(0.0);
			// The scene was written through the editor without a .meta: it is registered by the next refresh (ADR 0010
			// decision 11), which the editor's own write never echoes.
			Result<AssetRefreshReport> registered = editor.GetAssets().Refresh();
			REQUIRE_MESSAGE(registered.has_value(), registered.error().ToString());
			CHECK(registered->CreatedMetas == std::vector<VfsPath>{ MakeEditorPath("project://Assets/Scenes/Main.scene.meta") });
			CHECK_FALSE(editor.IsSceneChangedOnDisk());
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

		TEST_CASE("HotReload: the backup an editor write leaves is no external change")
		{
			Test::EditorTestFixture fixture("EditorWriteBackup");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			// A material that fails to import beside the one the editor rewrites: a failed import is retried by any external
			// change in its folder, so an echoed backup would import it again at every save.
			const VfsPath broken = MakeEditorPath("project://Assets/Materials/Broken.material");
			const VfsPath red = MakeEditorPath("project://Assets/Materials/Red.material");
			REQUIRE(editor.GetVfs().CreateDirectories(broken.GetParent()).has_value());
			REQUIRE(editor.GetVfs().WriteFileAtomic(broken, AsEditorBytes("not a material")).has_value());
			REQUIRE(editor.GetVfs().WriteFileAtomic(red, AsEditorBytes(R"({"Format": "Material", "Version": 1})")).has_value());
			const Test::ExpectLog importFailure(LogLevel::Error, "ASSET_IMPORT_FAILED Assets/Materials/Broken.material");
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const size_t failures = CountEditorEvents(editor, EngineEventType::AssetImportFailed);
			REQUIRE(failures == 1);
			editor.Update(0.0);

			// project:// keeps the replaced content as Red.material.bak (§4.10): written by the editor, never an external
			// change, neither for the polls nor for a refresh.
			for (const std::string_view metallic : { "1", "0.5" })
			{
				const std::string text = std::format(R"({{"Format": "Material", "Version": 1, "Metallic": {}}})", metallic);
				REQUIRE(editor.WriteProjectFile(red, AsEditorBytes(text)).has_value());
			}
			CHECK(editor.GetVfs().Exists(MakeEditorPath("project://Assets/Materials/Red.material.bak")));
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
			{
				editor.Update(seconds);
				static_cast<void>(editor.GetEngine().GetMainThreadQueue().Drain());
			}
			Result<AssetRefreshReport> refreshed = editor.GetAssets().Refresh();
			REQUIRE_MESSAGE(refreshed.has_value(), refreshed.error().ToString());
			editor.GetAssets().WaitIdle();
			CHECK(CountEditorEvents(editor, EngineEventType::AssetImportFailed) == failures);
		}

		TEST_CASE("EditorContext: the asset manager's own .meta writes keep provenance in step")
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

		TEST_CASE("EditorContext: a glTF's .meta rewritten with its sub-assets after asset.import returned is attributed to asset.import")
		{
			Test::EditorTestFixture fixture("DeferredImportProvenance");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			EditorAssetManager& assets = editor.GetAssets();
			// A request like asset.import (its request id and transcript line as the bridge sends them) writes the glTF, its
			// closure files and the glTF's own .meta, then starts the import (ReimportAsync, as the method's pending operation
			// starts it). Unlike the real handler, it writes no closure metas, so the publication writes the closure files'
			// dependency metas as well as the glTF .meta's sub-asset rewrite.
			editor.SetWriteAttribution(WriteAttribution{
				.Method = "asset.import",
				.RequestId = VariantValue(Json(7)),
				.Client = "engine-mcp",
				.TranscriptLine = 12,
			});
			for (const char* file : { "Textured.gltf", "Textured.bin", "Textures/Checker.png" })
			{
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(std::string("Assets/Gltf/") + file));
				REQUIRE(bytes.has_value());
				const VfsPath path = MakeEditorPath(std::string("project://Assets/Models/") + file);
				REQUIRE(editor.WriteProjectFile(path, *bytes).has_value());
			}
			const VfsPath gltf = MakeEditorPath("project://Assets/Models/Textured.gltf");
			const Result<AssetMetadata> metadata = assets.CreateMetadata(gltf);
			REQUIRE_MESSAGE(metadata.has_value(), metadata.error().ToString());
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Models/Textured.gltf.meta"), AsEditorBytes(SerializeAssetMetadata(*metadata)))
					.has_value());
			JobHandle<AssetImportOutcome> imported = assets.ReimportAsync(metadata->Handle);

			// The request returned. With inline jobs the import has run, and its publication waits for the main-thread queue,
			// which the next frame drains outside any request.
			editor.SetWriteAttribution(std::nullopt);
			static_cast<void>(fixture.GetEngine().GetMainThreadQueue().Drain());
			const Result<AssetImportOutcome> outcome = imported.Take();
			REQUIRE_MESSAGE(outcome.has_value(), outcome.error().ToString());
			REQUIRE_FALSE(outcome->SubAssets.empty());

			// The source's .meta, rewritten with its sub-assets by the publication, and the dependency metas of the files the
			// import read: attributed to the request that scheduled the import (§13.12 rule 2), never to "ui".
			const ProvenanceRecorder* provenance = editor.GetProvenance();
			REQUIRE(provenance != nullptr);
			for (const char* meta : { "Assets/Models/Textured.gltf.meta", "Assets/Models/Textured.bin.meta", "Assets/Models/Textures/Checker.png.meta" })
			{
				CAPTURE(std::string(meta));
				const ProvenanceEntry* entry = provenance->Find(meta);
				REQUIRE(entry != nullptr);
				CHECK(entry->Method == "asset.import");
				CHECK(entry->RequestId == VariantValue(Json(7)));
				CHECK(entry->Client == "engine-mcp");
				CHECK(entry->TranscriptLine == std::optional<uint64_t>(12));
				Result<Buffer> bytes = editor.GetVfs().ReadFile(MakeEditorPath(std::string("project://") + meta));
				REQUIRE(bytes.has_value());
				CHECK(XXH64(*bytes) == entry->Hash);
			}
		}

		TEST_CASE("EditorContext: a deferred asset scan writes nothing at open and runs under the attribution of OpenDeferredAssets")
		{
			Test::TempDirectory directory("DeferredAssetScan");
			REQUIRE(FileSystem::CreateDirectories(directory / "UserData").has_value());
			Result<Scope<EngineContext>> engine = EngineContext::Create({ .WorkerCount = 0, .UserDataDirectory = directory / "UserData", .RegisterTypes = &RegisterEditorMethodTypes });
			REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
			Result<Scope<EditorContext>> created = EditorContext::Create(**engine, {
																					   .IdGeneratorState = Test::EditorTestIdState,
																					   .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources" / "Templates" / "Projects",
																					   .ReadOnlyCacheRoot = directory / "ReadOnlyCache",
																					   .HistoryLimits = {},
																					   .EnvironmentBaker = nullptr,
																					   .StrictUnknowns = false,
																					   .DeferAssetScan = true,
																				   });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			// Declared after the engine context, so the editor (its project lock and mounts) goes first.
			Scope<EditorContext> editor = std::move(*created);

			const Result<CreatedProject> project = ProjectManager::CreateProject(
				{
					.Directory = directory / "Deferred",
					.Name = "Deferred",
					.Template = ProjectTemplate::Empty,
					.TemplatesDirectory = editor->GetSpecification().TemplatesDirectory,
				},
				editor->GetTypeRegistry());
			REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
			// A source without a .meta, as an engine change or a hand copy leaves one: the scan writes its .meta. The project's
			// .luaurc is not UTF-8 at first, which the script services refuse (Validation).
			const std::filesystem::path root = project->ProjectFile.parent_path();
			const Result<Buffer> png = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/Textures/Checker.png"));
			REQUIRE(png.has_value());
			REQUIRE(FileSystem::CreateDirectories(root / "Assets" / "Textures").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(root / "Assets" / "Textures" / "Checker.png", *png, { .KeepBackup = false }).has_value());
			const Result<Buffer> luaurc = FileSystem::ReadFile(root / ".luaurc");
			REQUIRE(luaurc.has_value());
			const Buffer invalidLuaurc = { std::byte{ 0xff } };
			REQUIRE(FileSystem::WriteFileAtomic(root / ".luaurc", invalidLuaurc, { .KeepBackup = false }).has_value());
			Result<Scope<LoadedProject>> loaded = ProjectManager::OpenProject(project->ProjectFile, {}, editor->GetTypeRegistry());
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());

			// The open mounts the project and loads its provenance, reads no .luaurc and writes nothing.
			REQUIRE(editor->OpenProject(std::move(*loaded)).has_value());
			CHECK(editor->HasProject());
			CHECK(editor->IsAssetScanDeferred());
			CHECK_FALSE(editor->GetAssets().HasProject());
			CHECK_FALSE(FileSystem::Exists(root / "Assets" / "Textures" / "Checker.png.meta"));

			// project.refreshAssets of --upgrade's cli batch runs the scan.
			editor->SetWriteAttribution(WriteAttribution{
				.Method = "project.refreshAssets",
				.RequestId = VariantValue(Json(1)),
				.Client = "cli",
				.TranscriptLine = 1,
			});
			// Its failure is its own error and leaves the project open with the scan still deferred, so the request's response
			// can still be transcribed through project:// (BatchRunner) and the caller closes the project.
			const Result<AssetRefreshReport> failed = editor->OpenDeferredAssets();
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::Validation);
			CHECK(failed.error().ToString().find(".luaurc") != std::string::npos);
			CHECK(editor->HasProject());
			CHECK(editor->IsAssetScanDeferred());
			CHECK(editor->GetVfs().IsMounted("project"));
			CHECK_FALSE(editor->GetAssets().HasProject());
			CHECK_FALSE(FileSystem::Exists(root / "Assets" / "Textures" / "Checker.png.meta"));

			// Repaired, the scan runs: its .meta write is that request's.
			REQUIRE(FileSystem::WriteFileAtomic(root / ".luaurc", *luaurc, { .KeepBackup = false }).has_value());
			const Result<AssetRefreshReport> scanned = editor->OpenDeferredAssets();
			editor->SetWriteAttribution(std::nullopt);
			REQUIRE_MESSAGE(scanned.has_value(), scanned.error().ToString());
			CHECK_FALSE(editor->IsAssetScanDeferred());
			CHECK(editor->GetAssets().HasProject());
			REQUIRE(scanned->CreatedMetas.size() == 1);
			CHECK(scanned->CreatedMetas[0] == MakeEditorPath("project://Assets/Textures/Checker.png.meta"));
			const ProvenanceEntry* entry = editor->GetProvenance()->Find("Assets/Textures/Checker.png.meta");
			REQUIRE(entry != nullptr);
			CHECK(entry->Method == "project.refreshAssets");
			CHECK(entry->Client == "cli");
			CHECK(entry->TranscriptLine == std::optional<uint64_t>(1));

			// Nothing is deferred any more.
			const Result<AssetRefreshReport> again = editor->OpenDeferredAssets();
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorContext: the asset manager is injected into the engine context and opened with the project")
		{
			Test::EditorTestFixture fixture("EditorAssets");
			EditorContext& editor = fixture.GetEditor();
			CHECK(fixture.GetEngine().GetAssetManager() == &editor.GetAssets());
			CHECK_FALSE(editor.GetAssets().HasProject());
			fixture.CreateAndOpenProject();
			CHECK(editor.GetAssets().HasProject());
			REQUIRE(editor.CloseProject());
			CHECK_FALSE(editor.GetAssets().HasProject());
		}

		// Docs/Decisions/0031-m20-contract.md decision 8 (Roadmap M20 acceptance "cycles are refused"): no editor action adds
		// a nested instance to an existing prefab, so a nesting cycle comes from a file. Here Lamp.prefab is written to hold
		// an instance of itself; the nesting graph names the cycle with PREFAB_CYCLE, and applying an edit of a lamp, whose
		// propagation would rewrite Lamp.prefab from itself forever, is refused and changes nothing.
		TEST_CASE("EditorContext: a prefab update refuses a nesting cycle")
		{
			Test::AutomationFixture setup("PrefabNestingCycle");
			EditorContext& editor = setup.GetEditor();
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Lamp" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/Lamp" } }).has_value());
			const Result<Json> created =
				setup.Call("prefab.create", Json{ { "entity", "/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } });
			REQUIRE(created.has_value());
			const std::optional<UUID> lamp = UUID::FromString(JsonReader((*created)["prefab"]["id"]).ReadString().value_or(std::string()));
			REQUIRE(lamp.has_value());

			// Lamp.prefab with a nested instance of Lamp under its root, expanded as instantiation would expand it.
			const std::filesystem::path file = setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Lamp.prefab";
			const Result<std::string> text = FileSystem::ReadText(file);
			REQUIRE(text.has_value());
			Result<Json> document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			const std::optional<UUID> root = UUID::FromString(JsonReader((*document)["Root"]).ReadString().value_or(std::string()));
			REQUIRE(root.has_value());
			const UUID nested(0x5151);
			const Json entities = (*document)["Entities"];
			for (const Json& entity : entities)
			{
				const std::optional<UUID> id = UUID::FromString(JsonReader(entity["ID"]).ReadString().value_or(std::string()));
				REQUIRE(id.has_value());
				Json copy = entity;
				const bool isRoot = *id == *root;
				copy["ID"] = (isRoot ? nested : PrefabInstantiator::DeriveInstanceID(nested, *id)).ToString();
				if (isRoot)
					copy["Parent"] = root->ToString();
				else
				{
					const std::optional<UUID> parent = UUID::FromString(JsonReader(entity["Parent"]).ReadString().value_or(std::string()));
					REQUIRE(parent.has_value());
					copy["Parent"] = (*parent == *root ? nested : PrefabInstantiator::DeriveInstanceID(nested, *parent)).ToString();
					copy["Components"]["PrefabLink"] = Json{ { "PrefabEntityID", id->ToString() }, { "InstanceRoot", nested.ToString() }, { "OuterPrefabEntityID", nullptr } };
				}
				if (isRoot)
				{
					copy["Components"]["Prefab"] = Json{ { "Prefab", lamp->ToString() }, { "Overrides", Json::array() } };
					copy["Components"]["PrefabLink"] = Json{ { "PrefabEntityID", root->ToString() }, { "InstanceRoot", nested.ToString() }, { "OuterPrefabEntityID", nullptr } };
				}
				(*document)["Entities"].push_back(std::move(copy));
			}
			(*document)["ComponentVersions"]["Prefab"] = 2;
			(*document)["ComponentVersions"]["PrefabLink"] = 1;
			const std::string written = document->dump(1, '\t') + "\n";
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Prefabs/Lamp.prefab"), AsEditorBytes(written)).has_value());
			editor.GetAssets().WaitIdle();

			const Result<std::vector<AssetHandle>> nesting = editor.GetNestingPrefabs(std::span(&*lamp, 1));
			REQUIRE_FALSE(nesting.has_value());
			CHECK(nesting.error().GetCode() == ErrorCode::Validation);
			CHECK(nesting.error().ToString().contains("PREFAB_CYCLE"));
			CHECK(nesting.error().ToString().contains("Assets/Prefabs/Lamp.prefab"));

			REQUIRE(setup.Call("entity.update", Json{ { "entity", "/Lamp/Bulb" }, { "name", "Globe" } }).has_value());
			const size_t steps = editor.GetHistory().GetUndoCount();
			const Result<Json> applied = setup.Call("prefab.apply", Json{ { "instance", "/Lamp" } });
			REQUIRE_FALSE(applied.has_value());
			CHECK(applied.error().GetCode() == ErrorCode::Validation);
			CHECK(applied.error().ToString().contains("PREFAB_CYCLE"));
			CHECK(editor.GetHistory().GetUndoCount() == steps);
			const Result<std::string> after = FileSystem::ReadText(file);
			REQUIRE(after.has_value());
			CHECK(*after == written);
		}

		// Docs/Decisions/0031-m20-contract.md decision 8 (Roadmap M20 acceptance "inner edits propagate"): the prefabs nesting
		// a prefab, directly or through others, come from the import dependencies in nesting order, each after the prefabs it
		// nests; the nesting rewrite writes each with its nested instances rebuilt from the prefab's new version, as one
		// undoable command whose undo restores every file; and the prefab update rebuilds the outermost instances of every
		// prefab nesting the changed one, their nested instances with them.
		TEST_CASE("EditorContext: a prefab's new version is written into the prefabs nesting it in nesting order, as one undoable command")
		{
			Test::AutomationFixture setup("PrefabNestingUpdate");
			EditorContext& editor = setup.GetEditor();
			const auto call = [&setup](std::string_view method, const Json& params)
			{
				Result<Json> result = setup.Call(method, params);
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				return std::move(*result);
			};
			const auto handleOf = [](const Json& created)
			{
				const std::optional<UUID> id = UUID::FromString(JsonReader(created["prefab"]["id"]).ReadString().value_or(std::string()));
				REQUIRE(id.has_value());
				return AssetHandle(*id);
			};
			call("entity.create", Json{ { "name", "House" } });
			call("entity.create", Json{ { "name", "Room" }, { "parent", "/House" } });
			call("entity.create", Json{ { "name", "Lamp" }, { "parent", "/House/Room" } });
			call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/House/Room/Lamp" }, { "components", Json{ { "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Cube" } } } } } });
			const AssetHandle lamp = handleOf(call("prefab.create", Json{ { "entity", "/House/Room/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } }));
			const AssetHandle room = handleOf(call("prefab.create", Json{ { "entity", "/House/Room" }, { "path", "Assets/Prefabs/Room.prefab" }, { "replaceWithInstance", true } }));
			const AssetHandle house = handleOf(call("prefab.create", Json{ { "entity", "/House" }, { "path", "Assets/Prefabs/House.prefab" }, { "replaceWithInstance", true } }));
			const AssetHandle shed = handleOf(call("prefab.create", Json{ { "entity", "/House/Room/Lamp/Bulb" }, { "path", "Assets/Prefabs/Shed.prefab" } }));

			const AssetHandle lampOnly[] = { lamp };
			const Result<std::vector<AssetHandle>> nesting = editor.GetNestingPrefabs(lampOnly);
			REQUIRE_MESSAGE(nesting.has_value(), nesting.error().ToString());
			CHECK(*nesting == std::vector<AssetHandle>{ room, house });
			const AssetHandle roomOnly[] = { room };
			const Result<std::vector<AssetHandle>> nestingRoom = editor.GetNestingPrefabs(roomOnly);
			REQUIRE(nestingRoom.has_value());
			CHECK(*nestingRoom == std::vector<AssetHandle>{ house });
			const AssetHandle shedOnly[] = { shed };
			const Result<std::vector<AssetHandle>> nothing = editor.GetNestingPrefabs(shedOnly);
			REQUIRE(nothing.has_value());
			CHECK(nothing->empty());
			const AssetHandle unknown[] = { AssetHandle(0x1234567890abcdefull) };
			const Result<std::vector<AssetHandle>> missing = editor.GetNestingPrefabs(unknown);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);

			// Lamp.prefab gets a new version outside the nesting rewrite: its bulb casts no shadows.
			const std::filesystem::path root = setup.GetEditorFixture().GetProjectRoot();
			const Result<std::string> lampText = FileSystem::ReadText(root / "Assets/Prefabs/Lamp.prefab");
			REQUIRE(lampText.has_value());
			std::string edited = *lampText;
			const size_t shadows = edited.find("\"CastShadows\": true");
			REQUIRE(shadows != std::string::npos);
			edited.replace(shadows, std::string_view("\"CastShadows\": true").size(), "\"CastShadows\": false");
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Prefabs/Lamp.prefab"), AsEditorBytes(edited)).has_value());
			REQUIRE(editor.GetAssets().Reimport(lamp).has_value());
			const Result<std::string> roomBefore = FileSystem::ReadText(root / "Assets/Prefabs/Room.prefab");
			const Result<std::string> houseBefore = FileSystem::ReadText(root / "Assets/Prefabs/House.prefab");
			REQUIRE(roomBefore.has_value());
			REQUIRE(houseBefore.has_value());

			Result<NestingPrefabUpdate> rewrite = editor.CreateNestingPrefabUpdateCommand(lampOnly);
			REQUIRE_MESSAGE(rewrite.has_value(), rewrite.error().ToString());
			REQUIRE(rewrite->Rewrite != nullptr);
			CHECK(rewrite->Prefabs == std::vector<AssetHandle>{ room, house });
			REQUIRE(editor.Execute(std::move(rewrite->Rewrite)).has_value());
			// Rewritten from the same version again, no document changes: no command, and no prefab named.
			const Result<NestingPrefabUpdate> again = editor.CreateNestingPrefabUpdateCommand(lampOnly);
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			CHECK(again->Rewrite == nullptr);
			CHECK(again->Prefabs.empty());
			const Result<std::string> roomAfter = FileSystem::ReadText(root / "Assets/Prefabs/Room.prefab");
			const Result<std::string> houseAfter = FileSystem::ReadText(root / "Assets/Prefabs/House.prefab");
			REQUIRE(roomAfter.has_value());
			REQUIRE(houseAfter.has_value());
			CHECK(roomAfter->contains("\"CastShadows\": false"));
			CHECK(houseAfter->contains("\"CastShadows\": false"));

			// The update rebuilds the house in the open scene, its room and lamp with it.
			const Entity bulb = editor.GetScene().FindEntityByPath("/House/Room/Lamp/Bulb");
			REQUIRE(bulb.IsValid());
			CHECK(bulb.GetComponent<MeshRendererComponent>().CastShadows);
			Result<Scope<Command>> update = editor.CreatePrefabUpdateCommand(lampOnly);
			REQUIRE_MESSAGE(update.has_value(), update.error().ToString());
			REQUIRE(*update != nullptr);
			REQUIRE(editor.Execute(std::move(*update)).has_value());
			CHECK_FALSE(editor.GetScene().FindEntityByPath("/House/Room/Lamp/Bulb").GetComponent<MeshRendererComponent>().CastShadows);

			// Undoing the rewrite restores both files.
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(FileSystem::ReadText(root / "Assets/Prefabs/Room.prefab").value_or(std::string()) == *roomBefore);
			CHECK(FileSystem::ReadText(root / "Assets/Prefabs/House.prefab").value_or(std::string()) == *houseBefore);
			CHECK(editor.GetScene().FindEntityByPath("/House/Room/Lamp/Bulb").GetComponent<MeshRendererComponent>().CastShadows);
		}

		// Command.h: Undo is atomic like Execute, so a failed undo leaves the command applied. Room.prefab's file held content
		// the importer refuses when the nesting rewrite was built (written behind the editor's back; the imported version
		// stays the last good one), so undoing the rewrite writes that content back and its import fails: the undo fails,
		// and the rewritten document is written and imported again. The undo used to return with the refused content on disk.
		TEST_CASE("EditorContext: a nesting rewrite whose undo cannot import the previous documents stays applied")
		{
			Test::AutomationFixture setup("PrefabNestingUndoAtomic");
			EditorContext& editor = setup.GetEditor();
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Room" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Lamp" }, { "parent", "/Room" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/Room/Lamp" }, { "components", Json{ { "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Cube" } } } } } }).has_value());
			const Result<Json> created = setup.Call("prefab.create", Json{ { "entity", "/Room/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } });
			REQUIRE(created.has_value());
			REQUIRE(setup.Call("prefab.create", Json{ { "entity", "/Room" }, { "path", "Assets/Prefabs/Room.prefab" }, { "replaceWithInstance", true } }).has_value());
			const std::optional<UUID> lampID = UUID::FromString(JsonReader((*created)["prefab"]["id"]).ReadString().value_or(std::string()));
			REQUIRE(lampID.has_value());
			const AssetHandle lampOnly[] = { AssetHandle(*lampID) };

			// Lamp.prefab gets a new version outside the nesting rewrite: its bulb casts no shadows.
			const std::filesystem::path root = setup.GetEditorFixture().GetProjectRoot();
			const Result<std::string> lampText = FileSystem::ReadText(root / "Assets/Prefabs/Lamp.prefab");
			REQUIRE(lampText.has_value());
			std::string edited = *lampText;
			const size_t shadows = edited.find("\"CastShadows\": true");
			REQUIRE(shadows != std::string::npos);
			edited.replace(shadows, std::string_view("\"CastShadows\": true").size(), "\"CastShadows\": false");
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Prefabs/Lamp.prefab"), AsEditorBytes(edited)).has_value());
			REQUIRE(editor.GetAssets().Reimport(AssetHandle(*lampID)).has_value());
			const std::filesystem::path roomFile = root / "Assets/Prefabs/Room.prefab";
			REQUIRE(FileSystem::WriteFileAtomic(roomFile, AsEditorBytes("not a prefab document\n"), { .KeepBackup = false }).has_value());

			Result<NestingPrefabUpdate> rewrite = editor.CreateNestingPrefabUpdateCommand(lampOnly);
			REQUIRE_MESSAGE(rewrite.has_value(), rewrite.error().ToString());
			REQUIRE(rewrite->Rewrite != nullptr);
			REQUIRE(editor.Execute(std::move(rewrite->Rewrite)).has_value());
			const Result<std::string> rewritten = FileSystem::ReadText(roomFile);
			REQUIRE(rewritten.has_value());
			REQUIRE(rewritten->contains("\"CastShadows\": false"));
			const size_t steps = editor.GetHistory().GetUndoCount();
			{
				const Test::ExpectLog importFailure(LogLevel::Error, "ASSET_IMPORT_FAILED Assets/Prefabs/Room.prefab");
				const Result<size_t> undone = editor.GetHistory().Undo(editor);
				editor.GetAssets().WaitIdle();
				REQUIRE_FALSE(undone.has_value());
			}
			CHECK(editor.GetHistory().GetUndoCount() == steps);
			CHECK(FileSystem::ReadText(roomFile).value_or(std::string()) == *rewritten);
		}

		// Docs/Decisions/0031-m20-contract.md decision 8: a cycle of two prefabs (A nests B, B nests A) is reported with one
		// issue per prefab on it, A and B, and never names C, which nests A without being on the cycle; C itself has no
		// prefab nesting it, so a new version of C meets no cycle.
		TEST_CASE("EditorContext: a nesting cycle of two prefabs names both and no prefab that only nests it")
		{
			Test::EditorTestFixture fixture("PrefabNestingCycleOfTwo");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const AssetHandle a(0x3a7e00000000a001ull), b(0x3a7e00000000b001ull), c(0x3a7e00000000c001ull);
			const UUID aRoot(0xa1), bRoot(0xb1), cRoot(0xc1);
			WriteNestingPrefab(editor, "A", a, aRoot, UUID(0xa2), b, bRoot);
			WriteNestingPrefab(editor, "B", b, bRoot, UUID(0xb2), a, aRoot);
			WriteNestingPrefab(editor, "C", c, cRoot, UUID(0xc2), a, aRoot);
			REQUIRE(editor.GetAssets().Refresh().has_value());
			// Imported now, so that the dependency graph holds each prefab's nested prefabs.
			for (const AssetHandle prefab : { a, b, c })
				REQUIRE(editor.GetAssets().Reimport(prefab).has_value());

			const AssetHandle aOnly[] = { a };
			const Result<std::vector<AssetHandle>> nesting = editor.GetNestingPrefabs(aOnly);
			REQUIRE_FALSE(nesting.has_value());
			CHECK(nesting.error().GetCode() == ErrorCode::Validation);
			CHECK(nesting.error().ToString().contains("PREFAB_CYCLE"));
			const std::vector<ErrorIssue>& issues = nesting.error().GetIssues();
			REQUIRE(issues.size() == 2);
			CHECK(issues[0].Message.contains("'Assets/Prefabs/A.prefab'"));
			CHECK(issues[1].Message.contains("'Assets/Prefabs/B.prefab'"));
			CHECK_FALSE(nesting.error().ToString().contains("C.prefab"));

			const AssetHandle cOnly[] = { c };
			const Result<std::vector<AssetHandle>> nestingC = editor.GetNestingPrefabs(cOnly);
			REQUIRE_MESSAGE(nestingC.has_value(), nestingC.error().ToString());
			CHECK(nestingC->empty());
		}

		TEST_CASE("EditorContext: moving and removing project files keeps provenance in step")
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

		TEST_CASE("EditorContext: moving a folder re-records every file in it and removing a full folder is refused")
		{
			Test::EditorTestFixture fixture("EditorMoveFolder");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const std::string material = R"({"Format": "Material", "Version": 1})";
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Old/Red.material"), AsEditorBytes(material)).has_value());
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Old/Deep/Blue.material"), AsEditorBytes(material)).has_value());
			editor.SetWriteAttribution(WriteAttribution{ .Method = "asset.move", .RequestId = VariantValue(Json(4)), .Client = "test", .TranscriptLine = 9 });
			REQUIRE(editor.MoveProjectFile(MakeEditorPath("project://Assets/Old"), MakeEditorPath("project://Assets/New/Place")).has_value());
			editor.SetWriteAttribution(std::nullopt);
			const ProvenanceRecorder* provenance = editor.GetProvenance();
			REQUIRE(provenance != nullptr);
			CHECK(provenance->Find("Assets/Old/Red.material") == nullptr);
			CHECK(provenance->Find("Assets/Old/Deep/Blue.material") == nullptr);
			const ProvenanceEntry* moved = provenance->Find("Assets/New/Place/Deep/Blue.material");
			REQUIRE(moved != nullptr);
			CHECK(moved->Method == "asset.move");
			CHECK(moved->Hash == XXH64(material));
			CHECK(provenance->Find("Assets/New/Place/Red.material") != nullptr);

			// A moved file that leaves Assets/ (the trash) loses its entry; only an empty folder can be removed.
			REQUIRE(editor.MoveProjectFile(MakeEditorPath("project://Assets/New/Place/Red.material"), MakeEditorPath("project://Library/Trash/Red.material"))
					.has_value());
			CHECK(provenance->Find("Assets/New/Place/Red.material") == nullptr);
			CHECK(provenance->Find("Library/Trash/Red.material") == nullptr);
			const Status full = editor.RemoveProjectFile(MakeEditorPath("project://Assets/New/Place"));
			REQUIRE_FALSE(full.has_value());
			CHECK(full.error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.GetVfs().Exists(MakeEditorPath("project://Assets/New/Place/Deep/Blue.material")));
			const Status missing = editor.RemoveProjectFile(MakeEditorPath("project://Assets/Nothing.material"));
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("EditorContext: a move carries a provenance entry only to a file whose bytes still match it")
		{
			Test::EditorTestFixture fixture("EditorMoveUnrecorded");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const std::filesystem::path root = editor.GetProject().GetRoot();
			const std::string material = R"({"Format": "Material", "Version": 1})";
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Old/Kept.material"), AsEditorBytes(material)).has_value());
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Old/Edited.material"), AsEditorBytes(material)).has_value());
			REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Single.material"), AsEditorBytes(material)).has_value());
			// Changed and created outside the editor: Edited's and Single's entries no longer match, Hand has none.
			const std::string edited = R"({"Format": "Material", "Version": 1, "Name": "Hand"})";
			WriteStrictnessFile(root / "Assets/Old/Edited.material", edited);
			WriteStrictnessFile(root / "Assets/Old/Hand.material", material);
			WriteStrictnessFile(root / "Assets/Single.material", edited);

			editor.SetWriteAttribution(WriteAttribution{ .Method = "asset.move", .RequestId = VariantValue(Json(5)), .Client = "test", .TranscriptLine = 11 });
			REQUIRE(editor.MoveProjectFile(MakeEditorPath("project://Assets/Old"), MakeEditorPath("project://Assets/New")).has_value());
			REQUIRE(editor.MoveProjectFile(MakeEditorPath("project://Assets/Single.material"), MakeEditorPath("project://Assets/Moved.material")).has_value());
			editor.SetWriteAttribution(std::nullopt);

			const ProvenanceRecorder* provenance = editor.GetProvenance();
			REQUIRE(provenance != nullptr);
			const ProvenanceEntry* kept = provenance->Find("Assets/New/Kept.material");
			REQUIRE(kept != nullptr);
			CHECK(kept->Method == "asset.move");
			CHECK(kept->TranscriptLine == std::optional<uint64_t>(11));
			CHECK(kept->Hash == XXH64(material));
			// A move writes no content: the files the editor did not write keep no entry, so the audit reports them.
			CHECK(provenance->Find("Assets/New/Edited.material") == nullptr);
			CHECK(provenance->Find("Assets/New/Hand.material") == nullptr);
			CHECK(provenance->Find("Assets/Moved.material") == nullptr);
			for (const std::string_view source : { "Assets/Old/Kept.material", "Assets/Old/Edited.material", "Assets/Single.material" })
				CHECK(provenance->Find(source) == nullptr);
			CHECK(editor.GetVfs().Exists(MakeEditorPath("project://Assets/New/Hand.material")));
		}

		TEST_CASE("EditorContext: the write path refuses read-only projects and works without provenance in dry runs")
		{
			Test::EditorTestFixture fixture("EditorWritePath");
			EditorContext& editor = fixture.GetEditor();
			const Status launcher = editor.CreateProjectDirectory(MakeEditorPath("project://Assets/Folder"));
			REQUIRE_FALSE(launcher.has_value());
			CHECK(launcher.error().GetCode() == ErrorCode::InvalidState);

			fixture.CreateAndOpenProject();
			{
				const Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun.has_value());
				REQUIRE(editor.WriteProjectFile(MakeEditorPath("project://Assets/Dry.material"), AsEditorBytes("{}")).has_value());
				REQUIRE(editor.MoveProjectFile(MakeEditorPath("project://Assets/Dry.material"), MakeEditorPath("project://Assets/Moved.material")).has_value());
				CHECK(editor.GetVfs().Exists(MakeEditorPath("project://Assets/Moved.material")));
				CHECK(editor.GetProvenance()->Find("Assets/Moved.material") == nullptr);
			}
			CHECK_FALSE(editor.GetVfs().Exists(MakeEditorPath("project://Assets/Moved.material")));
			CHECK(editor.GetProvenance()->Find("Assets/Dry.material") == nullptr);
		}

		TEST_CASE("EditorContext: prefab updates need an open scene and change nothing without instances")
		{
			Test::EditorTestFixture fixture("EditorPrefabUpdate");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const AssetHandle prefab(0x1234567890abcdefull);
			const AssetHandle prefabs[] = { prefab };
			const Result<Scope<Command>> withoutScene = editor.CreatePrefabUpdateCommand(prefabs);
			REQUIRE_FALSE(withoutScene.has_value());
			CHECK(withoutScene.error().GetCode() == ErrorCode::InvalidState);

			fixture.CreateAndOpenScene();
			static_cast<void>(CreateTrackedEntity(editor, "Plain"));
			const uint64_t revision = editor.GetRevision();
			const Result<Scope<Command>> nothing = editor.CreatePrefabUpdateCommand(prefabs);
			REQUIRE(nothing.has_value());
			CHECK(*nothing == nullptr);
			const Result<bool> updated = editor.UpdatePrefabInstances(editor.GetScene(), {});
			REQUIRE(updated.has_value());
			CHECK_FALSE(*updated);
			CHECK(editor.GetRevision() == revision);
		}

		// §6 "Strict reader" and the M15 completion review's A024: Editor --strict turns unknown keys, fields and components
		// of the project's .eproj, scenes and prefabs into errors; without it they are warnings and the project opens.
		TEST_CASE("EditorContext: --strict refuses a project whose .eproj, scenes or prefabs hold unknown keys or components, which only warn without it")
		{
			const Test::TempDirectory directory("EditorStrict");
			std::filesystem::path projectFile;
			{
				Test::StrictnessEditor creator(directory / "Creator", false);
				const Result<CreatedProject> created = ProjectManager::CreateProject(
					{ .Directory = directory / "Game", .Name = "Game", .Template = ProjectTemplate::Empty, .TemplatesDirectory = creator.GetEditor().GetSpecification().TemplatesDirectory },
					creator.GetEditor().GetTypeRegistry());
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				projectFile = created->ProjectFile;
			}
			const std::filesystem::path root = projectFile.parent_path();
			const std::string cleanScene(ExternalMainScene);
			std::string unknownKeyScene = cleanScene;
			constexpr std::string_view ActiveKey = "\"Active\": true,";
			const size_t active = unknownKeyScene.find(ActiveKey);
			REQUIRE(active != std::string::npos);
			unknownKeyScene.insert(active + ActiveKey.size(), "\n\t\t\t\"Colour\": \"red\",");
			WriteStrictnessFile(root / "Assets" / "Scenes" / "Main.scene", unknownKeyScene);
			WriteStrictnessFile(root / "Assets" / "Prefabs" / "Crate.prefab", UnknownComponentPrefab);
			const Result<VfsPath> scenePath = VfsPath::Create("project", "Assets/Scenes/Main.scene");
			REQUIRE(scenePath.has_value());

			// Without --strict: the project opens, and scene.open's load reports the unknown key as a warning.
			{
				Test::StrictnessEditor lenient(directory / "Lenient", false);
				const Status opened = lenient.Open(projectFile);
				REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
				const Scope<Scene> scene = lenient.GetEditor().CreateScene("Main");
				LoadReport report;
				REQUIRE(Utils::LoadSceneFile(lenient.GetEditor(), *scene, *scenePath, LoadMode::Strict, nullptr, report).has_value());
				CHECK(std::ranges::any_of(report.Diagnostics, [](const LoadDiagnostic& diagnostic)
				{
					return diagnostic.Severity == DiagnosticSeverity::Warning && diagnostic.Message.contains("Colour");
				}));
				REQUIRE(lenient.GetEditor().CloseProject().has_value());
			}

			// With --strict: the open fails naming both files, and leaves the project closed and its lock released.
			{
				Test::StrictnessEditor strict(directory / "Strict", true);
				const Status refused = strict.Open(projectFile);
				REQUIRE_FALSE(refused.has_value());
				INFO(refused.error().ToString());
				CHECK(refused.error().GetCode() == ErrorCode::Validation);
				CHECK(refused.error().GetIssues().size() == 2);
				CHECK(refused.error().ToString().contains("Assets/Prefabs/Crate.prefab"));
				CHECK(refused.error().ToString().contains("Hovercraft"));
				CHECK(refused.error().ToString().contains("Assets/Scenes/Main.scene"));
				CHECK(refused.error().ToString().contains("Colour"));
				CHECK_FALSE(strict.GetEditor().HasProject());
				CHECK_FALSE(strict.GetEditor().GetVfs().IsMounted("project"));

				// Clean files open (the lock was released); a scene file that gains an unknown key afterwards fails the
				// strict editor's scene.open load.
				WriteStrictnessFile(root / "Assets" / "Scenes" / "Main.scene", cleanScene);
				REQUIRE(FileSystem::Remove(root / "Assets" / "Prefabs" / "Crate.prefab").has_value());
				const Status opened = strict.Open(projectFile);
				REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
				WriteStrictnessFile(root / "Assets" / "Scenes" / "Main.scene", unknownKeyScene);
				const Scope<Scene> scene = strict.GetEditor().CreateScene("Main");
				LoadReport report;
				const Status loaded = Utils::LoadSceneFile(strict.GetEditor(), *scene, *scenePath, LoadMode::Strict, nullptr, report);
				REQUIRE_FALSE(loaded.has_value());
				CHECK(loaded.error().ToString().contains("Colour"));
				REQUIRE(strict.GetEditor().CloseProject().has_value());
			}

			// An unknown key in the .eproj: a warning without --strict, an error located at it with --strict.
			WriteStrictnessFile(root / "Assets" / "Scenes" / "Main.scene", cleanScene);
			Result<std::string> projectText = FileSystem::ReadText(projectFile);
			REQUIRE(projectText.has_value());
			const size_t brace = projectText->find('{');
			REQUIRE(brace != std::string::npos);
			projectText->insert(brace + 1, "\n\t\"Theme\": \"Dark\",");
			WriteStrictnessFile(projectFile, *projectText);
			{
				Test::StrictnessEditor lenient(directory / "LenientProject", false);
				const Status opened = lenient.Open(projectFile);
				REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
				const std::vector<ValidationIssue>& diagnostics = lenient.GetEditor().GetProject().GetLoadReport().Diagnostics;
				CHECK(std::ranges::any_of(diagnostics, [](const ValidationIssue& issue)
				{
					return issue.Severity == DiagnosticSeverity::Warning && issue.JsonPointer == "/Theme";
				}));
				REQUIRE(lenient.GetEditor().CloseProject().has_value());
			}
			{
				Test::StrictnessEditor strict(directory / "StrictProject", true);
				const Status refused = strict.Open(projectFile);
				REQUIRE_FALSE(refused.has_value());
				INFO(refused.error().ToString());
				CHECK(refused.error().GetCode() == ErrorCode::Validation);
				CHECK(refused.error().ToString().contains("/Theme"));
				CHECK_FALSE(strict.GetEditor().HasProject());
			}
		}

		// The M15 fix review's C2: --strict refuses a project for every scene or prefab that does not load strictly, not only
		// for unknown keys (CI wants every committed scene and prefab to load strictly): a structural defect, a file that is
		// not a document and a newer Version each name their file. Without --strict the project opens, since the editor
		// loads its scenes and prefabs only when they are opened or imported.
		TEST_CASE("EditorContext: --strict refuses a project whose scene or prefab does not load strictly for any reason, which opens without it")
		{
			const Test::TempDirectory directory("EditorStrictLoad");
			std::filesystem::path projectFile;
			{
				Test::StrictnessEditor creator(directory / "Creator", false);
				const Result<CreatedProject> created = ProjectManager::CreateProject(
					{ .Directory = directory / "Game", .Name = "Game", .Template = ProjectTemplate::Empty, .TemplatesDirectory = creator.GetEditor().GetSpecification().TemplatesDirectory },
					creator.GetEditor().GetTypeRegistry());
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				projectFile = created->ProjectFile;
			}
			const std::filesystem::path root = projectFile.parent_path();
			// Two entities with one ID: a structural defect (§6 "Structural pre-validation"), no unknown key.
			constexpr std::string_view DuplicateIdScene = R"({
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
			"Name": "First",
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
		},
		{
			"ID": "1a00000000000001",
			"Name": "Second",
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
			// A prefab of a newer format version than this build reads.
			constexpr std::string_view FuturePrefab = R"({
	"Format": "Prefab",
	"Version": 99,
	"Name": "Crate",
	"Root": "00000000000c0001",
	"ComponentVersions": {
		"Transform": 1
	},
	"Entities": [
		{
			"ID": "00000000000c0001",
			"Name": "Crate",
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
			WriteStrictnessFile(root / "Assets" / "Scenes" / "Main.scene", DuplicateIdScene);
			WriteStrictnessFile(root / "Assets" / "Scenes" / "Truncated.scene", "{\n\t\"Format\": \"Scene\",\n\t\"Version\": 1,\n");
			WriteStrictnessFile(root / "Assets" / "Prefabs" / "Crate.prefab", FuturePrefab);

			// Without --strict: the project opens.
			{
				Test::StrictnessEditor lenient(directory / "Lenient", false);
				const Status opened = lenient.Open(projectFile);
				REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
				CHECK(lenient.GetEditor().HasProject());
				REQUIRE(lenient.GetEditor().CloseProject().has_value());
			}

			// With --strict: the open fails with one issue per file, each naming its file and its problem, and leaves the
			// project closed.
			{
				Test::StrictnessEditor strict(directory / "Strict", true);
				const Status refused = strict.Open(projectFile);
				REQUIRE_FALSE(refused.has_value());
				INFO(refused.error().ToString());
				CHECK(refused.error().GetCode() == ErrorCode::Validation);
				const std::vector<ErrorIssue>& issues = refused.error().GetIssues();
				REQUIRE(issues.size() == 3);
				CHECK(issues[0].Message.starts_with("Assets/Prefabs/Crate.prefab: "));
				CHECK(issues[0].Message.contains("newer than this build supports"));
				CHECK(issues[1].Message.starts_with("Assets/Scenes/Main.scene: "));
				CHECK(issues[1].Message.contains("SCENE_DUPLICATE_ID"));
				CHECK(issues[2].Message.starts_with("Assets/Scenes/Truncated.scene: "));
				CHECK(issues[2].Message.contains("Parse"));
				CHECK_FALSE(strict.GetEditor().HasProject());
				CHECK_FALSE(strict.GetEditor().GetVfs().IsMounted("project"));
			}
		}

		// The M16 dry run's S1-04: a scene whose prefab instance overrides a script field (Breakout's bricks, each row's Points)
		// made every --strict editor refuse the project, because the override's value was resolved without its Script
		// component. It is read as free-form JSON and checked on the member's own Script component, so a valid override
		// opens and an invalid one is refused once, at the member's field, under the code project.validate reports.
		TEST_CASE("EditorContext: --strict opens a project whose scene overrides a prefab instance's script field and names an invalid override")
		{
			const Test::TempDirectory directory("EditorStrictScriptOverride");
			std::filesystem::path projectFile;
			{
				Test::StrictnessEditor creator(directory / "Creator", false);
				const Result<CreatedProject> created = ProjectManager::CreateProject(
					{ .Directory = directory / "Game", .Name = "Game", .Template = ProjectTemplate::Empty, .TemplatesDirectory = creator.GetEditor().GetSpecification().TemplatesDirectory },
					creator.GetEditor().GetTypeRegistry());
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				projectFile = created->ProjectFile;
			}
			const std::filesystem::path root = projectFile.parent_path();
			Test::WriteScriptOverrideProject(root);
			{
				Test::StrictnessEditor strict(directory / "Strict", true);
				const Status opened = strict.Open(projectFile);
				REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
				REQUIRE(strict.GetEditor().CloseProject().has_value());
			}

			// An override of a field the script does not declare.
			Test::WriteScriptOverrideProject(root, "Pointz");
			Test::StrictnessEditor strict(directory / "StrictInvalid", true);
			const Status refused = strict.Open(projectFile);
			REQUIRE_FALSE(refused.has_value());
			INFO(refused.error().ToString());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			const std::string pointer = std::string(Test::ScriptOverrideFieldsPointer) + "Pointz";
			const std::vector<ErrorIssue>& issues = refused.error().GetIssues();
			REQUIRE(issues.size() == 2);
			CHECK(issues[0].Message.starts_with("Assets/Scenes/Main.scene: "));
			CHECK(issues[0].JsonPointer == pointer);
			CHECK(issues[1].JsonPointer == pointer);
			CHECK(issues[1].Message.contains("SCRIPT_UNKNOWN_FIELD_OVERRIDE"));
			CHECK(issues[1].Message.contains(Test::ScriptOverrideInstanceID.ToString()));
			for (const ErrorIssue& issue : issues)
				CHECK_FALSE(issue.JsonPointer.contains("/Components/Prefab/"));
			CHECK_FALSE(strict.GetEditor().HasProject());
		}

		// M16 review R24: the strict open named every unresolved Script.Fields value SCRIPT_UNKNOWN_FIELD_OVERRIDE with the hint
		// to remove the override, also a value of the wrong type and every value of a script that failed to import, which
		// project.validate reports as a type mismatch and as the script's own error. It now names a value only under the
		// code validate reports for it.
		TEST_CASE("EditorContext: --strict names a script field value only under the code project.validate reports for it")
		{
			const Test::TempDirectory directory("EditorStrictScriptFieldCodes");
			std::filesystem::path projectFile;
			{
				Test::StrictnessEditor creator(directory / "Creator", false);
				const Result<CreatedProject> created = ProjectManager::CreateProject(
					{ .Directory = directory / "Game", .Name = "Game", .Template = ProjectTemplate::Empty, .TemplatesDirectory = creator.GetEditor().GetSpecification().TemplatesDirectory },
					creator.GetEditor().GetTypeRegistry());
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				projectFile = created->ProjectFile;
			}
			const std::filesystem::path root = projectFile.parent_path();
			const auto mentions = [](const std::vector<ErrorIssue>& issues, std::string_view text)
			{
				return std::ranges::count_if(issues, [text](const ErrorIssue& issue)
				{
					return issue.Message.contains(text) || issue.Hint.contains(text);
				});
			};

			// A valid override of a script that fails to import (it never imported): the scene and the prefab that run it do
			// not load strictly, each issue is a file's own load error, and no value is named.
			Test::WriteScriptOverrideProject(root);
			Test::WriteScriptOverrideScript(root, Test::ScriptOverrideBrokenSource);
			{
				const Test::ExpectLog importFailure(LogLevel::Error, "ASSET_IMPORT_FAILED Assets/Scripts/Brick.luau");
				Test::StrictnessEditor strict(directory / "StrictBrokenScript", true);
				const Status refused = strict.Open(projectFile);
				REQUIRE_FALSE(refused.has_value());
				INFO(refused.error().ToString());
				const std::vector<ErrorIssue>& issues = refused.error().GetIssues();
				REQUIRE_FALSE(issues.empty());
				CHECK(std::ranges::count_if(issues, [](const ErrorIssue& issue)
				{
					return issue.Message.starts_with("Assets/Scenes/Main.scene: ");
				}) == 1);
				for (const ErrorIssue& issue : issues)
					CHECK((issue.Message.starts_with("Assets/Scenes/Main.scene: ") || issue.Message.starts_with("Assets/Prefabs/Brick.prefab: ")));
				CHECK(mentions(issues, "SCRIPT_UNKNOWN_FIELD_OVERRIDE") == 0);
				CHECK(mentions(issues, "correct or remove the override") == 0);
			}

			// A value of the wrong type is a type mismatch, at the member's field.
			Test::WriteScriptOverrideProject(root, "Points", "\"five\"");
			Test::StrictnessEditor strict(directory / "StrictMismatch", true);
			const Status refused = strict.Open(projectFile);
			REQUIRE_FALSE(refused.has_value());
			INFO(refused.error().ToString());
			const std::vector<ErrorIssue>& issues = refused.error().GetIssues();
			CHECK(mentions(issues, "SCRIPT_FIELD_TYPE_MISMATCH") == 1);
			CHECK(mentions(issues, "SCRIPT_UNKNOWN_FIELD_OVERRIDE") == 0);
			const auto named = std::ranges::find_if(issues, [](const ErrorIssue& issue)
			{
				return issue.Message.contains("SCRIPT_FIELD_TYPE_MISMATCH");
			});
			REQUIRE(named != issues.end());
			CHECK(named->JsonPointer == std::string(Test::ScriptOverrideFieldsPointer) + "Points");
		}
	}

}
