#include "TestsPCH.h"
#include "Editor/Panels/InspectorPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/PanelInteractionFixture.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("InspectorPanel: a mixed checkbox commits once for all selected entities at the layer safe point")
		{
			Test::PanelInteractionFixture fixture("InspectorUiMultiEdit");
			auto& context = fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID first = scene.CreateEntity("First").GetUUID();
			const UUID second = scene.CreateEntity("Second").GetUUID();
			for (const UUID id : { first, second })
				REQUIRE(ComponentAccess::AddComponent(scene.FindEntityByID(id), "MeshRenderer", nullptr));
			REQUIRE(ComponentAccess::SetFieldValue(scene.FindEntityByID(second), "MeshRenderer", "Visible", Value::FromBool(false)));
			REQUIRE(context.Editor.SetSelection({ first, second }, SceneTarget::Edit));
			EditorLayer layer(context);
			for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
				REQUIRE(context.Editor.GetUiState().SetPanelOpen(static_cast<EditorPanel>(index), static_cast<EditorPanel>(index) == EditorPanel::Inspector));
			Test::PanelInteractionUi ui(false);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			ImVec2 end{};
			const auto draw = [&layer, &end]()
			{
				const auto status = layer.OnImGuiRender();
				ImGui::Begin("Inspector");
				end = ImGui::GetCursorScreenPos();
				ImGui::End();
				return status;
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			const float visibleY = end.y - 3.0f * ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeight() * 0.5f;
			REQUIRE(ui.Click(draw, ImVec2(end.x + 8.0f, visibleY)));
			CHECK(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			CHECK_FALSE(scene.FindEntityByID(second).GetComponent<MeshRendererComponent>().Visible);
			CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
			REQUIRE(context.InspectorEdits.IsEditing());
			REQUIRE(layer.OnSafePoint(0.0));
			CHECK_FALSE(context.InspectorEdits.IsEditing());
			CHECK_FALSE(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			CHECK_FALSE(scene.FindEntityByID(second).GetComponent<MeshRendererComponent>().Visible);
			const auto history = context.Editor.GetHistory().GetEntries(10);
			REQUIRE(history.size() == 1);
			CHECK(history.front().Origin == CommandOrigin::User);
			REQUIRE(layer.OnSafePoint(0.0));
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(ui.Click(draw, ImVec2(60.0f, 10.0f)));                                                              // Edit menu
			REQUIRE(ui.Click(draw, ImVec2(70.0f, ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y + 6.0f))); // Undo
			CHECK_FALSE(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			REQUIRE(layer.OnSafePoint(0.0));
			CHECK(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			CHECK_FALSE(scene.FindEntityByID(second).GetComponent<MeshRendererComponent>().Visible);
			REQUIRE(fixture.GetAutomation().Call("edit.redo", Json::object()));
			CHECK_FALSE(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			CHECK_FALSE(scene.FindEntityByID(second).GetComponent<MeshRendererComponent>().Visible);
		}

		TEST_CASE("InspectorPanel: remove component queues one atomic multi-selection action")
		{
			Test::PanelInteractionFixture fixture("InspectorUiRemove");
			auto& context = fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID first = scene.CreateEntity("First").GetUUID();
			const UUID second = scene.CreateEntity("Second").GetUUID();
			for (const UUID id : { first, second })
				REQUIRE(ComponentAccess::AddComponent(scene.FindEntityByID(id), "MeshRenderer", nullptr));
			REQUIRE(context.Editor.SetSelection({ first, second }, SceneTarget::Edit));
			InspectorPanel panel;
			Test::PanelInteractionUi ui;
			ImVec2 add{};
			const auto draw = [&panel, &context, &add]()
			{
				const auto status = panel.Draw(context);
				add = Test::PanelInteractionUi::LastItemCenter();
				return status;
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Click(draw, ImVec2(add.x, add.y - ImGui::GetFrameHeightWithSpacing())));
			for (const UUID id : { first, second })
				CHECK(scene.FindEntityByID(id).HasComponent<MeshRendererComponent>());
			CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
			context.Actions.Pump();
			for (const UUID id : { first, second })
				CHECK_FALSE(scene.FindEntityByID(id).HasComponent<MeshRendererComponent>());
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			for (const UUID id : { first, second })
				CHECK(scene.FindEntityByID(id).HasComponent<MeshRendererComponent>());
		}
	}

}
