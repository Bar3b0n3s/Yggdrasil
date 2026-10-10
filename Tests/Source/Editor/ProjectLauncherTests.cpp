#include "TestsPCH.h"
#include "Editor/ProjectLauncher.h"

#include "Editor/AssetDesignInteraction.h"
#include "Editor/PanelInteractionFixture.h"
#include "Engine/Core/FileSystem.h"

#include <vector>

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("ProjectLauncher: stale recent paths and matching authored suffixes remain browsable")
		{
			Test::PanelInteractionFixture fixture("LauncherStaleRecent", false);
			auto& context = fixture.GetContext();
			REQUIRE(context.Editor.CloseProject());
			const auto& directory = fixture.GetAutomation().GetEditorFixture().GetDirectory();
			std::vector<std::filesystem::path> projects;
			for (const char* folder : { "A###Shared", "B###Shared" })
			{
				const auto created = ProjectManager::CreateProject({ .Directory = directory / folder, .Name = "Demo", .Template = ProjectTemplate::Empty, .TemplatesDirectory = context.Editor.GetSpecification().TemplatesDirectory }, context.Editor.GetTypeRegistry());
				REQUIRE(created);
				projects.push_back(created->ProjectFile);
				REQUIRE(ProjectManager::AddRecentProject(context.Editor.GetVfs(), created->ProjectFile));
			}
			const auto stale = directory / "Gone" / "Missing" / "Demo.eproj";
			REQUIRE(ProjectManager::AddRecentProject(context.Editor.GetVfs(), stale));
			const auto recent = ProjectManager::ReadRecentProjects(context.Editor.GetVfs());
			REQUIRE(recent);
			REQUIRE_FALSE(recent->empty());
			CHECK(recent->front() == stale);
			ProjectLauncher launcher;
			Test::AssetDesignInteraction ui;
			const auto draw = [&launcher, &context]()
			{
				return launcher.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			auto* landing = ui.Child(root, root->GetID("ProjectLanding"));
			SUBCASE("browse walks to an accessible parent and opens a literal folder name")
			{
				const ImGuiID tab = ImHashStr("Open project", 0, landing->GetID("ProjectChoices"));
				ui.Click(draw, landing, tab);
				ui.Click(draw, landing, ImHashStr("Browse projects...", 0, tab));
				auto* picker = ui.Window("###FolderPicker");
				auto* entries = ui.Child(picker, picker->GetID("Entries"));
				const ImGuiID first = ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "A###Shared"));
				const ImGuiID second = ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "B###Shared"));
				const ImRect firstRow = ui.Locate(draw, entries, first);
				const ImRect secondRow = ui.Locate(draw, entries, second);
				CHECK(first != second);
				CHECK(firstRow.GetCenter().y != secondRow.GetCenter().y);
				ui.Click(draw, entries, second);
				const auto unchanged = ProjectManager::ReadRecentProjects(context.Editor.GetVfs());
				REQUIRE(unchanged);
				CHECK(*unchanged == *recent);
				ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Demo.eproj")));
				ui.Click(draw, picker, "Open file");
			}
			SUBCASE("recent project IDs include the full authored path")
			{
				const ImGuiID tab = ImHashStr("Recent", 0, landing->GetID("ProjectChoices"));
				auto* entries = ui.Child(landing, ImHashStr("RecentProjects", 0, tab));
				const ImGuiID first = ImHashStr("##RecentProject", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, FileSystem::PathToUtf8(projects[0])));
				const ImGuiID second = ImHashStr("##RecentProject", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, FileSystem::PathToUtf8(projects[1])));
				const ImRect firstRow = ui.Locate(draw, entries, first);
				const ImRect secondRow = ui.Locate(draw, entries, second);
				CHECK(first != second);
				CHECK(firstRow.GetCenter().y != secondRow.GetCenter().y);
				ui.Click(draw, entries, second);
			}
			CHECK_FALSE(context.Editor.HasProject());
			context.Actions.Pump();
			REQUIRE(context.Editor.HasProject());
			CHECK(context.Editor.GetProject().GetProjectFile() == projects[1]);
		}

		TEST_CASE("ProjectLauncher: entering an Empty project queues creation until the safe point")
		{
			Test::PanelInteractionFixture fixture("LauncherCreate", false);
			auto& context = fixture.GetContext();
			REQUIRE(context.Editor.CloseProject());
			const auto directory = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "UiProject";
			ProjectLauncher launcher;
			Test::AssetDesignInteraction ui;
			const auto draw = [&launcher, &context]()
			{
				return launcher.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			auto* landing = ui.Child(root, root->GetID("ProjectLanding"));
			const ImGuiID tab = ImHashStr("New project", 0, landing->GetID("ProjectChoices"));
			ui.Click(draw, landing, tab);
			ui.Type(draw, landing, ImHashStr("##ProjectName", 0, tab), "UiProject");
			ui.Type(draw, landing, ImHashStr("##ProjectLocation", 0, tab), FileSystem::PathToUtf8(directory.parent_path()).c_str());
			const int emptyIndex = 1;
			const ImGuiID templateScope = ImHashData(&emptyIndex, sizeof(emptyIndex), ImHashStr("Templates", 0, tab));
			ui.Click(draw, landing, ImHashStr("Empty project", 0, templateScope));
			ui.Click(draw, landing, ImHashStr("Create project", 0, tab));
			CHECK_FALSE(context.Editor.HasProject());
			CHECK_FALSE(FileSystem::Exists(directory / "UiProject.eproj"));
			context.Actions.Pump();
			REQUIRE(context.Editor.HasProject());
			CHECK(context.Editor.GetProject().GetProjectFile() == directory / "UiProject.eproj");
			CHECK(FileSystem::Exists(directory / "UiProject.eproj"));
			CHECK_FALSE(context.Editor.HasScene());
			const auto recent = ProjectManager::ReadRecentProjects(context.Editor.GetVfs());
			REQUIRE(recent);
			REQUIRE_FALSE(recent->empty());
			CHECK(recent->front() == directory / "UiProject.eproj");
		}
	}

}
