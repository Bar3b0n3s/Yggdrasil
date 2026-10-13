#include "TestsPCH.h"
#include "Editor/Panels/ProjectSettingsPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "Engine/Core/FileSystem.h"
#include "Support/ExpectLog.h"

#include <string>

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("ProjectSettingsPanel: an intervening edit discards the active settings draft")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, fixture.GetEditor().GetProject().GetSettings().Name);
			fixture.ReplaceFocusedText(panel, "Uncommitted draft");
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "Name", "Agent setting" } } } }));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const Test::ExpectLog conflict(LogLevel::Error, "project changed while editing settings");
			CHECK(fixture.Draw(panel).contains("draft discarded"));
			fixture.Press(panel, ImGuiKey_Enter);
			fixture.Pump();
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == "Agent setting");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			CHECK(conflict.GetMatchCount() == 1);
		}

		TEST_CASE("ProjectSettingsPanel: reflected text commits one queued human settings command")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			fixture.Draw(panel);
			const auto before = fixture.GetEditor().GetProject().GetSettings();
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const size_t history = fixture.GetEditor().GetHistory().GetUndoCount();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, before.Name);
			fixture.ReplaceFocusedText(panel, "Human settings name");
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == before.Name);
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Press(panel, ImGuiKey_Enter);
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Pump();
			fixture.Draw(panel);
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == "Human settings name");
			CHECK(fixture.GetEditor().GetProject().GetSettings().Window.Title == before.Window.Title);
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == history + 1);
			const auto entries = fixture.GetEditor().GetHistory().GetEntries(1);
			REQUIRE(entries.size() == 1);
			CHECK(entries[0].Origin == CommandOrigin::User);
			REQUIRE(fixture.GetClient().Call("edit.undo", Json::object()));
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == before.Name);
		}

		TEST_CASE("ProjectSettingsPanel: a project change discards the pending settings action")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			fixture.Draw(panel);
			const uint64_t probe = fixture.ProbeActionTicket();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, fixture.GetEditor().GetProject().GetSettings().Name);
			fixture.ReplaceFocusedText(panel, "Abandoned settings name");
			fixture.Press(panel, ImGuiKey_Enter);
			const std::optional<uint64_t> ticket = fixture.FindPendingAction(probe);
			REQUIRE(ticket.has_value());
			REQUIRE(fixture.GetEditor().CloseProject());
			fixture.Draw(panel);
			// Discarded, not merely cancelled: no Cancelled completion is left behind for a ticket nobody will take.
			const auto abandoned = fixture.GetContext().Actions.TakeResult(*ticket);
			REQUIRE_FALSE(abandoned.has_value());
			CHECK(abandoned.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("ProjectSettingsPanel: drawing reflected settings does not mutate the project")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			const auto before = FileSystem::ReadText(fixture.GetEditor().GetProject().GetProjectFile());
			REQUIRE(before);
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const std::string text = fixture.Draw(panel);
			CHECK(text.contains("General"));
			CHECK(text.contains("Name"));
			CHECK(text.contains("Start Scene"));
			fixture.Pump();
			fixture.Draw(panel);
			CHECK(fixture.GetEditor().GetRevision() == revision);
			const auto after = FileSystem::ReadText(fixture.GetEditor().GetProject().GetProjectFile());
			REQUIRE(after);
			CHECK(*after == *before);
		}

		TEST_CASE("ProjectSettingsPanel: category navigation retains every reflected category without writing")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const auto before = FileSystem::ReadText(fixture.GetEditor().GetProject().GetProjectFile());
			REQUIRE(before);
			CHECK_FALSE(fixture.Draw(panel).contains("Gravity"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "General");
			const std::string categories = fixture.Draw(panel);
			for (const char* category : { "Window", "Simulation", "Physics", "Input", "Rendering", "Scripting", "Export", "Testing" })
				CHECK_MESSAGE(categories.contains(category), category);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Physics");
			const std::string physics = fixture.Draw(panel);
			CHECK(physics.contains("Gravity"));
			CHECK(physics.contains("Collisions"));
			CHECK_FALSE(physics.contains("Start scene"));
			fixture.Pump();
			CHECK(fixture.GetEditor().GetRevision() == revision);
			const auto after = FileSystem::ReadText(fixture.GetEditor().GetProject().GetProjectFile());
			REQUIRE(after);
			CHECK(*after == *before);
		}

		TEST_CASE("ProjectSettingsPanel: text edits queue one undoable human command")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			fixture.Draw(panel); // Settle the reflected table layout before measuring its input text.
			const auto before = fixture.GetEditor().GetProject().GetSettings();
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const size_t history = fixture.GetEditor().GetHistory().GetUndoCount();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, before.Name);
			fixture.ReplaceFocusedText(panel, "Supporting human settings");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Press(panel, ImGuiKey_Enter);
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Pump();
			fixture.Draw(panel);
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == "Supporting human settings");
			CHECK(fixture.GetEditor().GetProject().GetSettings().Window.Title == before.Window.Title);
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == history + 1);
			const auto entries = fixture.GetEditor().GetHistory().GetEntries(1);
			REQUIRE(entries.size() == 1);
			CHECK(entries.front().Origin == CommandOrigin::User);
			REQUIRE(fixture.GetClient().Call("edit.undo", Json::object()));
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == before.Name);
		}

		TEST_CASE("ProjectSettingsPanel: an intervening agent edit discards the typed draft with a conflict")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			fixture.Draw(panel); // Match the initial layout frame used by the legacy settings interaction tests.
			Test::SupportingPanelTestUi::ClickText(fixture, panel, fixture.GetEditor().GetProject().GetSettings().Name);
			fixture.ReplaceFocusedText(panel, "Uncommitted supporting draft");
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "Name", "Agent settings" } } } }));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const Test::ExpectLog conflict(LogLevel::Error, "project changed while editing settings");
			CHECK(fixture.Draw(panel).contains("draft discarded"));
			fixture.Press(panel, ImGuiKey_Enter);
			fixture.Pump();
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == "Agent settings");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			CHECK(conflict.GetMatchCount() == 1);
		}
	}

}
