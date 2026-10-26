#include "TestsPCH.h"
#include "EditorCore/EditorActions.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string_view>

namespace Engine {

	static Result<Json> CompletePanelAction(EditorActions& actions, std::string_view method, const Json& params)
	{
		ENGINE_TRY_ASSIGN(uint64_t ticket, actions.Submit(method, params));
		actions.Pump();
		ENGINE_TRY_ASSIGN(auto result, actions.TakeResult(ticket));
		REQUIRE(result.has_value());
		return std::move(*result);
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("CommandHistory: UI and agent commands interleave in one history")
		{
			Test::AutomationFixture fixture("PanelHistory");
			EditorContext& editor = fixture.GetEditor();
			EditorActions actions(editor, fixture.GetClient().GetServer());
			REQUIRE(CompletePanelAction(actions, "entity.create", Json{ { "name", "Human" } }));
			REQUIRE(fixture.Call("entity.create", Json{ { "name", "Agent" } }));
			REQUIRE(CompletePanelAction(actions, "entity.create", Json{ { "name", "HumanAgain" } }));
			const auto entries = editor.GetHistory().GetEntries(10);
			REQUIRE(entries.size() == 3);
			CHECK(entries[0].Origin == CommandOrigin::User);
			CHECK(entries[1].Origin == CommandOrigin::Agent);
			CHECK(entries[2].Origin == CommandOrigin::User);
			CHECK(entries[1].Label.starts_with("[agent]"));
			CHECK_FALSE(entries[0].Label.starts_with("[agent]"));
			REQUIRE(editor.GetHistory().Undo(editor, 3));
			CHECK(editor.GetScene().GetRootEntities().empty());
			REQUIRE(editor.GetHistory().Redo(editor, 3));
			CHECK(editor.GetScene().GetRootEntities().size() == 3);
		}

		TEST_CASE("EditorActions: UI writes are attributed to ui and never agent")
		{
			Test::AutomationFixture fixture("PanelProvenance");
			EditorActions actions(fixture.GetEditor(), fixture.GetClient().GetServer());
			REQUIRE(CompletePanelAction(actions, "entity.create", Json{ { "name", "Human" } }));
			REQUIRE(CompletePanelAction(actions, "scene.save", Json::object()));
			auto path = VfsPath::Parse("project://Automation/Provenance.json");
			REQUIRE(path);
			auto text = fixture.GetEditor().GetVfs().ReadText(*path);
			REQUIRE(text);
			auto provenance = ProvenanceRecorder::FromText(*text);
			REQUIRE(provenance);
			bool foundScene = false;
			for (const auto& entry : *provenance)
			{
				if (entry.Path != "Assets/Scenes/Main.scene")
					continue;
				foundScene = true;
				CHECK(entry.Method == "ui");
				CHECK(entry.Client == "ui");
				CHECK(entry.RequestId.Get().is_null());
				CHECK_FALSE(entry.TranscriptLine);
			}
			CHECK(foundScene);
			CHECK(fixture.GetEditor().GetCommandOrigin() == CommandOrigin::User);
		}

		TEST_CASE("EditorActions: invalid params and read-only writes fail before enqueue")
		{
			Test::AutomationFixture fixture("PanelAdmission");
			EditorContext& editor = fixture.GetEditor();
			EditorActions actions(editor, fixture.GetClient().GetServer());
			CHECK_FALSE(actions.Submit("entity.create", Json{ { "nmae", "Typo" } }));
			CHECK_FALSE(actions.Submit("missing.method", Json::object()));
			CHECK_FALSE(actions.Submit("entity.create", Json{ { "name", "Stale" }, { "ifRevision", editor.GetRevision() + 1 } }));
			const auto projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(editor.CloseProject());
			auto project = ProjectManager::OpenProject(projectFile,
				{ .ReadOnly = true, .ReadOnlyCacheDirectory = fixture.GetEditorFixture().GetDirectory() / "Private" }, editor.GetTypeRegistry());
			REQUIRE(project);
			REQUIRE(editor.OpenProject(std::move(*project)));
			const auto denied = actions.Submit("entity.create", Json{ { "name", "Denied" } });
			REQUIRE_FALSE(denied);
			CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
		}

		TEST_CASE("EditorActions: pending user actions cancel through the shared handler")
		{
			Test::AutomationFixture fixture("PanelPending");
			EditorActions actions(fixture.GetEditor(), fixture.GetClient().GetServer());
			auto ticket = actions.Submit("debug.pend", Json{ { "frames", 0 } });
			REQUIRE(ticket);
			actions.Pump();
			actions.Pump();
			auto pending = actions.TakeResult(*ticket);
			REQUIRE(pending);
			CHECK_FALSE(pending->has_value());
			REQUIRE(actions.Cancel(*ticket));
			auto cancelled = actions.TakeResult(*ticket);
			REQUIRE(cancelled);
			REQUIRE(cancelled->has_value());
			REQUIRE_FALSE(**cancelled);
			CHECK((**cancelled).error().GetCode() == ErrorCode::Cancelled);
			auto log = fixture.Call("log.read", Json{ { "contains", "Cancelled debug.pend of client 'ui'" } });
			REQUIRE(log);
			CHECK((*log)["entries"].size() == 1);
			CHECK_FALSE(actions.Cancel(*ticket));
		}

		TEST_CASE("EditorActions: discarding a ticket releases its entry and cancels its work")
		{
			Test::AutomationFixture fixture("PanelDiscard");
			EditorContext& editor = fixture.GetEditor();
			EditorActions actions(editor, fixture.GetClient().GetServer());
			auto queued = actions.Submit("entity.create", Json{ { "name", "Discarded" } });
			REQUIRE(queued);
			REQUIRE(actions.Discard(*queued));
			const auto forgotten = actions.TakeResult(*queued);
			REQUIRE_FALSE(forgotten);
			CHECK(forgotten.error().GetCode() == ErrorCode::NotFound);
			actions.Pump();
			CHECK(editor.GetScene().GetRootEntities().empty());
			auto pending = actions.Submit("debug.pend", Json{ { "frames", 0 } });
			REQUIRE(pending);
			actions.Pump();
			actions.Pump();
			const Test::ExpectLog cancelled(LogLevel::Info, "Cancelled debug.pend of client 'ui'");
			REQUIRE(actions.Discard(*pending));
			CHECK(cancelled.GetMatchCount() == 1);
			CHECK(actions.TakeResult(*pending).error().GetCode() == ErrorCode::NotFound);
			const auto unknown = actions.Discard(*pending);
			REQUIRE_FALSE(unknown);
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("EditorActions: a test run rejects UI edits, undo and redo until it finishes")
		{
			Test::EditorTestFixture fixture("PanelTestRunLease");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), std::move(specification));
			REQUIRE(client.Call("script.write", Json{ { "path", "Assets/Tests/Lease.test.luau" }, { "source", "return Test.Suite('Lease', function() Test.Case('long', function() Test.WaitTicks(1000) end) end)" } }));
			REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Lease.test.luau" }, { "Scene", "" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } }));
			EditorContext& editor = fixture.GetEditor();
			EditorActions actions(editor, client.GetServer());
			REQUIRE(CompletePanelAction(actions, "entity.create", Json{ { "name", "Created" } }));
			REQUIRE(CompletePanelAction(actions, "entity.create", Json{ { "name", "Undone" } }));
			REQUIRE(CompletePanelAction(actions, "edit.undo", Json::object()));
			const UUID created = editor.GetScene().FindEntityByPath("/Created").GetUUID();
			REQUIRE(editor.SetSelection({ created }, SceneTarget::Edit));
			const auto undo = editor.GetHistory().GetUndoCount();
			const auto redo = editor.GetHistory().GetRedoCount();
			const uint64_t revision = editor.GetRevision();
			// Queued before the run starts: rejected when the run already owns the editor at the safe point.
			auto queuedUndo = actions.Submit("edit.undo", Json::object());
			REQUIRE(queuedUndo);
			static_cast<void>(client.Submit("test.run", Json::object()));
			client.GetServer().Pump();
			REQUIRE(editor.GetPlay().IsTestRunActive());
			actions.Pump();
			auto queuedResult = actions.TakeResult(*queuedUndo);
			REQUIRE(queuedResult);
			REQUIRE(queuedResult->has_value());
			REQUIRE_FALSE(**queuedResult);
			CHECK((**queuedResult).error().GetCode() == ErrorCode::InvalidState);
			for (const std::string_view method : { "edit.undo", "edit.redo", "entity.create", "edit.select" })
			{
				CAPTURE(method);
				const Json params = method == "entity.create" ? Json{ { "name", "Rejected" } } : method == "edit.select" ? Json{ { "entities", Json::array() } }
																														 : Json::object();
				const auto rejected = actions.Submit(method, params);
				REQUIRE_FALSE(rejected);
				CHECK(rejected.error().GetCode() == ErrorCode::InvalidState);
				CHECK(rejected.error().GetMessageText().contains("a test run owns the editor"));
			}
			// Observation stays available, and the history itself refuses to move under the lease.
			CHECK(CompletePanelAction(actions, "scene.tree", Json::object()));
			const auto direct = editor.GetHistory().Undo(editor);
			REQUIRE_FALSE(direct);
			CHECK(direct.error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.GetHistory().GetUndoCount() == undo);
			CHECK(editor.GetHistory().GetRedoCount() == redo);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetScene().FindEntityByID(created).IsValid());
			client.GetServer().DisconnectInProcess(client.GetClient());
			REQUIRE_FALSE(editor.GetPlay().IsTestRunActive());
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == created);
			REQUIRE(CompletePanelAction(actions, "edit.redo", Json::object()));
			CHECK(editor.GetScene().FindEntityByPath("/Undone").IsValid());
		}

		TEST_CASE("EditorActions: pending contexts survive independent requests and poll once per pump")
		{
			Test::AutomationFixture fixture("PanelPendingLifetime");
			EditorActions actions(fixture.GetEditor(), fixture.GetClient().GetServer());
			auto ticket = actions.Submit("debug.pend", Json{ { "frames", 2 } });
			REQUIRE(ticket);
			actions.Pump();
			REQUIRE(fixture.Call("entity.create", Json{ { "name", "Between polls" } }));
			actions.Pump();
			auto pending = actions.TakeResult(*ticket);
			REQUIRE(pending);
			CHECK_FALSE(pending->has_value());
			actions.Pump();
			auto done = actions.TakeResult(*ticket);
			REQUIRE(done);
			REQUIRE(done->has_value());
			REQUIRE(**done);
			CHECK((***done)["frames"] == Json(2));
			CHECK_FALSE(actions.TakeResult(*ticket));
		}

		TEST_CASE("EditorActions: queued actions execute outside ImGui traversal")
		{
			Test::AutomationFixture fixture("PanelQueue");
			EditorContext& editor = fixture.GetEditor();
			EditorActions actions(editor, fixture.GetClient().GetServer());
			auto ticket = actions.Submit("entity.create", Json{ { "name", "Queued" } });
			REQUIRE(ticket);
			CHECK(editor.GetScene().GetRootEntities().empty());
			actions.Pump();
			CHECK(editor.GetScene().GetRootEntities().size() == 1);
			auto result = actions.TakeResult(*ticket);
			REQUIRE(result);
			REQUIRE(result->has_value());
			CHECK((**result).has_value());
			CHECK_FALSE(actions.TakeResult(*ticket));
			auto cancelled = actions.Submit("entity.create", Json{ { "name", "Cancelled" } });
			REQUIRE(cancelled);
			REQUIRE(actions.Cancel(*cancelled));
			actions.Pump();
			CHECK(editor.GetScene().GetRootEntities().size() == 1);
		}

		// Docs/Decisions/0031-m20-contract.md decisions 16 and 20: the scene tabs submit scene.activate and scene.close with a
		// scene target, so a UI action resolves and scopes its target as the automation server does. The target's open scene
		// is the one its revision check, dry-run copy, handler and history address, at Submit and again at the safe point,
		// and the active scene never changes under it.
		TEST_CASE("EditorActions: a scene target addresses its open scene for admission, the handler and its history")
		{
			Test::AutomationFixture fixture("PanelSceneTarget");
			EditorContext& editor = fixture.GetEditor();
			EditorActions actions(editor, fixture.GetClient().GetServer());
			REQUIRE(fixture.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }));
			const Result<VfsPath> mainPath = VfsPath::Parse("project://Assets/Scenes/Main.scene");
			REQUIRE(mainPath);
			const OpenSceneId main = editor.FindOpenScene(*mainPath);
			REQUIRE(main != NoOpenScene);
			const OpenSceneId second = editor.GetActiveSceneId();
			REQUIRE(second != main);
			const auto inMain = [](Json params)
			{
				params["scene"] = "Assets/Scenes/Main.scene";
				return params;
			};

			// The action lands in the target and its history, as the user's; the active scene and its history stay as they were.
			REQUIRE(CompletePanelAction(actions, "entity.create", inMain(Json{ { "name", "Targeted" } })));
			CHECK(editor.GetActiveSceneId() == second);
			CHECK_FALSE(editor.GetHistory().CanUndo());
			REQUIRE(editor.FindScene(main) != nullptr);
			CHECK(editor.FindScene(main)->FindEntityByPath("/Targeted").IsValid());
			const Result<OpenSceneInfo> targeted = editor.DescribeOpenScene(main);
			REQUIRE(targeted);
			CHECK(targeted->Dirty);

			// ifRevision is the target's: the active scene's revision is refused at Submit, the target's is accepted.
			const auto stale = actions.Submit("entity.create", inMain(Json{ { "name", "Stale" }, { "ifRevision", editor.GetRevision() } }));
			REQUIRE_FALSE(stale);
			CHECK(stale.error().GetCode() == ErrorCode::Conflict);
			REQUIRE(CompletePanelAction(actions, "entity.create", inMain(Json{ { "name", "Current" }, { "ifRevision", targeted->Revision } })));
			CHECK(editor.FindScene(main)->FindEntityByPath("/Current").IsValid());

			// A dry run copies the target and leaves it alone; undo moves the target's history only.
			REQUIRE(CompletePanelAction(actions, "entity.create", inMain(Json{ { "name", "Dry" }, { "dryRun", true } })));
			CHECK_FALSE(editor.FindScene(main)->FindEntityByPath("/Dry").IsValid());
			REQUIRE(CompletePanelAction(actions, "edit.undo", inMain(Json::object())));
			CHECK_FALSE(editor.FindScene(main)->FindEntityByPath("/Current").IsValid());
			CHECK(editor.FindScene(main)->FindEntityByPath("/Targeted").IsValid());
			CHECK(editor.GetActiveSceneId() == second);
			REQUIRE(editor.ActivateScene(main));
			const auto entries = editor.GetHistory().GetEntries(10);
			REQUIRE(entries.size() == 2);
			CHECK(entries[0].Origin == CommandOrigin::User);
			REQUIRE(editor.ActivateScene(second));

			// A target that names no open scene is refused at Submit, one closed after Submit at the safe point; a method
			// without the scene target refuses the member when it reads its params, as it refuses any unknown member.
			const auto missing = actions.Submit("scene.tree", Json{ { "scene", "Assets/Scenes/Missing.scene" } });
			REQUIRE_FALSE(missing);
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			const auto queued = actions.Submit("scene.tree", inMain(Json::object()));
			REQUIRE(queued);
			REQUIRE(fixture.Call("scene.close", inMain(Json{ { "discardChanges", true } })));
			actions.Pump();
			const auto closed = actions.TakeResult(*queued);
			REQUIRE(closed);
			REQUIRE(closed->has_value());
			REQUIRE_FALSE(**closed);
			CHECK((**closed).error().GetCode() == ErrorCode::NotFound);
			const auto untargeted = actions.Submit("asset.list", Json{ { "scene", second } });
			REQUIRE(untargeted);
			actions.Pump();
			const auto refused = actions.TakeResult(*untargeted);
			REQUIRE(refused);
			REQUIRE(refused->has_value());
			REQUIRE_FALSE(**refused);
			CHECK((**refused).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditorActions: admission is rechecked and dry runs leave no history")
		{
			Test::AutomationFixture fixture("PanelQueueRevision");
			EditorContext& editor = fixture.GetEditor();
			EditorActions actions(editor, fixture.GetClient().GetServer());
			auto stale = actions.Submit("entity.create", Json{ { "name", "Stale" }, { "ifRevision", editor.GetRevision() } });
			REQUIRE(stale);
			REQUIRE(fixture.Call("entity.create", Json{ { "name", "Agent" } }));
			actions.Pump();
			auto result = actions.TakeResult(*stale);
			REQUIRE(result);
			REQUIRE(result->has_value());
			REQUIRE_FALSE(**result);
			CHECK((**result).error().GetCode() == ErrorCode::Conflict);
			REQUIRE(CompletePanelAction(actions, "entity.create", Json{ { "name", "Dry" }, { "dryRun", true } }));
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK(editor.GetScene().GetRootEntities().size() == 1);
		}
	}

}
