#include "TestsPCH.h"
#include "Editor/Panels/SceneHierarchyPanel.h"

#include "Editor/PanelInteractionFixture.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/TransformSystem.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("SceneHierarchyPanel: create and control selection clicks queue human actions")
		{
			Test::PanelInteractionFixture fixture("HierarchyClicks");
			auto& context = fixture.GetContext();
			SceneHierarchyPanel panel;
			Test::PanelInteractionUi ui;
			ImVec2 origin{};
			const auto draw = [&panel, &context, &origin]()
			{
				origin = ImGui::GetCursorScreenPos();
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			const float frame = ImGui::GetFrameHeightWithSpacing();
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 45.0f, origin.y + frame + 8.0f)));
			CHECK(context.Editor.GetScene().GetEntityCount() == 0);
			context.Actions.Pump();
			REQUIRE(context.Editor.GetScene().GetEntityCount() == 1);
			const UUID first = context.Editor.GetScene().GetRootEntities().front();
			const auto created = context.Editor.GetHistory().GetEntries(10);
			REQUIRE(created.size() == 1);
			CHECK(created.front().Origin == CommandOrigin::User);
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(context.Editor.GetScene().GetEntityCount() == 0);
			REQUIRE(fixture.GetAutomation().Call("edit.redo", Json::object()));
			const UUID second = context.Editor.GetScene().CreateEntity("Second").GetUUID();
			REQUIRE(context.Editor.SetSelection({}, SceneTarget::Edit));
			const float firstY = origin.y + 2.0f * frame + 3.0f + ImGui::GetStyle().ItemSpacing.y + 6.0f;
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, firstY)));
			CHECK(context.Editor.GetSelection().empty());
			context.Actions.Pump();
			REQUIRE(context.Editor.GetSelection().size() == 1);
			CHECK(context.Editor.GetSelection().front() == first);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			const float row = ImGui::GetTextLineHeightWithSpacing() + 3.0f + ImGui::GetStyle().ItemSpacing.y;
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, firstY + row)));
			CHECK(context.Editor.GetSelection().size() == 1);
			context.Actions.Pump();
			const auto selection = context.Editor.GetSelection();
			REQUIRE(selection.size() == 2);
			CHECK(std::find(selection.begin(), selection.end(), second) != selection.end());
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, firstY)));
			context.Actions.Pump();
			REQUIRE(context.Editor.GetSelection().size() == 1);
			CHECK(context.Editor.GetSelection().front() == second);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
		}

		TEST_CASE("SceneHierarchyPanel: search retains matching descendants and drag reparent preserves world pose")
		{
			Test::PanelInteractionFixture fixture("HierarchySearchDrop");
			auto& context = fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID parent = scene.CreateEntity("Parent").GetUUID();
			const UUID child = scene.CreateEntity("Needle child").GetUUID();
			TransformSystem::SetWorldPosition(scene.FindEntityByID(parent), { 5.0f, 0.0f, 0.0f });
			TransformSystem::SetWorldPosition(scene.FindEntityByID(child), { 2.0f, 3.0f, 0.0f });
			SceneHierarchyPanel panel;
			Test::PanelInteractionUi ui;
			ImVec2 origin{};
			ImVec2 rootDrop{};
			const auto draw = [&panel, &context, &origin, &rootDrop]()
			{
				origin = ImGui::GetCursorScreenPos();
				const auto status = panel.Draw(context);
				rootDrop = Test::PanelInteractionUi::LastItemCenter();
				return status;
			};
			REQUIRE(ui.Frame(draw));
			const float firstY = origin.y + 2.0f * ImGui::GetFrameHeightWithSpacing() + 3.0f + ImGui::GetStyle().ItemSpacing.y + 6.0f;
			const float row = ImGui::GetTextLineHeightWithSpacing() + 3.0f + ImGui::GetStyle().ItemSpacing.y;
			const ImVec2 source(origin.x + 80.0f, firstY + row);
			const ImVec2 destination(origin.x + 80.0f, firstY);
			ImGui::GetIO().AddMousePosEvent(source.x, source.y);
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddMousePosEvent(destination.x, destination.y);
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(scene.FindEntityByID(child).GetParent().IsValid());
			context.Actions.Pump();
			REQUIRE(scene.FindEntityByID(child).GetParent().IsValid());
			CHECK(scene.FindEntityByID(child).GetParent().GetUUID() == parent);
			CHECK(TransformSystem::GetWorldPosition(scene.FindEntityByID(child)) == glm::vec3(2.0f, 3.0f, 0.0f));
			const auto history = context.Editor.GetHistory().GetEntries(10);
			REQUIRE(history.size() == 1);
			CHECK(history.front().Origin == CommandOrigin::User);
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 60.0f, origin.y + 8.0f)));
			ImGui::GetIO().AddInputCharactersUTF8("Needle");
			REQUIRE(ui.Frame(draw));
			const float matchingRowsBottom = rootDrop.y;
			REQUIRE(context.Editor.SetSelection({}, SceneTarget::Edit));
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 100.0f, firstY + row)));
			context.Actions.Pump();
			REQUIRE(context.Editor.GetSelection().size() == 1);
			CHECK(context.Editor.GetSelection().front() == child);
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 60.0f, origin.y + 8.0f)));
			ImGui::GetIO().AddInputCharactersUTF8(" absent");
			REQUIRE(ui.Frame(draw));
			CHECK(rootDrop.y < matchingRowsBottom - row);
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK_FALSE(scene.FindEntityByID(child).GetParent().IsValid());
			CHECK(TransformSystem::GetWorldPosition(scene.FindEntityByID(child)) == glm::vec3(2.0f, 3.0f, 0.0f));
		}
	}

}
