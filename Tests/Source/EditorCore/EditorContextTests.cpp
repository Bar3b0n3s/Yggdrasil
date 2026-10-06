#include "TestsPCH.h"

#include "EditorCore/EditorContext.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Support/EditorTestFixture.h"

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

		TEST_CASE("EditorContext: the revision grows with every mutation and never repeats across scenes" * doctest::skip(true))
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

		TEST_CASE("EditorContext: opening a project mounts project:// and cache://" * doctest::skip(true))
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

		TEST_CASE("EditorContext: a second project while one is open is InvalidState" * doctest::skip(true))
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

		TEST_CASE("EditorContext: WriteProjectFile records provenance for Assets/ and the .eproj only" * doctest::skip(true))
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

		TEST_CASE("EditorContext: a read-only project refuses writes and commands with PermissionDenied" * doctest::skip(true))
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

		TEST_CASE("EditorContext: SetScene clears the history and the selection" * doctest::skip(true))
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

		TEST_CASE("EditorContext: commands executed while serving a request are agent commands" * doctest::skip(true))
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

		TEST_CASE("EditorTransaction: commit records one composite undo step" * doctest::skip(true))
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

		TEST_CASE("EditorTransaction: a transaction opened inside another joins it" * doctest::skip(true))
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

		TEST_CASE("EditorTransaction: a rollback that meets a failing Undo records what stays applied" * doctest::skip(true))
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

		TEST_CASE("EditorTransaction: destroying an uncommitted transaction rolls back" * doctest::skip(true))
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

		TEST_CASE("EditorDryRunScope: scene mutations leave the revision, history and selection unchanged" * doctest::skip(true))
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

		TEST_CASE("EditorDryRunScope: file writes stay in the overlay and record no provenance or events" * doctest::skip(true))
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

		TEST_CASE("EditorContext: RequestShutdown keeps the first exit code" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("EditorShutdown");
			fixture.GetEditor().RequestShutdown(0);
			fixture.GetEditor().RequestShutdown(1);
			CHECK(fixture.GetEditor().GetShutdownRequest() == 0);
		}
	}

}
