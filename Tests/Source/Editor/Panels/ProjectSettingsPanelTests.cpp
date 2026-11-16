#include "TestsPCH.h"
#include "Editor/Panels/ProjectSettingsPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Session/PlayInput.h"
#include "Support/ExpectLog.h"

#include <format>
#include <string>
#include <vector>

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

		// M34 review finding 7 (Docs/Decisions/0049-m34-characters-input.md decision 6): "Add element" commits an empty
		// binding at once; the panel's project.setSettings runs as the user, so it commits, and the panel shows the binding's
		// problem beside it until it is filled in.
		TEST_CASE("ProjectSettingsPanel: an input binding added empty commits and shows its problem beside it until it is filled in")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			const Json jump{ { "Type", "Button" }, { "Bindings", Json::array({ "Key.Space" }) } };
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "Input", Json{ { "Actions", Json{ { "Jump", jump } } } } } } } }));
			const auto jumpBindings = [&fixture]()
			{
				const auto& actions = fixture.GetEditor().GetProject().GetSettings().Input.Actions;
				const auto found = actions.find("Jump");
				return found == actions.end() ? std::vector<std::string>{} : found->second.Bindings;
			};
			// The problem play.start would report for the empty binding, which the panel shows as a note: its message and, in
			// brackets, its hint (an error line would read "hint: " instead).
			InputSettings added = fixture.GetEditor().GetProject().GetSettings().Input;
			added.Actions["Jump"].Bindings.push_back("");
			const Status problems = ValidateInputActions(added);
			REQUIRE_FALSE(problems.has_value());
			REQUIRE(problems.error().GetIssues().size() == 1);
			const ErrorIssue& issue = problems.error().GetIssues().front();
			const std::string problem = issue.Hint.empty() ? issue.Message : std::format("{} ({})", issue.Message, issue.Hint);

			ProjectSettingsPanel panel;
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "General");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Input");
			CHECK_FALSE(fixture.Draw(panel).contains(problem));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Add element");
			fixture.Pump();
			CHECK(fixture.Draw(panel).contains(problem));
			CHECK(jumpBindings() == std::vector<std::string>{ "Key.Space", "" });
			CHECK(fixture.GetEditor().GetHistory().GetEntries(1).front().Origin == CommandOrigin::User);

			const Json filled{ { "Type", "Button" }, { "Bindings", Json::array({ "Key.Space", "Key.J" }) } };
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "Input", Json{ { "Actions", Json{ { "Jump", filled } } } } } } } }));
			CHECK_FALSE(fixture.Draw(panel).contains(problem));
			CHECK(jumpBindings() == std::vector<std::string>{ "Key.Space", "Key.J" });
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
