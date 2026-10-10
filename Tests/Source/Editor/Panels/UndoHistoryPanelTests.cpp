#include "TestsPCH.h"
#include "Editor/Panels/UndoHistoryPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "Engine/Scene/Scene.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("UndoHistoryPanel: repaired scenes stay unsaved after undo until the scene is saved")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			EditorContext& editor = fixture.GetEditor();
			const auto path = editor.GetScenePath();
			REQUIRE(path.has_value());
			editor.SetScene(editor.CreateScene("Repaired"), path, true);
			REQUIRE_FALSE(editor.GetHistory().IsDirty());
			REQUIRE(editor.IsSceneDirty());
			UndoHistoryPanel panel;
			const std::string repaired = fixture.Draw(panel);
			CHECK(repaired.contains("Unsaved scene changes"));
			CHECK_FALSE(repaired.contains("Scene matches the saved state"));
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "Change after repair" } }));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Undo");
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			fixture.Pump();
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			CHECK_FALSE(editor.GetHistory().IsDirty());
			CHECK(editor.IsSceneDirty());
			CHECK(fixture.Draw(panel).contains("Unsaved scene changes"));
			REQUIRE(fixture.GetClient().Call("scene.save", Json::object()));
			CHECK_FALSE(editor.IsSceneDirty());
			const std::string saved = fixture.Draw(panel);
			CHECK(saved.contains("Scene matches the saved state"));
			CHECK_FALSE(saved.contains("Unsaved scene changes"));
		}

		TEST_CASE("UndoHistoryPanel: project settings history stays visible and undoable without a scene")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			EditorContext& editor = fixture.GetEditor();
			editor.CloseScene();
			REQUIRE_FALSE(editor.HasScene());
			const std::string original = editor.GetProject().GetSettings().Window.Title;
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "Window", Json{ { "Title", "Project history title" } } } } } }));
			const auto entries = editor.GetHistory().GetEntries(1);
			REQUIRE(entries.size() == 1);
			REQUIRE_FALSE(entries.front().Label.empty());
			UndoHistoryPanel panel;
			const std::string applied = fixture.Draw(panel);
			CHECK(applied.contains(entries.front().Label));
			CHECK(applied.contains("Agent / Applied"));
			CHECK(applied.contains("1 applied"));
			CHECK_FALSE(applied.contains("No scene open"));
			CHECK_FALSE(applied.contains("Scene matches the saved state"));
			CHECK_FALSE(applied.contains("Unsaved scene changes"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Undo");
			CHECK(editor.GetProject().GetSettings().Window.Title == "Project history title");
			fixture.Pump();
			CHECK(editor.GetProject().GetSettings().Window.Title == original);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			CHECK(editor.GetHistory().GetRedoCount() == 1);
			const std::string undone = fixture.Draw(panel);
			CHECK(undone.contains(entries.front().Label));
			CHECK(undone.contains("Agent / Undone"));
			CHECK(undone.contains("Current position"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Redo");
			CHECK(editor.GetProject().GetSettings().Window.Title == original);
			fixture.Pump();
			CHECK(editor.GetProject().GetSettings().Window.Title == "Project history title");
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK(editor.GetHistory().GetRedoCount() == 0);
			CHECK(fixture.Draw(panel).contains("Agent / Applied"));
			CHECK_FALSE(editor.HasScene());
		}

		TEST_CASE("UndoHistoryPanel: human undo is queued and preserves agent labels")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "History target" } }));
			UndoHistoryPanel panel;
			const std::string text = fixture.Draw(panel);
			CHECK(text.contains("[agent]"));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const size_t before = fixture.GetEditor().GetHistory().GetUndoCount();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Undo");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Pump();
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() + 1 == before);
			CHECK(fixture.GetEditor().GetHistory().GetRedoCount() == 1);
			CHECK(fixture.Draw(panel).contains("Current position"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Redo");
			CHECK(fixture.GetEditor().GetHistory().GetRedoCount() == 1);
			fixture.Pump();
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == before);
			CHECK(fixture.GetEditor().GetHistory().GetRedoCount() == 0);
		}
	}

}
