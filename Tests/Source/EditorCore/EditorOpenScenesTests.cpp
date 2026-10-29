#include "TestsPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		VfsPath OpenScenesPath(std::string_view text)
		{
			Result<VfsPath> path = VfsPath::Parse(text);
			REQUIRE_MESSAGE(path.has_value(), path.error().ToString());
			return *path;
		}

		// Writes an empty scene named after `path` and opens it as an additional open scene (EditorContext::AddScene).
		OpenSceneId AddOpenScene(EditorContext& editor, std::string_view path)
		{
			const VfsPath file = OpenScenesPath(path);
			Scope<Scene> scene = editor.CreateScene(std::string(file.GetStem()));
			const Result<std::string> text = SceneSerializer::SaveToString(*scene);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			REQUIRE(editor.WriteProjectFile(file, std::as_bytes(std::span(text->data(), text->size()))).has_value());
			const Result<OpenSceneId> added = editor.AddScene(std::move(scene), file);
			REQUIRE_MESSAGE(added.has_value(), added.error().ToString());
			return *added;
		}

		// Creates an entity named `name` in the addressed scene as one undoable edit; returns its id.
		UUID CreateEdited(EditorContext& editor, std::string name)
		{
			SceneEdit edit(editor, "Create " + name);
			const UUID id = editor.GetScene().CreateEntity(std::move(name)).GetUUID();
			REQUIRE(edit.Commit().has_value());
			return id;
		}

		std::string OpenScenesName(const Json& entity)
		{
			return JsonReader(entity["name"]).ReadString().value_or(std::string());
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0031-m20-contract.md decision 13 (E-SCENES; Roadmap M20 acceptance "undo is isolated per open scene"):
		// every open scene is its own document. Edits, undo, the dirty flag, the selection and the revision of one never
		// touch another, switching the active scene keeps each scene's state, and revisions never repeat across them.
		TEST_CASE("EditorContext: each open scene keeps its own history, dirty flag, selection and revision")
		{
			Test::EditorTestFixture fixture("OpenScenesIsolated");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(main != NoOpenScene);
			const UUID inMain = CreateEdited(editor, "InMain");
			editor.SetSelection({ inMain });
			const uint64_t mainRevision = editor.GetRevision();

			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			CHECK(second != main);
			CHECK(editor.GetActiveSceneId() == second);
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ main, second });
			CHECK(editor.GetScene().GetName() == "Second");
			CHECK_FALSE(editor.IsSceneDirty());
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK(editor.GetSelection().empty());
			// A revision names one state of one scene: the new scene's never equals one the first reported.
			CHECK(editor.GetRevision() > mainRevision);

			const UUID inSecond = CreateEdited(editor, "InSecond");
			CHECK(editor.IsSceneDirty());
			const Result<OpenSceneInfo> mainInfo = editor.DescribeOpenScene(main);
			REQUIRE(mainInfo.has_value());
			CHECK(mainInfo->Revision == mainRevision);
			CHECK(mainInfo->Dirty);
			CHECK_FALSE(mainInfo->Active);
			CHECK(mainInfo->EntityCount == 1);
			CHECK(editor.IsAnySceneDirty());

			// Undo in the second scene leaves the first as it was.
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK_FALSE(editor.GetScene().FindEntityByID(inSecond).IsValid());
			CHECK(editor.DescribeOpenScene(main)->Revision == mainRevision);
			REQUIRE(editor.FindScene(main) != nullptr);
			CHECK(editor.FindScene(main)->FindEntityByID(inMain).IsValid());

			// Switching back restores the first scene's history, dirty flag and selection.
			REQUIRE(editor.ActivateScene(main).has_value());
			CHECK(editor.GetScene().GetName() == "Main");
			CHECK(editor.GetHistory().CanUndo());
			CHECK(editor.IsSceneDirty());
			CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ inMain });
			CHECK(editor.GetRevision() == mainRevision);
			CHECK(editor.FindOpenScene(OpenScenesPath("project://Assets/Scenes/Second.scene")) == second);

			// Closing the active scene activates its neighbour; an id never names another scene afterwards.
			REQUIRE(editor.CloseScene(main).has_value());
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ second });
			CHECK(editor.GetActiveSceneId() == second);
			CHECK(editor.FindScene(main) == nullptr);
			CHECK(editor.DescribeOpenScene(main).error().GetCode() == ErrorCode::NotFound);
			CHECK(editor.ActivateScene(main).error().GetCode() == ErrorCode::NotFound);
		}

		// Decision 13: GetScene() is the active scene for every caller outside a targeted request; an EditorSceneTargetScope
		// makes another open scene the addressed one, and commands executed inside it land in that scene and its history,
		// without changing the active scene.
		TEST_CASE("EditorContext: a scene target scope addresses another open scene and leaves the active one")
		{
			Test::EditorTestFixture fixture("OpenScenesTargetScope");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(main != NoOpenScene);
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			REQUIRE(editor.GetActiveSceneId() == second);

			UUID targeted;
			{
				Result<Scope<EditorSceneTargetScope>> scope = EditorSceneTargetScope::Begin(editor, main);
				REQUIRE_MESSAGE(scope.has_value(), scope.error().ToString());
				CHECK((*scope)->GetScene() == main);
				CHECK(editor.GetAddressedSceneId() == main);
				CHECK(editor.GetActiveSceneId() == second);
				CHECK(editor.GetScene().GetName() == "Main");
				targeted = CreateEdited(editor, "Targeted");
				CHECK(editor.GetHistory().CanUndo());
				// Scopes do not nest, and the active scene cannot change under one.
				CHECK(EditorSceneTargetScope::Begin(editor, second).error().GetCode() == ErrorCode::InvalidState);
				CHECK(editor.ActivateScene(second).error().GetCode() == ErrorCode::InvalidState);
			}
			CHECK(editor.GetAddressedSceneId() == second);
			CHECK(editor.GetScene().GetName() == "Second");
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK_FALSE(editor.GetScene().FindEntityByID(targeted).IsValid());
			REQUIRE(editor.FindScene(main) != nullptr);
			CHECK(editor.FindScene(main)->FindEntityByID(targeted).IsValid());
			CHECK(editor.DescribeOpenScene(main)->Dirty);
			CHECK(EditorSceneTargetScope::Begin(editor, OpenSceneId(9999)).error().GetCode() == ErrorCode::NotFound);
		}

		// Decision 13: inside a target scope only the scope's own scene can be activated or closed; closing it leaves the
		// scope addressing no scene until it ends, and the active scene is the addressed one again afterwards.
		TEST_CASE("EditorContext: a scene target scope can activate or close its own scene only")
		{
			Test::EditorTestFixture fixture("OpenScenesScopeOwnScene");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			const OpenSceneId third = AddOpenScene(editor, "project://Assets/Scenes/Third.scene");
			{
				Result<Scope<EditorSceneTargetScope>> scope = EditorSceneTargetScope::Begin(editor, second);
				REQUIRE(scope.has_value());
				CHECK(editor.CloseScene(main).error().GetCode() == ErrorCode::InvalidState);
				REQUIRE(editor.ActivateScene(second).has_value());
				CHECK(editor.GetActiveSceneId() == second);
				CHECK(editor.GetAddressedSceneId() == second);
			}
			CHECK(editor.GetAddressedSceneId() == second);
			CHECK(editor.GetScene().GetName() == "Second");
			{
				Result<Scope<EditorSceneTargetScope>> scope = EditorSceneTargetScope::Begin(editor, main);
				REQUIRE(scope.has_value());
				REQUIRE(editor.CloseScene(main).has_value());
				CHECK((*scope)->GetScene() == NoOpenScene);
				CHECK(editor.GetAddressedSceneId() == NoOpenScene);
				CHECK_FALSE(editor.HasScene());
				CHECK(editor.GetActiveSceneId() == second);
				CHECK(editor.CloseScene(third).error().GetCode() == ErrorCode::InvalidState);
			}
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ second, third });
			CHECK(editor.GetAddressedSceneId() == second);
			CHECK(editor.GetScene().GetName() == "Second");
		}

		// Decision 13: at most MaxOpenScenes open scenes, one per path; the open scenes change only outside dry runs and
		// transactions.
		TEST_CASE("EditorContext: AddScene refuses a seventeenth open scene, an open path, a dry run and a transaction")
		{
			Test::EditorTestFixture fixture("OpenScenesLimits");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			for (size_t index = 1; index < MaxOpenScenes; ++index)
				static_cast<void>(AddOpenScene(editor, std::format("project://Assets/Scenes/Scene{}.scene", index)));
			REQUIRE(editor.GetOpenScenes().size() == MaxOpenScenes);
			const OpenSceneId last = editor.GetActiveSceneId();
			CHECK(editor.AddScene(editor.CreateScene("Extra"), std::nullopt).error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.GetOpenScenes().size() == MaxOpenScenes);
			CHECK(editor.GetActiveSceneId() == last);

			REQUIRE(editor.CloseScene(last).has_value());
			CHECK(editor.AddScene(editor.CreateScene("Main"), OpenScenesPath("project://Assets/Scenes/Main.scene")).error().GetCode() == ErrorCode::AlreadyExists);
			const OpenSceneId active = editor.GetActiveSceneId();
			const OpenSceneId other = editor.GetOpenScenes().front();
			{
				Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun.has_value());
				CHECK(editor.AddScene(editor.CreateScene("DryRun"), std::nullopt).error().GetCode() == ErrorCode::InvalidState);
				CHECK(editor.ActivateScene(other).error().GetCode() == ErrorCode::InvalidState);
				CHECK(editor.CloseScene(other).error().GetCode() == ErrorCode::InvalidState);
			}
			{
				EditorTransaction transaction(editor, "Open scenes");
				CHECK(editor.AddScene(editor.CreateScene("Transaction"), std::nullopt).error().GetCode() == ErrorCode::InvalidState);
				CHECK(editor.ActivateScene(other).error().GetCode() == ErrorCode::InvalidState);
				CHECK(editor.CloseScene(other).error().GetCode() == ErrorCode::InvalidState);
			}
			CHECK(editor.GetOpenScenes().size() == MaxOpenScenes - 1);
			CHECK(editor.GetActiveSceneId() == active);
		}

		// Decision 13: a test run owns the editor's scenes, so none opens, activates or closes until it finishes.
		TEST_CASE("EditorContext: the open scenes do not change while a test run owns the editor")
		{
			Test::EditorTestFixture fixture("OpenScenesTestRun");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			// Every clock reading advances 100 ms, past TestRunFrameBudget, so one Pump advances the run by exactly one runner
			// step in every configuration: the run is still pending after the Pump below however fast the build is.
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(editor, std::move(specification));
			REQUIRE(client.Call("script.write", Json{ { "path", "Assets/Tests/Long.test.luau" }, { "source", "return Test.Suite('Long', function() Test.Case('waits', function() Test.WaitTicks(100000) end) end)" } }));
			REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Long.test.luau" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } }));
			static_cast<void>(client.Submit("test.run", Json::object()));
			client.GetServer().Pump();
			REQUIRE(editor.GetPlay().IsTestRunActive());
			CHECK(editor.AddScene(editor.CreateScene("DuringRun"), std::nullopt).error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.ActivateScene(main).error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.CloseScene(main).error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ main, second });
			CHECK(editor.GetActiveSceneId() == second);
			client.GetServer().DisconnectInProcess(client.GetClient());
			REQUIRE_FALSE(editor.GetPlay().IsTestRunActive());
			CHECK(editor.ActivateScene(main).has_value());
		}

		// Decision 13: closing the active tab activates the next one, the previous one when it was the last; SetScene
		// replaces the active scene in its tab position with a new id; activating and closing append their events.
		TEST_CASE("EditorContext: closing the active scene activates its next tab, the previous one after the last")
		{
			Test::EditorTestFixture fixture("OpenScenesTabs");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			const OpenSceneId third = AddOpenScene(editor, "project://Assets/Scenes/Third.scene");
			const uint64_t cursor = editor.GetEngine().GetEventLog().GetNextSeq();

			REQUIRE(editor.ActivateScene(second).has_value());
			REQUIRE(editor.CloseScene(second).has_value());
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ main, third });
			CHECK(editor.GetActiveSceneId() == third);
			CHECK(editor.GetScene().GetName() == "Third");
			REQUIRE(editor.CloseScene(third).has_value());
			CHECK(editor.GetActiveSceneId() == main);
			CHECK(editor.GetScene().GetName() == "Main");

			const EngineEventType types[] = { EngineEventType::SceneActivated, EngineEventType::SceneClosed };
			const EventReadResult events = editor.GetEngine().GetEventLog().Read(cursor, types, 10);
			REQUIRE(events.Events.size() == 3);
			CHECK(events.Events[0].Type == EngineEventType::SceneActivated);
			CHECK(events.Events[0].Path == "Assets/Scenes/Second.scene");
			CHECK(events.Events[1].Type == EngineEventType::SceneClosed);
			CHECK(events.Events[1].Path == "Assets/Scenes/Second.scene");
			CHECK(events.Events[2].Type == EngineEventType::SceneClosed);
			CHECK(events.Events[2].Path == "Assets/Scenes/Third.scene");

			// SetScene replaces the addressed scene in its tab with a new id, which the old one never names again.
			const OpenSceneId fourth = AddOpenScene(editor, "project://Assets/Scenes/Fourth.scene");
			REQUIRE(editor.ActivateScene(main).has_value());
			editor.SetScene(editor.CreateScene("Replacement"), std::nullopt);
			const std::vector<OpenSceneId> open = editor.GetOpenScenes();
			REQUIRE(open.size() == 2);
			CHECK(open[0] != main);
			CHECK(open[1] == fourth);
			CHECK(editor.GetActiveSceneId() == open[0]);
			CHECK(editor.FindScene(main) == nullptr);
			CHECK(editor.GetScene().GetName() == "Replacement");

			// Closing the last open scene leaves none.
			REQUIRE(editor.CloseScene(fourth).has_value());
			REQUIRE(editor.CloseScene(open[0]).has_value());
			CHECK(editor.GetOpenScenes().empty());
			CHECK(editor.GetActiveSceneId() == NoOpenScene);
			CHECK_FALSE(editor.HasScene());
		}

		// Decision 13 (ADR 0008 decision 28 kept): an open scene keeps its revision while it is not changed, also across
		// activations and dry runs, and its next change continues past every revision another open scene reported.
		TEST_CASE("EditorContext: a revision of one open scene never repeats another's and an unchanged scene keeps its own")
		{
			Test::EditorTestFixture fixture("OpenScenesRevisions");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			static_cast<void>(CreateEdited(editor, "First"));
			const uint64_t mainRevision = editor.GetRevision();
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			static_cast<void>(CreateEdited(editor, "Second"));
			static_cast<void>(CreateEdited(editor, "Third"));
			const uint64_t secondRevision = editor.GetRevision();
			REQUIRE(secondRevision > mainRevision);

			REQUIRE(editor.ActivateScene(main).has_value());
			CHECK(editor.GetRevision() == mainRevision);
			// A dry run reports what the real edit then reports.
			uint64_t dryRunRevision = 0;
			{
				Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun.has_value());
				CHECK(editor.GetRevision() == mainRevision);
				static_cast<void>(CreateEdited(editor, "DryRun"));
				dryRunRevision = editor.GetRevision();
			}
			CHECK(editor.GetRevision() == mainRevision);
			static_cast<void>(CreateEdited(editor, "Fourth"));
			const uint64_t mainChanged = editor.GetRevision();
			CHECK(mainChanged > secondRevision);
			CHECK(mainChanged == dryRunRevision);

			REQUIRE(editor.ActivateScene(second).has_value());
			CHECK(editor.GetRevision() == secondRevision);
			CHECK(editor.DescribeOpenScene(main)->Revision == mainChanged);
			static_cast<void>(CreateEdited(editor, "Fifth"));
			CHECK(editor.GetRevision() > mainChanged);
		}

		// The merge with M19 (Docs/Decisions/0031-m20-contract.md, "Merge with M19"): a native module load migrates every open
		// scene through ApplySceneMigration, a change no tracked component records. Its revision then moves past every
		// revision any open scene reported, also for a scene that keeps an older one until it changes (it used to advance
		// only the base under that kept revision, so the revision stayed and an ifRevision taken before still matched), and
		// a migration of a parked scene leaves the active scene addressed.
		TEST_CASE("EditorContext: a scene migration moves the revision of any open scene past every revision reported")
		{
			Test::EditorTestFixture fixture("OpenScenesMigration");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			static_cast<void>(CreateEdited(editor, "Second"));
			const uint64_t secondRevision = editor.GetRevision();
			REQUIRE(editor.ActivateScene(main).has_value());
			const uint64_t mainRevision = editor.GetRevision();
			REQUIRE(mainRevision < secondRevision);
			REQUIRE_FALSE(editor.IsSceneDirty());

			editor.ApplySceneMigration(main, true, false);
			const uint64_t migrated = editor.GetRevision();
			CHECK(migrated > secondRevision);
			CHECK(editor.IsSceneDirty());
			CHECK_FALSE(editor.GetHistory().CanUndo());

			// A parked scene's migration: its revision moves on, its history goes, the active scene stays addressed.
			REQUIRE(editor.DescribeOpenScene(second)->Dirty);
			editor.ApplySceneMigration(second, true, true);
			CHECK(editor.GetActiveSceneId() == main);
			CHECK(editor.GetAddressedSceneId() == main);
			CHECK(editor.GetRevision() == migrated);
			CHECK(editor.DescribeOpenScene(second)->Revision > migrated);
			CHECK(editor.DescribeOpenScene(second)->Dirty);
			REQUIRE(editor.ActivateScene(second).has_value());
			CHECK_FALSE(editor.GetHistory().CanUndo());
		}

		// Decision 15 (external changes per open scene): a changed file flags and reports only the open scene it concerns.
		TEST_CASE("EditorContext: an external change raises SceneChangedOnDisk on the open scene it concerns only")
		{
			Test::EditorTestFixture fixture("OpenScenesChangedOnDisk");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			REQUIRE(editor.ActivateScene(main).has_value());
			editor.Update(0.0);
			const uint64_t cursor = editor.GetEngine().GetEventLog().GetNextSeq();
			// Another program rewrites the parked scene's file; its size differs from what the editor wrote.
			Scope<Scene> external = editor.CreateScene("SecondChangedOutside");
			static_cast<void>(external->CreateEntity("Outside"));
			const Result<std::string> text = SceneSerializer::SaveToString(*external);
			REQUIRE(text.has_value());
			REQUIRE(FileSystem::WriteFileAtomic(editor.GetProject().GetRoot() / "Assets/Scenes/Second.scene", std::as_bytes(std::span(text->data(), text->size()))).has_value());
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
			{
				editor.Update(seconds);
				static_cast<void>(editor.GetEngine().GetMainThreadQueue().Drain());
			}
			CHECK_FALSE(editor.IsSceneChangedOnDisk());
			CHECK_FALSE(editor.DescribeOpenScene(main)->ChangedOnDisk);
			CHECK(editor.DescribeOpenScene(second)->ChangedOnDisk);
			const EngineEventType types[] = { EngineEventType::SceneChangedOnDisk };
			const EventReadResult events = editor.GetEngine().GetEventLog().Read(cursor, types, 10);
			REQUIRE(events.Events.size() == 1);
			CHECK(events.Events.front().Path == "Assets/Scenes/Second.scene");
			CHECK_FALSE(events.Events.front().Dirty);
			REQUIRE(editor.ActivateScene(second).has_value());
			CHECK(editor.IsSceneChangedOnDisk());
			CHECK(editor.GetScene().GetName() == "Second");
		}

		// Decision 13 (play): the play scene shows in the played scene's tab only; another tab selects in its edit scene, and
		// the played scene's play selection comes back to its edit scene once play stopped.
		TEST_CASE("EditorContext: the play selection belongs to the played scene and comes back to its edit scene after Stop")
		{
			Test::EditorTestFixture fixture("OpenScenesPlaySelection");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			const UUID played = CreateEdited(editor, "Played");
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			const UUID other = CreateEdited(editor, "Other");
			REQUIRE(editor.ActivateScene(main).has_value());
			editor.SetSelection({ played });
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }).has_value());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);

			// Another tab shows its edit scene: selections there address it, not the session.
			REQUIRE(editor.ActivateScene(second).has_value());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			editor.SetSelection({ played, other });
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ other });
			// A play selection still belongs to the played scene.
			REQUIRE(editor.SetSelection({ played }, SceneTarget::Play).has_value());
			CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ other });

			REQUIRE(editor.GetPlay().Stop().has_value());
			REQUIRE(editor.ActivateScene(main).has_value());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ played });
		}

		// Decision 15 (Roadmap M20 acceptance "a prefab update rebuilds every open scene"): applying an edit of an instance
		// in one open scene rebuilds the prefab's instances in another, as an entry of that scene's own history, so undo
		// there reverts only that scene; requests address the second scene through the scene target "scene".
		TEST_CASE("EditorContext: a prefab update rebuilds its instances in every open scene, each in its own history")
		{
			Test::AutomationFixture setup("OpenScenesPrefabUpdate");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Lamp" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/Lamp" } }).has_value());
			REQUIRE(setup.Call("prefab.create", Json{ { "entity", "/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } }).has_value());
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			REQUIRE(setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Lamp.prefab" }, { "name", "Copy" } }).has_value());
			const Result<Json> secondHistory = setup.Call("edit.history", Json::object());
			REQUIRE(secondHistory.has_value());
			const size_t secondEntries = (*secondHistory)["entries"].size();

			// Back in Main, rename the bulb and apply it to the prefab.
			const Json main{ { "scene", "Assets/Scenes/Main.scene" } };
			REQUIRE(setup.Call("scene.activate", main).has_value());
			REQUIRE(setup.Call("entity.update", Json{ { "entity", "/Lamp/Bulb" }, { "name", "Globe" } }).has_value());
			const Result<Json> applied = setup.Call("prefab.apply", Json{ { "instance", "/Lamp" } });
			REQUIRE_MESSAGE(applied.has_value(), applied.error().ToString());

			// The second scene's instance followed, recorded in its own history.
			const Json inSecond{ { "scene", "Assets/Scenes/Second.scene" } };
			Json tree = inSecond;
			tree["format"] = "json";
			const Result<Json> secondTree = setup.Call("scene.tree", tree);
			REQUIRE(secondTree.has_value());
			std::vector<std::string> names;
			for (const Json& entity : (*secondTree)["entities"])
				names.push_back(OpenScenesName(entity));
			CHECK(names == std::vector<std::string>{ "Copy", "Globe" });
			const Result<Json> afterHistory = setup.Call("edit.history", inSecond);
			REQUIRE(afterHistory.has_value());
			REQUIRE((*afterHistory)["entries"].size() == secondEntries + 1);
			CHECK(JsonReader((*afterHistory)["entries"].back()["label"]).ReadString().value_or(std::string()).contains("Update Prefab Instances"));

			// Undo there reverts only the second scene; Main keeps its applied edit.
			REQUIRE(setup.Call("edit.undo", inSecond).has_value());
			const Result<Json> undone = setup.Call("scene.tree", tree);
			REQUIRE(undone.has_value());
			CHECK(OpenScenesName((*undone)["entities"][1]) == "Bulb");
			CHECK(setup.GetEditor().GetScene().FindEntityByPath("/Lamp/Globe").IsValid());
		}

		// Decision 15 (the milestone review, T1): a new version of a prefab that only another open scene instantiates reaches
		// that scene, as an entry of its own history, whether an asset.reimport or an import setting gives it that version.
		TEST_CASE("EditorContext: a reimport and an import setting of a glTF that only another open scene instantiates rebuild that scene")
		{
			Test::AutomationFixture setup("OpenScenesGltfVersions");
			EditorContext& editor = setup.GetEditor();
			const Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/Hierarchy.gltf"));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			const VfsPath gltf = OpenScenesPath("project://Assets/Models/Hierarchy.gltf");
			REQUIRE(editor.CreateProjectDirectory(gltf.GetParent()).has_value());
			REQUIRE(editor.WriteProjectFile(gltf, *bytes).has_value());
			REQUIRE(setup.Call("project.refreshAssets", Json::object()).has_value());
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }).has_value());
			REQUIRE(setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Models/Hierarchy.gltf" } }).has_value());
			REQUIRE(setup.Call("scene.activate", Json{ { "scene", main } }).has_value());
			const Json inSecond{ { "scene", "Assets/Scenes/Second.scene" } };
			const auto baseHeight = [&setup, &inSecond]()
			{
				Json params = inSecond;
				params["entity"] = "/Hierarchy/Base";
				const Result<Json> base = setup.Call("entity.get", params);
				REQUIRE_MESSAGE(base.has_value(), base.error().ToString());
				return JsonReader((*base)["entity"]["components"]["Transform"]["Translation"][1]).ReadFloat().value_or(-1.0f);
			};
			const auto history = [&setup, &inSecond]()
			{
				const Result<Json> entries = setup.Call("edit.history", inSecond);
				REQUIRE_MESSAGE(entries.has_value(), entries.error().ToString());
				return (*entries)["entries"];
			};
			const size_t entries = history().size();
			CHECK(baseHeight() == doctest::Approx(1.0f));

			// Another program moves the Base node up, and asset.reimport, while Main is active, rebuilds Second.
			const std::filesystem::path file = setup.GetEditorFixture().GetProjectRoot() / "Assets/Models/Hierarchy.gltf";
			const Result<std::string> text = FileSystem::ReadText(file);
			REQUIRE(text.has_value());
			Result<Json> document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			(*document)["nodes"][0]["translation"][1] = 2.0;
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(document->dump(2)), {}).has_value());
			const Result<Json> reimported = setup.Call("asset.reimport", Json{ { "asset", "Assets/Models/Hierarchy.gltf" } });
			REQUIRE_MESSAGE(reimported.has_value(), reimported.error().ToString());
			CHECK(baseHeight() == doctest::Approx(2.0f));
			Json after = history();
			REQUIRE(after.size() == entries + 1);
			CHECK(JsonReader(after.back()["label"]).ReadString().value_or(std::string()).contains("Update Prefab Instances"));

			// An import setting halves the model, in an undo step of Main's history, and Second follows in an entry of its own.
			const Result<Json> scaled = setup.Call("asset.setImportSettings", Json{ { "asset", "Assets/Models/Hierarchy.gltf" }, { "settings", Json{ { "Scale", 0.5 } } } });
			REQUIRE_MESSAGE(scaled.has_value(), scaled.error().ToString());
			CHECK(baseHeight() == doctest::Approx(1.0f));
			after = history();
			REQUIRE(after.size() == entries + 2);
			CHECK(JsonReader(after.back()["label"]).ReadString().value_or(std::string()).contains("Update Prefab Instances"));
			CHECK(editor.GetActiveSceneId() == main);
		}

		// Decision 15 with decision 8's nesting (the milestone review, T1): an edit applied at the Lamp level of a lamp nested
		// in a room reaches another open scene that holds only a room, whose lamp follows Lamp.prefab through Room.prefab.
		TEST_CASE("EditorContext: an applied edit of a nested prefab reaches another open scene that holds only the prefab nesting it")
		{
			Test::AutomationFixture setup("OpenScenesNestedApply");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Room" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Lamp" }, { "parent", "/Room" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/Room/Lamp" }, { "components", Json{ { "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Cube" } } } } } })
					.has_value());
			REQUIRE(setup.Call("prefab.create", Json{ { "entity", "/Room/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } }).has_value());
			REQUIRE(setup.Call("prefab.create", Json{ { "entity", "/Room" }, { "path", "Assets/Prefabs/Room.prefab" }, { "replaceWithInstance", true } }).has_value());
			const OpenSceneId main = setup.GetEditor().GetActiveSceneId();
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }).has_value());
			REQUIRE(setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Room.prefab" }, { "name", "Room2" } }).has_value());
			const Json inSecond{ { "scene", "Assets/Scenes/Second.scene" } };
			const Result<Json> before = setup.Call("edit.history", inSecond);
			REQUIRE(before.has_value());
			REQUIRE(setup.Call("scene.activate", Json{ { "scene", main } }).has_value());

			REQUIRE(setup.Call("entity.update", Json{ { "entity", "/Room/Lamp/Bulb" }, { "components", Json{ { "MeshRenderer", Json{ { "CastShadows", false } } } } } }).has_value());
			const Result<Json> applied = setup.Call("prefab.apply", Json{ { "instance", "/Room/Lamp" }, { "level", "/Room/Lamp" } });
			REQUIRE_MESSAGE(applied.has_value(), applied.error().ToString());
			CHECK((*applied)["prefab"]["path"] == Json("Assets/Prefabs/Lamp.prefab"));
			Json bulb = inSecond;
			bulb["entity"] = "/Room2/Lamp/Bulb";
			const Result<Json> followed = setup.Call("entity.get", bulb);
			REQUIRE_MESSAGE(followed.has_value(), followed.error().ToString());
			CHECK((*followed)["entity"]["components"]["MeshRenderer"]["CastShadows"] == Json(false));
			const Result<Json> after = setup.Call("edit.history", inSecond);
			REQUIRE(after.has_value());
			REQUIRE((*after)["entries"].size() == (*before)["entries"].size() + 1);
			CHECK(JsonReader((*after)["entries"].back()["label"]).ReadString().value_or(std::string()).contains("Update Prefab Instances"));
		}

		// Decision 15 (the milestone review, M1): undoing another scene's "Update Prefab Instances" entry leaves its instance
		// as it was before the update, which differs from the prefab. The next prefab update records that difference as the
		// scene's overrides against the prefab's version before the write, as the addressed scene's are, so the instance keeps
		// it and still follows the prefab's other changes.
		TEST_CASE("EditorContext: an open scene that undid a prefab update keeps its own value through the next update")
		{
			Test::AutomationFixture setup("OpenScenesPrefabUndoneUpdate");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Lamp" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/Lamp" } }).has_value());
			REQUIRE(setup.Call("prefab.create", Json{ { "entity", "/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } }).has_value());
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }).has_value());
			REQUIRE(setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Lamp.prefab" }, { "name", "Copy" } }).has_value());
			const Json main{ { "scene", "Assets/Scenes/Main.scene" } };
			const Json inSecond{ { "scene", "Assets/Scenes/Second.scene" } };
			const auto inMain = [&main](Json params)
			{
				params.update(main);
				return params;
			};

			// Main renames the bulb and applies it; Second follows, then undoes its entry and keeps the old name.
			REQUIRE(setup.Call("entity.update", inMain(Json{ { "entity", "/Lamp/Bulb" }, { "name", "Globe" } })).has_value());
			REQUIRE(setup.Call("prefab.apply", inMain(Json{ { "instance", "/Lamp" } })).has_value());
			REQUIRE(setup.Call("edit.undo", inSecond).has_value());
			Scene* second = setup.GetEditor().FindScene(setup.GetEditor().FindOpenScene(OpenScenesPath("project://Assets/Scenes/Second.scene")));
			REQUIRE(second != nullptr);
			REQUIRE(second->FindEntityByPath("/Copy/Bulb").IsValid());

			// Main moves the bulb up and applies that: Second's instance keeps its name and takes the move.
			REQUIRE(setup.Call("entity.update", inMain(Json{ { "entity", "/Lamp/Globe" }, { "components", Json{ { "Transform", Json{ { "Translation", { 0, 2, 0 } } } } } } }))
					.has_value());
			const Result<Json> applied = setup.Call("prefab.apply", inMain(Json{ { "instance", "/Lamp" } }));
			REQUIRE_MESSAGE(applied.has_value(), applied.error().ToString());
			second = setup.GetEditor().FindScene(setup.GetEditor().FindOpenScene(OpenScenesPath("project://Assets/Scenes/Second.scene")));
			REQUIRE(second != nullptr);
			CHECK_FALSE(second->FindEntityByPath("/Copy/Globe").IsValid());
			Json bulb = inSecond;
			bulb["entity"] = "/Copy/Bulb";
			const Result<Json> kept = setup.Call("entity.get", bulb);
			REQUIRE_MESSAGE(kept.has_value(), kept.error().ToString());
			CHECK((*kept)["entity"]["components"]["Transform"]["Translation"] == Json::array({ 0, 2, 0 }));
		}

		// Decision 15 under decision 16's scene target: a prefab update made by a request that targets an open scene other
		// than the active one still reaches every other open scene, the active one included, while the request's scope keeps
		// addressing its target; and the undo of that update, run inside the targeted scene's own history, makes them follow
		// again, each in an entry of its own history.
		TEST_CASE("EditorContext: a prefab update of a targeted request rebuilds the other open scenes, also when it is undone")
		{
			Test::AutomationFixture setup("OpenScenesTargetedPrefabUpdate");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Lamp" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/Lamp" } }).has_value());
			REQUIRE(setup.Call("prefab.create", Json{ { "entity", "/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } }).has_value());
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }).has_value());
			REQUIRE(setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Lamp.prefab" }, { "name", "Copy" } }).has_value());
			EditorContext& editor = setup.GetEditor();
			const OpenSceneId second = editor.GetActiveSceneId();
			const size_t secondEntries = editor.GetHistory().GetUndoCount();
			const auto bulbName = [&editor]()
			{
				const Entity copy = editor.GetScene().FindEntityByPath("/Copy");
				REQUIRE(copy.IsValid());
				REQUIRE(copy.GetChildren().size() == 1);
				return editor.GetScene().FindEntityByID(copy.GetChildren().front()).GetName();
			};

			// Second stays active while Main's requests rename its bulb and apply the rename to the prefab.
			const Json inMain{ { "scene", "Assets/Scenes/Main.scene" } };
			Json rename = inMain;
			rename["entity"] = "/Lamp/Bulb";
			rename["name"] = "Globe";
			REQUIRE(setup.Call("entity.update", rename).has_value());
			Json apply = inMain;
			apply["instance"] = "/Lamp";
			const Result<Json> applied = setup.Call("prefab.apply", apply);
			REQUIRE_MESSAGE(applied.has_value(), applied.error().ToString());
			REQUIRE(editor.GetActiveSceneId() == second);
			CHECK(bulbName() == "Globe");
			CHECK(editor.GetHistory().GetUndoCount() == secondEntries + 1);
			CHECK(editor.GetHistory().GetUndoLabel().contains("Update Prefab Instances"));

			// Undoing the apply in Main restores the prefab, and Second follows it back in a further entry of its own.
			const Result<Json> undone = setup.Call("edit.undo", inMain);
			REQUIRE_MESSAGE(undone.has_value(), undone.error().ToString());
			REQUIRE(editor.GetActiveSceneId() == second);
			CHECK(bulbName() == "Bulb");
			CHECK(editor.GetHistory().GetUndoCount() == secondEntries + 2);
			const Scene* main = std::as_const(editor).FindScene(editor.FindOpenScene(OpenScenesPath("project://Assets/Scenes/Main.scene")));
			REQUIRE(main != nullptr);
			CHECK(main->FindEntityByPath("/Lamp/Globe").IsValid());
		}
	}

}
