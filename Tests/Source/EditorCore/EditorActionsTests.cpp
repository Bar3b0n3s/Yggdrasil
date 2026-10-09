#include "TestsPCH.h"
#include "EditorCore/EditorActions.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"

#include <doctest/doctest.h>

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
