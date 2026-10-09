#include "TestsPCH.h"
#include "Editor/ProjectLauncher.h"

#include "Editor/PanelInteractionFixture.h"
#include "Engine/Core/FileSystem.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("ProjectLauncher: entering an Empty project queues creation until the safe point")
		{
			Test::PanelInteractionFixture fixture("LauncherCreate", false);
			auto& context = fixture.GetContext();
			REQUIRE(context.Editor.CloseProject());
			const auto directory = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "CreatedFromUi";
			ProjectLauncher launcher;
			Test::PanelInteractionUi ui;
			ImVec2 origin{};
			const auto draw = [&launcher, &context, &origin]()
			{
				origin = ImGui::GetCursorScreenPos();
				return launcher.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			const float textRow = ImGui::GetTextLineHeightWithSpacing();
			const float frame = ImGui::GetFrameHeightWithSpacing();
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, origin.y + textRow + 8.0f)));
			ImGui::GetIO().AddInputCharactersUTF8(FileSystem::PathToUtf8(directory).c_str());
			REQUIRE(ui.Frame(draw));
			const float nameY = origin.y + textRow + 2.0f * frame + 1.0f + ImGui::GetStyle().ItemSpacing.y + 8.0f;
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, nameY)));
			ImGui::GetIO().AddInputCharactersUTF8("UiProject");
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 8.0f, nameY + frame)));
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 45.0f, nameY + 2.0f * frame + textRow)));
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
