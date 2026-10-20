#include "TestsPCH.h"
#include "Editor/Panels/InspectorPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"

#include <imgui_internal.h>

#include <cmath>
#include <functional>
#include <string>

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
			path += ":Edit:" + std::to_string(editor.GetSceneDocumentEpoch());
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

	namespace {

		struct InspectorLayerHarness
		{
			Test::PanelInteractionFixture Fixture{ "InspectorLayer" };
			EditorLayer Layer{ Fixture.GetContext() };
			Test::PanelInteractionUi Ui{ false };
			std::string Text{};
			bool CaptureText = false;

			InspectorLayerHarness()
			{
				Utils::ApplyEditorStyle();
				ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
				ImGui::GetIO().DisplaySize = ImVec2(1600.0f, 900.0f);
				for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
					REQUIRE(Fixture.GetContext().Editor.GetUiState().SetPanelOpen(static_cast<EditorPanel>(index), static_cast<EditorPanel>(index) == EditorPanel::Inspector));
			}

			Status Draw()
			{
				if (CaptureText)
					ImGui::LogToBuffer(0);
				const Status status = Layer.OnImGuiRender();
				if (CaptureText)
				{
					Text = ImGui::GetCurrentContext()->LogBuffer.c_str();
					ImGui::LogFinish();
				}
				return status;
			}

			Status Frame()
			{
				return Ui.Frame([this]()
				{
					return Draw();
				});
			}
		};

	}

	static ImGuiID InspectorLayerIntId(ImGuiID parent, int key)
	{
		return ImHashData(&key, sizeof(key), parent);
	}

	static ImGuiID InspectorLayerComponentId(std::string_view component)
	{
		const auto* window = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::Inspector));
		REQUIRE(window != nullptr);
		return InspectorLayerIntId(window->ID, static_cast<int>(FNV1a32(component)));
	}

	static std::string InspectorLayerOwnerIdentity(const EditorPanelContext& context, bool asset = false)
	{
		const auto& editor = context.Editor;
		std::string identity = FileSystem::PathToUtf8(editor.GetProject().GetProjectFile()) + ":" + std::to_string(context.Thumbnails.GetProjectGeneration());
		if (asset)
			return identity + ":Asset";
		if (editor.GetSelectionTarget() == SceneTarget::Play)
		{
			const auto* session = editor.GetPlay().GetSession();
			REQUIRE(session != nullptr);
			return identity + ":Play:" + std::to_string(session->GetSerial()) + ":" + std::to_string(session->GetSceneGeneration());
		}
		return identity + ":Edit:" + std::to_string(editor.GetSceneDocumentEpoch());
	}

	static ImGuiID InspectorLayerPropertyId(const EditorPanelContext& context, std::string_view component, std::string_view field, bool variant = false)
	{
		const auto& editor = context.Editor;
		std::string path = "0:0000000000000000:" + std::string(component) + "." + std::string(field);
		for (const UUID id : editor.GetSelection())
			path += ":" + id.ToString();
		path += editor.GetSelectionTarget() == SceneTarget::Play ? ":Play" : ":Edit";
		path += ":" + InspectorLayerOwnerIdentity(context);
		ImGuiID id = InspectorLayerIntId(InspectorLayerComponentId(component), static_cast<int>(FNV1a32(path)));
		if (variant)
			id = InspectorLayerIntId(id, static_cast<int>(FNV1a32(path)));
		return ImHashStr("Property", 0, id);
	}

	// ImGui records this rectangle when it submits the requested navigation item. Tests then click the real geometry;
	// they never toggle storage, replace a command, or synthesize a successful widget result.
	static ImRect InspectorLayerItem(Test::PanelInteractionUi& ui, const std::function<Status()>& draw,
		const char* windowName, ImGuiID id)
	{
		ImRect rectangle;
		const auto locate = [&draw, windowName, id, &rectangle]() -> Status
		{
			ImGuiWindow* window = ImGui::FindWindowByName(windowName);
			REQUIRE(window != nullptr);
			ImGui::SetNavWindow(window);
			ImGui::SetNavID(id, ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
			ImGui::GetCurrentContext()->NavIdIsAlive = false;
			ENGINE_TRY(draw());
			const auto& gui = *ImGui::GetCurrentContext();
			REQUIRE(gui.NavIdIsAlive);
			REQUIRE(gui.NavId == id);
			rectangle = ImGui::WindowRectRelToAbs(gui.NavWindow, gui.NavWindow->NavRectRel[gui.NavLayer]);
			CHECK(rectangle.GetWidth() > 0.0f);
			CHECK(rectangle.GetHeight() > 0.0f);
			return {};
		};
		REQUIRE(ui.Frame(locate));
		return rectangle;
	}

	static ImRect InspectorLayerItem(InspectorLayerHarness& test, ImGuiID id)
	{
		return InspectorLayerItem(test.Ui, [&test]()
		{
			return test.Draw();
		}, Utils::EditorWindowTitle(EditorPanel::Inspector), id);
	}

	static ImGuiWindow* InspectorLayerPopup(std::string_view child = {})
	{
		for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
		{
			if (window->Active && (child.empty() ? (window->Flags & ImGuiWindowFlags_Popup) != 0 : std::string_view(window->Name).contains(child)))
				return window;
		}
		return nullptr;
	}

	static void InspectorLayerReplaceText(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, const char* text)
	{
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
		REQUIRE(ui.Frame(draw));
		ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
		REQUIRE(ui.Frame(draw));
		ImGui::GetIO().AddInputCharactersUTF8(text);
		REQUIRE(ui.Frame(draw));
	}

	static ImGuiID InspectorLayerAxisId(const EditorPanelContext& context, std::string_view component, std::string_view field, int axis)
	{
		return ImHashStr("##Axis", 0, InspectorLayerIntId(InspectorLayerPropertyId(context, component, field), axis));
	}

	// Ctrl+click turns a drag into its text entry; the typed text applies on Enter.
	static void InspectorLayerTypeInto(InspectorLayerHarness& test, ImVec2 point, const char* text)
	{
		const auto draw = [&test]()
		{
			return test.Draw();
		};
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
		REQUIRE(test.Ui.Click(draw, point));
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
		REQUIRE(test.Frame());
		InspectorLayerReplaceText(test.Ui, draw, text);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
		REQUIRE(test.Frame());
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
		REQUIRE(test.Frame());
	}

	// A real drag with no drag threshold: each pixel moves a float field by one step of its speed.
	static void InspectorLayerDrag(InspectorLayerHarness& test, ImVec2 from, float pixels)
	{
		ImGui::GetIO().MouseDragThreshold = 0.0f;
		// Consecutive drags from one point must not read as a double click, which opens the text entry instead.
		ImGui::GetIO().MouseDoubleClickTime = 0.0f;
		ImGui::GetIO().AddMousePosEvent(from.x, from.y);
		REQUIRE(test.Frame());
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		REQUIRE(test.Frame());
		ImGui::GetIO().AddMousePosEvent(from.x + pixels, from.y);
		REQUIRE(test.Frame());
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		REQUIRE(test.Frame());
	}

	// Whether the last frame drew anything in `color` into the named window (text, check marks and solid shapes keep
	// the exact colour in their vertices).
	static bool InspectorLayerWindowUsesColor(const char* windowName, ImU32 color)
	{
		const ImGuiWindow* window = ImGui::FindWindowByName(windowName);
		REQUIRE(window != nullptr);
		for (const ImDrawVert& vertex : window->DrawList->VtxBuffer)
		{
			if (vertex.col == color)
				return true;
		}
		return false;
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

		TEST_CASE("InspectorPanel: an active name buffer cannot rename a replacement scene or restarted play entity")
		{
			for (const bool restartPlay : { false, true })
			{
				CAPTURE(restartPlay);
				Test::PanelInteractionFixture fixture("InspectorNameEpoch");
				auto& context = fixture.GetContext();
				auto& editor = context.Editor;
				auto& automation = fixture.GetAutomation();
				const UUID id = editor.GetScene().CreateEntity("Original object").GetUUID();
				REQUIRE(automation.Call("scene.save", Json::object()));
				if (restartPlay)
					REQUIRE(automation.Call("play.start", Json{ { "paused", true } }));
				const SceneTarget target = restartPlay ? SceneTarget::Play : SceneTarget::Edit;
				REQUIRE(editor.SetSelection({ id }, target));
				InspectorPanel panel;
				Test::PanelInteractionUi ui;
				Utils::ApplyEditorStyle();
				bool focusName = false;
				const auto draw = [&panel, &context, &focusName]() -> Status
				{
					if (focusName)
					{
						// The name follows the active checkbox. Discover it through focus navigation, independent of its ID.
						ImGui::SetKeyboardFocusHere(1);
						focusName = false;
					}
					return panel.Draw(context);
				};
				REQUIRE(ui.Frame(draw));
				REQUIRE(ui.Frame(draw));
				focusName = true;
				REQUIRE(ui.Frame(draw));
				REQUIRE(ui.Frame(draw));
				auto& gui = *ImGui::GetCurrentContext();
				const ImGuiID nameId = gui.ActiveId;
				auto* input = ImGui::GetInputTextState(nameId);
				REQUIRE(input != nullptr);
				REQUIRE(std::string_view(input->GetText()) == "Original object");
				const ImRect nameRect = ImGui::WindowRectRelToAbs(gui.NavWindow, gui.NavWindow->NavRectRel[gui.NavLayer]);
				REQUIRE(nameRect.GetWidth() > 0.0f);
				REQUIRE(ui.Click(draw, nameRect.GetCenter()));
				InspectorLayerReplaceText(ui, draw, "Stale rename");
				REQUIRE(gui.ActiveId == nameId);
				input = ImGui::GetInputTextState(nameId);
				REQUIRE(input != nullptr);
				REQUIRE(std::string_view(input->GetText()) == "Stale rename");
				CHECK(editor.GetScene().FindEntityByID(id).GetName() == "Original object");
				if (restartPlay)
				{
					const uint64_t serial = editor.GetPlay().GetSession()->GetSerial();
					REQUIRE(automation.Call("play.stop", Json::object()));
					REQUIRE(automation.Call("play.start", Json{ { "paused", true } }));
					REQUIRE(editor.GetPlay().GetSession()->GetSerial() != serial);
				}
				else
					REQUIRE(automation.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true } }));
				// Re-select the identical UUID before another ImGui frame; an empty-selection frame must not mask the bug.
				REQUIRE(editor.SetSelection({ id }, target));
				const auto history = editor.GetHistory().GetUndoCount();
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
				context.Actions.Pump();
				REQUIRE(ui.Frame(draw));
				CHECK(gui.ActiveId != nameId);
				Scene& addressed = restartPlay ? editor.GetPlay().GetSession()->GetScene() : editor.GetScene();
				REQUIRE(addressed.FindEntityByID(id).IsValid());
				CHECK(addressed.FindEntityByID(id).GetName() == "Original object");
				CHECK(editor.GetScene().FindEntityByID(id).GetName() == "Original object");
				CHECK(editor.GetHistory().GetUndoCount() == history);
				if (restartPlay)
					REQUIRE(automation.Call("play.stop", Json::object()));
			}
		}

		TEST_CASE("InspectorPanel: replacement owners discard active reflected input and pending field gestures")
		{
			for (const bool commitBeforeReplacement : { false, true })
			{
				CAPTURE(commitBeforeReplacement);
				InspectorLayerHarness test;
				auto& context = test.Fixture.GetContext();
				auto& automation = test.Fixture.GetAutomation();
				const UUID id = context.Editor.GetScene().CreateEntity("Original object").GetUUID();
				REQUIRE(automation.Call("scene.save", Json::object()));
				REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
				REQUIRE(test.Frame());
				REQUIRE(test.Frame());
				const auto draw = [&test]()
				{
					return test.Draw();
				};
				const ImGuiID property = InspectorLayerPropertyId(context, "Transform", "Translation");
				const ImGuiID axis = ImHashStr("##Axis", 0, InspectorLayerIntId(property, 0));
				const ImVec2 point = InspectorLayerItem(test, axis).GetCenter();
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
				REQUIRE(test.Ui.Click(draw, point));
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
				REQUIRE(test.Frame());
				InspectorLayerReplaceText(test.Ui, draw, "42");
				REQUIRE(context.InspectorEdits.IsEditing());
				REQUIRE(ImGui::GetCurrentContext()->ActiveId == axis);
				auto* input = ImGui::GetInputTextState(axis);
				REQUIRE(input != nullptr);
				REQUIRE(std::string_view(input->GetText()) == "42");
				// Scalar text is applied on validation in the pinned ImGui version, not while typing.
				const auto preview = context.InspectorEdits.GetPreview();
				REQUIRE(preview);
				REQUIRE(preview->AsVec3().x == 0.0f);
				if (commitBeforeReplacement)
				{
					ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
					REQUIRE(test.Frame());
					ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
					const auto validated = context.InspectorEdits.GetPreview();
					REQUIRE(validated);
					REQUIRE(validated->AsVec3().x == 42.0f);
				}
				REQUIRE(automation.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true } }));
				REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
				const auto history = context.Editor.GetHistory().GetUndoCount();
				if (!commitBeforeReplacement)
				{
					ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
					REQUIRE(test.Frame());
					ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
				}
				// Also cover replacement between the UI's commit request and the safe point, with no intervening Draw.
				REQUIRE(test.Layer.OnSafePoint(0.0));
				REQUIRE(test.Frame());
				CHECK_FALSE(context.InspectorEdits.IsEditing());
				CHECK(ImGui::GetCurrentContext()->ActiveId != axis);
				CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
				CHECK(context.Editor.GetHistory().GetUndoCount() == history);
			}
		}

		TEST_CASE("InspectorPanel: a project-settings change or its undo keeps a pending gesture, which a scene replacement discards")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			auto& automation = test.Fixture.GetAutomation();
			const UUID id = context.Editor.GetScene().CreateEntity("Original object").GetUUID();
			REQUIRE(automation.Call("scene.save", Json::object()));
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const auto translation = [&context, id]()
			{
				return context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Translation;
			};
			const auto typeIntoAxis = [&test, &context, &draw](const char* text)
			{
				const ImGuiID axis = InspectorLayerAxisId(context, "Transform", "Translation", 0);
				const ImVec2 point = InspectorLayerItem(test, axis).GetCenter();
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
				REQUIRE(test.Ui.Click(draw, point));
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
				REQUIRE(test.Frame());
				InspectorLayerReplaceText(test.Ui, draw, text);
				REQUIRE(context.InspectorEdits.IsEditing());
				REQUIRE(ImGui::GetCurrentContext()->ActiveId == axis);
				return axis;
			};
			const auto enter = [&test]()
			{
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
				REQUIRE(test.Frame());
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
				REQUIRE(test.Layer.OnSafePoint(0.0));
				REQUIRE(test.Frame());
			};

			ImGuiID axis = typeIntoAxis("42");
			// Installing project settings, and undoing that, advances the editor's revision; the scene document stays the same.
			const uint64_t epoch = context.Editor.GetSceneDocumentEpoch();
			const uint64_t revision = context.Editor.GetRevision();
			REQUIRE(automation.Call("project.setSettings", Json{ { "patch", Json{ { "Rendering", Json{ { "ShadowMapSize", 512 } } } } } }));
			REQUIRE(test.Frame());
			REQUIRE(automation.Call("edit.undo", Json::object()));
			REQUIRE(context.Editor.GetProject().GetSettings().Rendering.ShadowMapSize != 512);
			REQUIRE(context.Editor.GetRevision() != revision);
			REQUIRE(context.Editor.GetSceneDocumentEpoch() == epoch);
			REQUIRE(test.Frame());
			CHECK(context.InspectorEdits.IsEditing());
			CHECK(ImGui::GetCurrentContext()->ActiveId == axis);
			const auto history = context.Editor.GetHistory().GetUndoCount();
			enter();
			CHECK_FALSE(context.InspectorEdits.IsEditing());
			CHECK(translation() == glm::vec3(42.0f, 0.0f, 0.0f));
			CHECK(context.Editor.GetHistory().GetUndoCount() == history + 1);

			// Replacing the scene document still discards a pending gesture.
			REQUIRE(automation.Call("scene.save", Json::object()));
			axis = typeIntoAxis("7");
			REQUIRE(automation.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true } }));
			REQUIRE(context.Editor.GetSceneDocumentEpoch() != epoch);
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			enter();
			CHECK_FALSE(context.InspectorEdits.IsEditing());
			CHECK(ImGui::GetCurrentContext()->ActiveId != axis);
			CHECK(translation() == glm::vec3(42.0f, 0.0f, 0.0f));
			CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("InspectorPanel: a typed name survives a project-settings change and its undo")
		{
			Test::PanelInteractionFixture fixture("InspectorNameSettings");
			auto& context = fixture.GetContext();
			auto& editor = context.Editor;
			auto& automation = fixture.GetAutomation();
			const UUID id = editor.GetScene().CreateEntity("Original object").GetUUID();
			REQUIRE(editor.SetSelection({ id }, SceneTarget::Edit));
			InspectorPanel panel;
			Test::PanelInteractionUi ui;
			Utils::ApplyEditorStyle();
			bool focusName = false;
			const auto draw = [&panel, &context, &focusName]() -> Status
			{
				if (focusName)
				{
					// The name follows the active checkbox. Discover it through focus navigation, independent of its ID.
					ImGui::SetKeyboardFocusHere(1);
					focusName = false;
				}
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			focusName = true;
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			auto& gui = *ImGui::GetCurrentContext();
			const ImGuiID nameId = gui.ActiveId;
			auto* input = ImGui::GetInputTextState(nameId);
			REQUIRE(input != nullptr);
			REQUIRE(std::string_view(input->GetText()) == "Original object");
			const ImRect nameRect = ImGui::WindowRectRelToAbs(gui.NavWindow, gui.NavWindow->NavRectRel[gui.NavLayer]);
			REQUIRE(nameRect.GetWidth() > 0.0f);
			REQUIRE(ui.Click(draw, nameRect.GetCenter()));
			InspectorLayerReplaceText(ui, draw, "Renamed object");
			REQUIRE(gui.ActiveId == nameId);
			REQUIRE(automation.Call("project.setSettings", Json{ { "patch", Json{ { "Rendering", Json{ { "ShadowMapSize", 512 } } } } } }));
			REQUIRE(ui.Frame(draw));
			CHECK(gui.ActiveId == nameId);
			REQUIRE(automation.Call("edit.undo", Json::object()));
			REQUIRE(editor.GetProject().GetSettings().Rendering.ShadowMapSize != 512);
			REQUIRE(ui.Frame(draw));
			CHECK(gui.ActiveId == nameId);
			input = ImGui::GetInputTextState(nameId);
			REQUIRE(input != nullptr);
			CHECK(std::string_view(input->GetText()) == "Renamed object");
			const auto history = editor.GetHistory().GetUndoCount();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
			context.Actions.Pump();
			REQUIRE(ui.Frame(draw));
			CHECK(editor.GetScene().FindEntityByID(id).GetName() == "Renamed object");
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
		}

		TEST_CASE("InspectorPanel: transform defaults expose Euler editing and advanced values stay accessible")
		{
			InspectorLayerHarness test;
			auto& editor = test.Fixture.GetContext().Editor;
			const UUID id = editor.GetScene().CreateEntity("Camera###authored-name").GetUUID();
			REQUIRE(editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			test.CaptureText = true;
			REQUIRE(test.Frame());
			CHECK(test.Text.contains("Position (m)"));
			CHECK(test.Text.contains("Rotation (deg)"));
			CHECK(test.Text.contains("Scale"));
			CHECK_FALSE(test.Text.contains("Quaternion"));
			CHECK_FALSE(test.Text.contains("World Position"));
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const ImGuiID advanced = ImHashStr("Advanced transform", 0, InspectorLayerComponentId("Transform"));
			REQUIRE(test.Ui.Click(draw, InspectorLayerItem(test, advanced).GetCenter()));
			REQUIRE(test.Frame());
			CHECK(test.Text.contains("Quaternion"));
			CHECK(test.Text.contains("World Position"));
			CHECK(test.Text.contains("Render Position"));
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			CHECK(editor.GetScene().FindEntityByID(id).GetName() == "Camera###authored-name");
		}

		TEST_CASE("InspectorPanel: Euler entry commits once and undo restores the original transform")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			const UUID id = context.Editor.GetScene().CreateEntity("Rotating object").GetUUID();
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const ImGuiID property = InspectorLayerPropertyId(context, "Transform", "EulerAngles");
			const ImGuiID axis = ImHashStr("##Axis", 0, InspectorLayerIntId(property, 1));
			const ImVec2 point = InspectorLayerItem(test, axis).GetCenter();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			REQUIRE(test.Ui.Click(draw, point));
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			REQUIRE(test.Frame());
			InspectorLayerReplaceText(test.Ui, draw, "45");
			CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
			REQUIRE(test.Frame());
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
			REQUIRE(test.Layer.OnSafePoint(0.0));
			const auto euler = ComponentAccess::GetFieldValue(context.Editor.GetScene().FindEntityByID(id), "Transform", "EulerAngles");
			REQUIRE(euler);
			CHECK(euler->AsVec3().y == doctest::Approx(45.0f));
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(test.Fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		}

		TEST_CASE("InspectorPanel: escape and selection changes discard a live transform preview")
		{
			for (const bool changeSelection : { false, true })
			{
				InspectorLayerHarness test;
				auto& context = test.Fixture.GetContext();
				const UUID first = context.Editor.GetScene().CreateEntity("First").GetUUID();
				const UUID second = context.Editor.GetScene().CreateEntity("Second").GetUUID();
				REQUIRE(context.Editor.SetSelection({ first }, SceneTarget::Edit));
				REQUIRE(test.Frame());
				REQUIRE(test.Frame());
				const ImGuiID property = InspectorLayerPropertyId(context, "Transform", "Translation");
				const ImGuiID axis = ImHashStr("##Axis", 0, InspectorLayerIntId(property, 0));
				const ImVec2 point = InspectorLayerItem(test, axis).GetCenter();
				ImGui::GetIO().AddMousePosEvent(point.x, point.y);
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				REQUIRE(test.Frame());
				ImGui::GetIO().AddMousePosEvent(point.x + ImGui::GetFontSize() * 3.0f, point.y);
				REQUIRE(test.Frame());
				REQUIRE(context.InspectorEdits.IsEditing());
				const auto preview = context.InspectorEdits.GetPreview();
				REQUIRE(preview);
				CHECK(preview->AsVec3().x != 0.0f);
				if (changeSelection)
					REQUIRE(context.Editor.SetSelection({ second }, SceneTarget::Edit));
				else
					ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
				REQUIRE(test.Frame());
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				REQUIRE(test.Frame());
				REQUIRE(test.Layer.OnSafePoint(0.0));
				CHECK_FALSE(context.InspectorEdits.IsEditing());
				CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
				for (const UUID id : { first, second })
					CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
			}
		}

		TEST_CASE("InspectorPanel: a mixed checkbox previews all objects and commits one undoable change")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID first = scene.CreateEntity("First").GetUUID();
			const UUID second = scene.CreateEntity("Second").GetUUID();
			for (const UUID id : { first, second })
				REQUIRE(ComponentAccess::AddComponent(scene.FindEntityByID(id), "MeshRenderer", nullptr));
			REQUIRE(ComponentAccess::SetFieldValue(scene.FindEntityByID(second), "MeshRenderer", "Visible", Value::FromBool(false)));
			REQUIRE(context.Editor.SetSelection({ first, second }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const ImGuiID checkbox = ImHashStr("##Value", 0, InspectorLayerPropertyId(context, "MeshRenderer", "Visible"));
			REQUIRE(test.Ui.Click(draw, InspectorLayerItem(test, checkbox).GetCenter()));
			CHECK(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			CHECK_FALSE(scene.FindEntityByID(second).GetComponent<MeshRendererComponent>().Visible);
			CHECK(context.InspectorEdits.IsEditing());
			REQUIRE(test.Layer.OnSafePoint(0.0));
			CHECK_FALSE(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			CHECK_FALSE(scene.FindEntityByID(second).GetComponent<MeshRendererComponent>().Visible);
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(test.Fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(scene.FindEntityByID(first).GetComponent<MeshRendererComponent>().Visible);
			CHECK_FALSE(scene.FindEntityByID(second).GetComponent<MeshRendererComponent>().Visible);
		}

		TEST_CASE("InspectorPanel: component search adds and options remove one atomic multi-selection batch")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID first = scene.CreateEntity("First").GetUUID();
			const UUID second = scene.CreateEntity("Second").GetUUID();
			REQUIRE(context.Editor.SetSelection({ first, second }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const auto* inspector = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::Inspector));
			REQUIRE(inspector != nullptr);
			REQUIRE(test.Ui.Click(draw, InspectorLayerItem(test, ImHashStr("Add component", 0, inspector->ID)).GetCenter()));
			REQUIRE(test.Frame());
			ImGui::GetIO().AddInputCharactersUTF8("mesh renderer");
			REQUIRE(test.Frame());
			ImGuiWindow* results = InspectorLayerPopup("/Components_");
			REQUIRE(results != nullptr);
			const ImGuiID candidate = ImHashStr("Mesh Renderer", 0, InspectorLayerIntId(results->ID, static_cast<int>(FNV1a32("MeshRenderer"))));
			const ImRect entry = InspectorLayerItem(test.Ui, draw, results->Name, candidate);
			REQUIRE(test.Ui.Click(draw, entry.GetCenter()));
			CHECK_FALSE(scene.FindEntityByID(first).HasComponent<MeshRendererComponent>());
			REQUIRE(test.Layer.OnSafePoint(0.0));
			for (const UUID id : { first, second })
				CHECK(scene.FindEntityByID(id).HasComponent<MeshRendererComponent>());
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(test.Frame());
			const ImGuiID header = ImHashStr("ComponentHeader", 0, InspectorLayerComponentId("MeshRenderer"));
			REQUIRE(test.Ui.Click(draw, InspectorLayerItem(test, ImHashStr("...##Options", 0, header)).GetCenter()));
			REQUIRE(test.Frame());
			ImGuiWindow* options = InspectorLayerPopup();
			REQUIRE(options != nullptr);
			const ImRect remove = InspectorLayerItem(test.Ui, draw, options->Name, ImHashStr("Remove component", 0, options->ID));
			REQUIRE(test.Ui.Click(draw, remove.GetCenter()));
			for (const UUID id : { first, second })
				CHECK(scene.FindEntityByID(id).HasComponent<MeshRendererComponent>());
			REQUIRE(test.Layer.OnSafePoint(0.0));
			for (const UUID id : { first, second })
				CHECK_FALSE(scene.FindEntityByID(id).HasComponent<MeshRendererComponent>());
			CHECK(context.Editor.GetHistory().GetUndoCount() == 2);
			REQUIRE(test.Fixture.GetAutomation().Call("edit.undo", Json::object()));
			for (const UUID id : { first, second })
				CHECK(scene.FindEntityByID(id).HasComponent<MeshRendererComponent>());
		}

		TEST_CASE("InspectorPanel: absent script values use their defaults and preserve unknown overrides")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			const auto path = VfsPath::Create("project", "Assets/Toggle.luau");
			REQUIRE(path);
			const auto written = context.Editor.GetScriptService()->Write(*path,
				"return Script.Define(\"Toggle\", {Fields = {Enabled = Field.Bool()}})");
			REQUIRE(written);
			const UUID id = context.Editor.GetScene().CreateEntity("Script object").GetUUID();
			// Model a legacy scene load: an unrelated unknown override must survive a supported field edit.
			ScriptComponent legacy;
			legacy.Script = TypedAssetHandle<AssetType::Script>(written->Script);
			legacy.Fields.emplace("Legacy", VariantValue(Json::array({ 1, 2 })));
			context.Editor.GetScene().FindEntityByID(id).AddComponent<ScriptComponent>(std::move(legacy));
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			const auto historyBefore = context.Editor.GetHistory().GetUndoCount();
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const ImGuiID checkbox = ImHashStr("##Value", 0, InspectorLayerPropertyId(context, "Script", "Fields[Enabled]", true));
			REQUIRE(test.Ui.Click(draw, InspectorLayerItem(test, checkbox).GetCenter()));
			CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields.size() == 1);
			CHECK(context.Editor.GetHistory().GetUndoCount() == historyBefore);
			REQUIRE(test.Layer.OnSafePoint(0.0));
			const auto& fields = context.Editor.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields;
			REQUIRE(fields.size() == 2);
			CHECK(fields.find("Enabled")->second.Get() == Json(true));
			CHECK(fields.find("Legacy")->second.Get() == Json::array({ 1, 2 }));
			CHECK(context.Editor.GetHistory().GetUndoCount() == historyBefore + 1);
			REQUIRE(test.Fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(context.Editor.GetHistory().GetUndoCount() == historyBefore);
			const auto& restored = context.Editor.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields;
			REQUIRE(restored.size() == 1);
			CHECK(restored.find("Enabled") == restored.end());
			REQUIRE(restored.find("Legacy") != restored.end());
			CHECK(restored.find("Legacy")->second.Get() == Json::array({ 1, 2 }));
		}

		TEST_CASE("InspectorPanel: material and import settings commit through the existing asset commands")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			auto& automation = test.Fixture.GetAutomation();
			const auto created = automation.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Inspector.material" } });
			REQUIRE(created);
			const auto id = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(id);
			context.Editor.GetUiState().SetSelectedAsset(*id);
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const auto* inspector = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::Inspector));
			REQUIRE(inspector != nullptr);
			const std::string path = "1:" + id->ToString() + ":.Roughness:Edit:" + InspectorLayerOwnerIdentity(context, true);
			const ImGuiID property = ImHashStr("Property", 0, InspectorLayerIntId(inspector->ID, static_cast<int>(FNV1a32(path))));
			const ImVec2 control = InspectorLayerItem(test, ImHashStr("##Value", 0, property)).GetCenter();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			REQUIRE(test.Ui.Click(draw, control));
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			REQUIRE(test.Frame());
			InspectorLayerReplaceText(test.Ui, draw, "0.25");
			const auto before = context.Editor.GetHistory().GetUndoCount();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
			REQUIRE(test.Frame());
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
			REQUIRE(test.Layer.OnSafePoint(0.0));
			const auto properties = automation.Call("asset.getProperties", Json{ { "asset", id->ToString() } });
			REQUIRE(properties);
			CHECK((*properties)["values"]["Roughness"] == Json(0.25f));
			CHECK(context.Editor.GetHistory().GetUndoCount() == before + 1);
			REQUIRE(automation.Call("edit.undo", Json::object()));

			const auto texturePath = VfsPath::Create("project", "Assets/Inspector.png");
			REQUIRE(texturePath);
			REQUIRE(context.Editor.WriteProjectFile(*texturePath, Test::MakeTestPng(8, 8)));
			REQUIRE(automation.Call("project.refreshAssets", Json::object()));
			auto settings = automation.Call("asset.getImportSettings", Json{ { "asset", "Assets/Inspector.png" } });
			REQUIRE(settings);
			const auto textureId = JsonReader((*settings)["asset"]["id"]).ReadUUID();
			REQUIRE(textureId);
			context.Editor.GetUiState().SetSelectedAsset(*textureId);
			REQUIRE(test.Frame());
			REQUIRE(test.Ui.Click(draw, InspectorLayerItem(test, ImHashStr("Import settings", 0, inspector->ID)).GetCenter()));
			const std::string importPath = "2:" + textureId->ToString() + ":.GenerateMips:Edit:" + InspectorLayerOwnerIdentity(context, true);
			const ImGuiID importScope = ImHashStr("ImportSettings", 0, inspector->ID);
			const ImGuiID importProperty = ImHashStr("Property", 0, InspectorLayerIntId(importScope, static_cast<int>(FNV1a32(importPath))));
			REQUIRE(test.Ui.Click(draw, InspectorLayerItem(test, ImHashStr("##Value", 0, importProperty)).GetCenter()));
			const auto importBefore = context.Editor.GetHistory().GetUndoCount();
			REQUIRE(test.Layer.OnSafePoint(0.0));
			settings = automation.Call("asset.getImportSettings", Json{ { "asset", textureId->ToString() } });
			REQUIRE(settings);
			CHECK((*settings)["settings"]["GenerateMips"] == Json(false));
			CHECK(context.Editor.GetHistory().GetUndoCount() == importBefore + 1);
			REQUIRE(automation.Call("edit.undo", Json::object()));
			settings = automation.Call("asset.getImportSettings", Json{ { "asset", textureId->ToString() } });
			REQUIRE(settings);
			CHECK((*settings)["settings"]["GenerateMips"] == Json(true));
		}

		TEST_CASE("InspectorPanel: primary Euler rotation offers the serialized prefab override revert")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			auto& automation = test.Fixture.GetAutomation();
			REQUIRE(automation.Call("entity.create", Json{ { "name", "PrefabObject" } }));
			REQUIRE(automation.Call("entity.create", Json{ { "name", "Child" }, { "parent", "/PrefabObject" } }));
			REQUIRE(automation.Call("prefab.create", Json{ { "entity", "/PrefabObject" }, { "path", "Assets/Prefabs/Object.prefab" }, { "replaceWithInstance", true } }));
			// Root transforms are implicit instance placement; a child records a revertible field override.
			REQUIRE(automation.Call("entity.update", Json{ { "entity", "/PrefabObject/Child" }, { "components", Json{ { "Transform", Json{ { "EulerAngles", Json::array({ 0, 45, 0 }) } } } } } }));
			const UUID root = context.Editor.GetScene().FindEntityByPath("/PrefabObject").GetUUID();
			const UUID id = context.Editor.GetScene().FindEntityByPath("/PrefabObject/Child").GetUUID();
			const auto overrides = context.Editor.GetScene().FindEntityByID(root).GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(overrides.size() == 1);
			CHECK(overrides.front().Kind == PrefabOverrideKind::Field);
			CHECK(overrides.front().Component == "Transform");
			CHECK(overrides.front().Field == "Rotation");
			CHECK(overrides.front().Path == std::vector<UUID>{ context.Editor.GetScene().FindEntityByID(id).GetComponent<PrefabLinkComponent>().PrefabEntityID });
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const auto before = context.Editor.GetHistory().GetUndoCount();
			const ImGuiID revert = ImHashStr("Revert", 0, ImHashStr("EulerAngles", 0, InspectorLayerComponentId("Transform")));
			REQUIRE(test.Ui.Click(draw, InspectorLayerItem(test, revert).GetCenter()));
			CHECK(context.Editor.GetHistory().GetUndoCount() == before);
			CHECK(context.Editor.GetScene().FindEntityByID(root).GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);
			REQUIRE(test.Layer.OnSafePoint(0.0));
			CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(context.Editor.GetScene().FindEntityByID(root).GetComponent<PrefabInstanceComponent>().Overrides.empty());
			CHECK(context.Editor.GetHistory().GetUndoCount() == before + 1);
			REQUIRE(automation.Call("edit.undo", Json::object()));
			CHECK(context.Editor.GetHistory().GetUndoCount() == before);
			const auto restored = ComponentAccess::GetFieldValue(context.Editor.GetScene().FindEntityByID(id), "Transform", "EulerAngles");
			REQUIRE(restored);
			CHECK(restored->AsVec3().y == doctest::Approx(45.0f));
			const auto restoredOverrides = context.Editor.GetScene().FindEntityByID(root).GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(restoredOverrides.size() == 1);
			CHECK(restoredOverrides.front().Field == "Rotation");
			CHECK(restoredOverrides.front().Value.Get() == overrides.front().Value.Get());
		}

		TEST_CASE("InspectorPanel: a text entry released by activating another field commits before that field's gesture")
		{
			// Position is drawn before Scale. Pressing one while the other holds typed text releases the text entry in
			// the same frame the drag activates, whichever of the two is drawn first.
			for (const bool typedFirst : { true, false })
			{
				CAPTURE(typedFirst);
				InspectorLayerHarness test;
				auto& context = test.Fixture.GetContext();
				const UUID id = context.Editor.GetScene().CreateEntity("Object").GetUUID();
				REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
				REQUIRE(test.Frame());
				REQUIRE(test.Frame());
				const auto draw = [&test]()
				{
					return test.Draw();
				};
				const auto transform = [&context, id]()
				{
					return context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>();
				};
				const char* typed = typedFirst ? "Translation" : "Scale";
				const char* held = typedFirst ? "Scale" : "Translation";
				const ImVec2 typedPoint = InspectorLayerItem(test, InspectorLayerAxisId(context, "Transform", typed, 0)).GetCenter();
				const ImVec2 heldPoint = InspectorLayerItem(test, InspectorLayerAxisId(context, "Transform", held, 1)).GetCenter();
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
				REQUIRE(test.Ui.Click(draw, typedPoint));
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
				REQUIRE(test.Frame());
				InspectorLayerReplaceText(test.Ui, draw, "3");
				REQUIRE(context.InspectorEdits.IsEditing());
				ImGui::GetIO().MouseDragThreshold = 0.0f;
				ImGui::GetIO().AddMousePosEvent(heldPoint.x, heldPoint.y);
				REQUIRE(test.Frame());
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				REQUIRE(test.Frame());
				REQUIRE(test.Layer.OnSafePoint(0.0));
				// The released text is its own undo step; the held drag is not committed.
				CHECK((typedFirst ? transform().Translation.x : transform().Scale.x) == 3.0f);
				CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
				CHECK(context.InspectorEdits.IsEditing());
				ImGui::GetIO().AddMousePosEvent(heldPoint.x + 4.0f, heldPoint.y);
				REQUIRE(test.Frame());
				REQUIRE(test.Layer.OnSafePoint(0.0));
				CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				REQUIRE(test.Frame());
				REQUIRE(test.Layer.OnSafePoint(0.0));
				CHECK(context.Editor.GetHistory().GetUndoCount() == 2);
				CHECK((typedFirst ? transform().Scale.y : transform().Translation.y) == doctest::Approx(typedFirst ? 1.2f : 0.2f));
				CHECK((typedFirst ? transform().Translation.x : transform().Scale.x) == 3.0f);
			}
		}

		TEST_CASE("InspectorPanel: tabbing out of a typed field commits it as its own step")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			const UUID id = context.Editor.GetScene().CreateEntity("Object").GetUUID();
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const ImVec2 point = InspectorLayerItem(test, InspectorLayerAxisId(context, "Transform", "Translation", 2)).GetCenter();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			REQUIRE(test.Ui.Click(draw, point));
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			REQUIRE(test.Frame());
			InspectorLayerReplaceText(test.Ui, draw, "5");
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, true);
			REQUIRE(test.Frame());
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, false);
			REQUIRE(test.Frame());
			REQUIRE(test.Layer.OnSafePoint(0.0));
			CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Translation.z == 5.0f);
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			// Whatever the focus moved to has not changed, so no further step is recorded.
			REQUIRE(test.Frame());
			REQUIRE(test.Layer.OnSafePoint(0.0));
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
		}

		TEST_CASE("InspectorPanel: a collider rotation edits as Euler degrees and commits a unit quaternion")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			const Entity entity = context.Editor.GetScene().CreateEntity("Box");
			REQUIRE(ComponentAccess::AddComponent(entity, "BoxCollider", nullptr));
			REQUIRE(context.Editor.SetSelection({ entity.GetUUID() }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			InspectorLayerTypeInto(test, InspectorLayerItem(test, InspectorLayerAxisId(context, "BoxCollider", "Rotation", 1)).GetCenter(), "30");
			REQUIRE(test.Layer.OnSafePoint(0.0));
			const auto rotation = ComponentAccess::GetFieldValue(context.Editor.GetScene().FindEntityByID(entity.GetUUID()), "BoxCollider", "Rotation");
			REQUIRE(rotation);
			const glm::quat expected = TransformSystem::QuaternionFromEulerDegrees({ 0.0f, 30.0f, 0.0f });
			CHECK(std::fabs(glm::dot(rotation->AsQuat(), expected)) == doctest::Approx(1.0f));
			CHECK(glm::length(rotation->AsQuat()) == doctest::Approx(1.0f));
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(test.Fixture.GetAutomation().Call("edit.undo", Json::object()));
			const auto restored = ComponentAccess::GetFieldValue(context.Editor.GetScene().FindEntityByID(entity.GetUUID()), "BoxCollider", "Rotation");
			REQUIRE(restored);
			CHECK(restored->AsQuat() == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		}

		TEST_CASE("InspectorPanel: a selection made before Play inspects and edits the running scene transiently")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			const UUID id = context.Editor.GetScene().CreateEntity("Player").GetUUID();
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Fixture.GetAutomation().Call("play.start", Json{ { "lockstep", true } }));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			InspectorLayerTypeInto(test, InspectorLayerItem(test, InspectorLayerAxisId(context, "Transform", "Translation", 0)).GetCenter(), "7");
			REQUIRE(test.Layer.OnSafePoint(0.0));
			const auto* session = context.Editor.GetPlay().GetSession();
			REQUIRE(session != nullptr);
			CHECK(session->GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Translation.x == 7.0f);
			CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Translation.x == 0.0f);
			CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
			REQUIRE(test.Fixture.GetAutomation().Call("play.stop", Json::object()));
			CHECK(context.Editor.GetScene().FindEntityByID(id).GetComponent<TransformComponent>().Translation.x == 0.0f);
		}

		TEST_CASE("InspectorPanel: dragging one axis of a mixed vector keeps every object's other axes")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID first = scene.CreateEntity("First").GetUUID();
			const UUID second = scene.CreateEntity("Second").GetUUID();
			REQUIRE(ComponentAccess::SetFieldValue(scene.FindEntityByID(second), "Transform", "Translation", Value::FromVec3({ 0.0f, 5.0f, 0.0f })));
			REQUIRE(context.Editor.SetSelection({ first, second }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			test.CaptureText = true;
			REQUIRE(test.Frame());
			// Only the differing axis is shown as mixed.
			CHECK(test.Text.contains("X 0"));
			CHECK(test.Text.contains("--"));
			CHECK(test.Text.contains("Z 0"));
			test.CaptureText = false;
			InspectorLayerDrag(test, InspectorLayerItem(test, InspectorLayerAxisId(context, "Transform", "Translation", 0)).GetCenter(), 4.0f);
			REQUIRE(test.Layer.OnSafePoint(0.0));
			const auto translation = [&scene](UUID id)
			{
				return scene.FindEntityByID(id).GetComponent<TransformComponent>().Translation;
			};
			CHECK(translation(first).x == doctest::Approx(0.2f));
			CHECK(translation(first).y == 0.0f);
			CHECK(translation(second).x == doctest::Approx(0.2f));
			CHECK(translation(second).y == 5.0f);
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(test.Fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(translation(second) == glm::vec3(0.0f, 5.0f, 0.0f));
		}

		TEST_CASE("InspectorPanel: runtime selections draw with the play accent")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			const UUID id = context.Editor.GetScene().CreateEntity("Player").GetUUID();
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const char* title = Utils::EditorWindowTitle(EditorPanel::Inspector);
			CHECK_FALSE(InspectorLayerWindowUsesColor(title, Utils::EditorPlayAccentColor()));
			REQUIRE(test.Fixture.GetAutomation().Call("play.start", Json{ { "lockstep", true } }));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			REQUIRE(context.Editor.GetSelectionTarget() == SceneTarget::Play);
			CHECK(InspectorLayerWindowUsesColor(title, Utils::EditorPlayAccentColor()));
			REQUIRE(test.Fixture.GetAutomation().Call("play.stop", Json::object()));
			REQUIRE(test.Frame());
			CHECK_FALSE(InspectorLayerWindowUsesColor(title, Utils::EditorPlayAccentColor()));
		}

		TEST_CASE("InspectorPanel: an unreadable material shows its error inline and is not re-read every frame")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			const auto created = test.Fixture.GetAutomation().Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Broken.material" } });
			REQUIRE(created);
			const auto id = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(id);
			const auto path = VfsPath::Create("project", "Assets/Materials/Broken.material");
			REQUIRE(path);
			// Another tool rewrites the file behind the editor's back with something that does not parse.
			REQUIRE(context.Editor.GetVfs().WriteFileAtomic(*path, AsBytes("not a material")));
			context.Editor.GetUiState().SetSelectedAsset(*id);
			const Test::ExpectLog warned(LogLevel::Warn, "cannot read the properties");
			for (int frame = 0; frame < 4; ++frame)
				REQUIRE(test.Frame());
			CHECK(warned.GetMatchCount() == 1);
			test.CaptureText = true;
			REQUIRE(test.Frame());
			CHECK(test.Text.contains("These properties cannot be read"));
			CHECK(test.Text.contains("Broken.material"));
		}

		TEST_CASE("InspectorPanel: an action error clears on dismiss, on a new selection and after a later success")
		{
			Test::PanelInteractionFixture fixture("InspectorErrors");
			auto& context = fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID first = scene.CreateEntity("First").GetUUID();
			const UUID second = scene.CreateEntity("Second").GetUUID();
			REQUIRE(context.Editor.SetSelection({ first }, SceneTarget::Edit));
			InspectorPanel panel;
			Test::PanelInteractionUi ui;
			Utils::ApplyEditorStyle();
			std::string text;
			const auto draw = [&panel, &context, &text]() -> Status
			{
				ImGui::LogToBuffer(0);
				const Status status = panel.Draw(context);
				text = ImGui::GetCurrentContext()->LogBuffer.c_str();
				ImGui::LogFinish();
				return status;
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			const Test::ExpectLog failures(LogLevel::Error, "InspectorPanel:");
			// Renaming to an empty name is refused when the queued action runs.
			const auto rename = [&](UUID id, const char* name)
			{
				const auto* window = ImGui::FindWindowByName("Panel interaction");
				REQUIRE(window != nullptr);
				const int scope = static_cast<int>(FNV1a32(id.ToString() + ":" + InspectorLayerOwnerIdentity(context)));
				const ImGuiID field = ImHashStr("##EntityName", 0, InspectorLayerIntId(window->ID, scope));
				REQUIRE(ui.Click(draw, InspectorLayerItem(ui, draw, "Panel interaction", field).GetCenter()));
				InspectorLayerReplaceText(ui, draw, name);
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, name[0] == '\0');
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
				context.Actions.Pump();
				REQUIRE(ui.Frame(draw));
			};
			rename(first, "");
			CHECK(failures.GetMatchCount() == 1);
			CHECK(text.contains("Dismiss"));
			REQUIRE(ui.Frame(draw));
			CHECK(text.contains("Dismiss"));
			auto* window = ImGui::FindWindowByName("Panel interaction");
			REQUIRE(window != nullptr);
			REQUIRE(ui.Click(draw, InspectorLayerItem(ui, draw, "Panel interaction", window->GetID("Dismiss##EditorError")).GetCenter()));
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(text.contains("Dismiss"));
			rename(first, "");
			CHECK(failures.GetMatchCount() == 2);
			CHECK(text.contains("Dismiss"));
			REQUIRE(context.Editor.SetSelection({ second }, SceneTarget::Edit));
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(text.contains("Dismiss"));
			rename(second, "");
			CHECK(failures.GetMatchCount() == 3);
			CHECK(text.contains("Dismiss"));
			rename(second, "Renamed");
			CHECK_FALSE(text.contains("Dismiss"));
			CHECK(scene.FindEntityByID(second).GetName() == "Renamed");
		}

		TEST_CASE("InspectorPanel: a destroyed entity leaves the rest of a multi-selection inspectable")
		{
			InspectorLayerHarness test;
			auto& context = test.Fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID kept = scene.CreateEntity("Kept").GetUUID();
			const UUID destroyed = scene.CreateEntity("Destroyed").GetUUID();
			REQUIRE(context.Editor.SetSelection({ kept, destroyed }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Fixture.GetAutomation().Call("entity.destroy", Json{ { "entities", Json::array({ destroyed.ToString() }) } }));
			test.CaptureText = true;
			REQUIRE(test.Frame());
			CHECK(test.Text.contains("Transform"));
			CHECK(test.Text.contains("Scene object"));
			CHECK_FALSE(test.Text.contains("objects selected"));
			REQUIRE(test.Fixture.GetAutomation().Call("entity.destroy", Json{ { "entities", Json::array({ kept.ToString() }) } }));
			REQUIRE(test.Frame());
			CHECK(test.Text.contains("Nothing selected"));
		}
	}

}
