#include "TestsPCH.h"
#include "Editor/Panels/InspectorPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"

#include <imgui_internal.h>

namespace Engine {

	static ImGuiID InspectorPanelTestScope(ImGuiID parent, std::string_view key)
	{
		const int hash = static_cast<int>(FNV1a32(key));
		return ImHashData(&hash, sizeof(hash), parent);
	}

	static ImGuiID InspectorPanelTestField(const EditorPanelContext& context, std::string_view component, std::string_view field, bool variant = false)
	{
		const auto& editor = context.Editor;
		const auto* window = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::Inspector));
		REQUIRE(window != nullptr);
		std::string path = "0:0000000000000000:" + std::string(component) + "." + std::string(field);
		for (const UUID id : editor.GetSelection())
			path += ":" + id.ToString();
		path += editor.GetSelectionTarget() == SceneTarget::Play ? ":Play" : ":Edit";
		path += ":" + FileSystem::PathToUtf8(editor.GetProject().GetProjectFile()) + ":" + std::to_string(context.Thumbnails.GetProjectGeneration());
		if (editor.GetSelectionTarget() == SceneTarget::Play)
		{
			const auto* session = editor.GetPlay().GetSession();
			REQUIRE(session != nullptr);
			path += ":Play:" + std::to_string(session->GetSerial()) + ":" + std::to_string(session->GetSceneGeneration());
		}
		else
			path += ":Edit:" + std::to_string(editor.GetRevision() - editor.GetScene().GetRevision());
		ImGuiID scope = InspectorPanelTestScope(InspectorPanelTestScope(window->ID, component), path);
		if (variant)
			scope = InspectorPanelTestScope(scope, path);
		return ImHashStr("##Value", 0, ImHashStr("Property", 0, scope));
	}

	static ImVec2 InspectorPanelTestItem(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, const char* windowName, ImGuiID id, ImGuiNavLayer layer = ImGuiNavLayer_Main)
	{
		ImRect rectangle;
		const auto locate = [&draw, windowName, id, layer, &rectangle]() -> Status
		{
			ImGuiWindow* window = ImGui::FindWindowByName(windowName);
			REQUIRE(window != nullptr);
			ImGui::SetNavWindow(window);
			ImGui::SetNavID(id, layer, window->NavRootFocusScopeId, ImRect());
			ImGui::GetCurrentContext()->NavIdIsAlive = false;
			ENGINE_TRY(draw());
			const auto& gui = *ImGui::GetCurrentContext();
			REQUIRE(gui.NavIdIsAlive);
			REQUIRE(gui.NavId == id);
			REQUIRE(gui.NavLayer == layer);
			rectangle = ImGui::WindowRectRelToAbs(gui.NavWindow, gui.NavWindow->NavRectRel[gui.NavLayer]);
			REQUIRE(rectangle.GetWidth() > 0.0f);
			REQUIRE(rectangle.GetHeight() > 0.0f);
			return {};
		};
		REQUIRE(ui.Frame(locate));
		return rectangle.GetCenter();
	}

	static ImGuiWindow* InspectorPanelTestPopup()
	{
		for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
		{
			if (window->Active && (window->Flags & ImGuiWindowFlags_Popup) != 0)
				return window;
		}
		return nullptr;
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("InspectorPanel: an absent script field displays its default and commits one override at the safe point")
		{
			Test::PanelInteractionFixture fixture("InspectorScriptDefault");
			auto& context = fixture.GetContext();
			const auto path = VfsPath::Create("project", "Assets/Toggle.luau");
			REQUIRE(path);
			REQUIRE(context.Editor.GetScriptService() != nullptr);
			const auto written = context.Editor.GetScriptService()->Write(*path,
				"return Script.Define(\"Toggle\", {Fields = {Enabled = Field.Bool()}})");
			REQUIRE(written);
			const Entity entity = context.Editor.GetScene().CreateEntity("Toggle");
			const UUID id = entity.GetUUID();
			const Json component{ { "Script", written->Script.ToString() } };
			REQUIRE(ComponentAccess::AddComponent(entity, "Script", &component));
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			EditorLayer layer(context);
			for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
				REQUIRE(context.Editor.GetUiState().SetPanelOpen(static_cast<EditorPanel>(index), static_cast<EditorPanel>(index) == EditorPanel::Inspector));
			Test::PanelInteractionUi ui(false);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			const auto draw = [&layer]()
			{
				return layer.OnImGuiRender();
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			CHECK(entity.GetComponent<ScriptComponent>().Fields.empty());
			const auto history = context.Editor.GetHistory().GetUndoCount();
			REQUIRE(ui.Click(draw, InspectorPanelTestItem(ui, draw, Utils::EditorWindowTitle(EditorPanel::Inspector), InspectorPanelTestField(context, "Script", "Fields[Enabled]", true))));
			CHECK(entity.GetComponent<ScriptComponent>().Fields.empty());
			REQUIRE(context.InspectorEdits.IsEditing());
			REQUIRE(layer.OnSafePoint(0.0));
			const auto& fields = context.Editor.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields;
			REQUIRE(fields.size() == 1);
			CHECK(fields.find("Enabled")->second.Get() == Json(true));
			CHECK(context.Editor.GetHistory().GetUndoCount() == history + 1);
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields.empty());
		}

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
			const auto draw = [&layer]()
			{
				return layer.OnImGuiRender();
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Click(draw, InspectorPanelTestItem(ui, draw, Utils::EditorWindowTitle(EditorPanel::Inspector), InspectorPanelTestField(context, "MeshRenderer", "Visible"))));
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
			auto* menuBar = ImGui::FindWindowByName("##MainMenuBar");
			REQUIRE(menuBar != nullptr);
			// BeginMenuBar adds this ID scope and submits its entries on the menu navigation layer.
			const ImGuiID editMenu = ImHashStr("Edit", 0, menuBar->GetID("##MenuBar"));
			REQUIRE(ui.Click(draw, InspectorPanelTestItem(ui, draw, menuBar->Name, editMenu, ImGuiNavLayer_Menu)));
			REQUIRE(ui.Frame(draw));
			auto* menu = InspectorPanelTestPopup();
			REQUIRE(menu != nullptr);
			REQUIRE(ui.Click(draw, InspectorPanelTestItem(ui, draw, menu->Name, menu->GetID("Undo"))));
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
			const auto draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			const auto* window = ImGui::FindWindowByName("Panel interaction");
			REQUIRE(window != nullptr);
			const ImGuiID component = InspectorPanelTestScope(window->ID, "MeshRenderer");
			const ImGuiID options = ImHashStr("...##Options", 0, ImHashStr("ComponentHeader", 0, component));
			REQUIRE(ui.Click(draw, InspectorPanelTestItem(ui, draw, window->Name, options)));
			REQUIRE(ui.Frame(draw));
			const auto* popup = InspectorPanelTestPopup();
			REQUIRE(popup != nullptr);
			REQUIRE(ui.Click(draw, InspectorPanelTestItem(ui, draw, popup->Name, ImHashStr("Remove component", 0, popup->ID))));
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
