#include "TestsPCH.h"
#include "Editor/Panels/InspectorPanel.h"

#include "Editor/Drawers/ReflectedDrawers.h"
#include "Editor/EditorLayer.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/AssetTestFixture.h"

#include <imgui_internal.h>

#include <array>
#include <functional>
#include <string>

namespace Engine {

	namespace {

		struct InspectorDesignHarness
		{
			Test::PanelInteractionFixture Fixture{ "InspectorDesign" };
			EditorLayer Layer{ Fixture.GetContext() };
			Test::PanelInteractionUi Ui{ false };
			std::string Text{};
			bool CaptureText = false;

			InspectorDesignHarness()
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

	static ImGuiID InspectorDesignIntId(ImGuiID parent, int key)
	{
		return ImHashData(&key, sizeof(key), parent);
	}

	static ImGuiID InspectorDesignComponentId(std::string_view component)
	{
		const auto* window = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::Inspector));
		REQUIRE(window != nullptr);
		return InspectorDesignIntId(window->ID, static_cast<int>(FNV1a32(component)));
	}

	static std::string InspectorDesignOwnerIdentity(const EditorPanelContext& context, bool asset = false)
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
		return identity + ":Edit:" + std::to_string(editor.GetRevision() - editor.GetScene().GetRevision());
	}

	static ImGuiID InspectorDesignPropertyId(const EditorPanelContext& context, std::string_view component, std::string_view field, bool variant = false)
	{
		const auto& editor = context.Editor;
		std::string path = "0:0000000000000000:" + std::string(component) + "." + std::string(field);
		for (const UUID id : editor.GetSelection())
			path += ":" + id.ToString();
		path += editor.GetSelectionTarget() == SceneTarget::Play ? ":Play" : ":Edit";
		path += ":" + InspectorDesignOwnerIdentity(context);
		ImGuiID id = InspectorDesignIntId(InspectorDesignComponentId(component), static_cast<int>(FNV1a32(path)));
		if (variant)
			id = InspectorDesignIntId(id, static_cast<int>(FNV1a32(path)));
		return ImHashStr("Property", 0, id);
	}

	// ImGui records this rectangle when it submits the requested navigation item. Tests then click the real geometry;
	// they never toggle storage, replace a command, or synthesize a successful widget result.
	static ImRect InspectorDesignItem(Test::PanelInteractionUi& ui, const std::function<Status()>& draw,
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

	static ImRect InspectorDesignItem(InspectorDesignHarness& test, ImGuiID id)
	{
		return InspectorDesignItem(test.Ui, [&test]()
		{
			return test.Draw();
		}, Utils::EditorWindowTitle(EditorPanel::Inspector), id);
	}

	static ImGuiWindow* InspectorDesignPopup(std::string_view child = {})
	{
		for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
		{
			if (window->Active && (child.empty() ? (window->Flags & ImGuiWindowFlags_Popup) != 0 : std::string_view(window->Name).contains(child)))
				return window;
		}
		return nullptr;
	}

	static void InspectorDesignReplaceText(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, const char* text)
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

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorInspectorDesign: an active name buffer cannot rename a replacement scene or restarted play entity")
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
				InspectorDesignReplaceText(ui, draw, "Stale rename");
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

		TEST_CASE("EditorInspectorDesign: replacement owners discard active reflected input and pending field gestures")
		{
			for (const bool commitBeforeReplacement : { false, true })
			{
				CAPTURE(commitBeforeReplacement);
				InspectorDesignHarness test;
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
				const ImGuiID property = InspectorDesignPropertyId(context, "Transform", "Translation");
				const ImGuiID axis = ImHashStr("##Axis", 0, InspectorDesignIntId(property, 0));
				const ImVec2 point = InspectorDesignItem(test, axis).GetCenter();
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
				REQUIRE(test.Ui.Click(draw, point));
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
				REQUIRE(test.Frame());
				InspectorDesignReplaceText(test.Ui, draw, "42");
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

		TEST_CASE("EditorInspectorDesign: transform defaults expose Euler editing and advanced values stay accessible")
		{
			InspectorDesignHarness test;
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
			const ImGuiID advanced = ImHashStr("Advanced transform", 0, InspectorDesignComponentId("Transform"));
			REQUIRE(test.Ui.Click(draw, InspectorDesignItem(test, advanced).GetCenter()));
			REQUIRE(test.Frame());
			CHECK(test.Text.contains("Quaternion"));
			CHECK(test.Text.contains("World Position"));
			CHECK(test.Text.contains("Render Position"));
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			CHECK(editor.GetScene().FindEntityByID(id).GetName() == "Camera###authored-name");
		}

		TEST_CASE("EditorInspectorDesign: Euler entry commits once and undo restores the original transform")
		{
			InspectorDesignHarness test;
			auto& context = test.Fixture.GetContext();
			const UUID id = context.Editor.GetScene().CreateEntity("Rotating object").GetUUID();
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const ImGuiID property = InspectorDesignPropertyId(context, "Transform", "EulerAngles");
			const ImGuiID axis = ImHashStr("##Axis", 0, InspectorDesignIntId(property, 1));
			const ImVec2 point = InspectorDesignItem(test, axis).GetCenter();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			REQUIRE(test.Ui.Click(draw, point));
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			REQUIRE(test.Frame());
			InspectorDesignReplaceText(test.Ui, draw, "45");
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

		TEST_CASE("EditorInspectorDesign: escape and selection changes discard a live transform preview")
		{
			for (const bool changeSelection : { false, true })
			{
				InspectorDesignHarness test;
				auto& context = test.Fixture.GetContext();
				const UUID first = context.Editor.GetScene().CreateEntity("First").GetUUID();
				const UUID second = context.Editor.GetScene().CreateEntity("Second").GetUUID();
				REQUIRE(context.Editor.SetSelection({ first }, SceneTarget::Edit));
				REQUIRE(test.Frame());
				REQUIRE(test.Frame());
				const ImGuiID property = InspectorDesignPropertyId(context, "Transform", "Translation");
				const ImGuiID axis = ImHashStr("##Axis", 0, InspectorDesignIntId(property, 0));
				const ImVec2 point = InspectorDesignItem(test, axis).GetCenter();
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

		TEST_CASE("EditorInspectorDesign: a mixed checkbox previews all objects and commits one undoable change")
		{
			InspectorDesignHarness test;
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
			const ImGuiID checkbox = ImHashStr("##Value", 0, InspectorDesignPropertyId(context, "MeshRenderer", "Visible"));
			REQUIRE(test.Ui.Click(draw, InspectorDesignItem(test, checkbox).GetCenter()));
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

		TEST_CASE("EditorInspectorDesign: component search adds and options remove one atomic multi-selection batch")
		{
			InspectorDesignHarness test;
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
			REQUIRE(test.Ui.Click(draw, InspectorDesignItem(test, ImHashStr("Add component", 0, inspector->ID)).GetCenter()));
			REQUIRE(test.Frame());
			ImGui::GetIO().AddInputCharactersUTF8("mesh renderer");
			REQUIRE(test.Frame());
			ImGuiWindow* results = InspectorDesignPopup("/Components_");
			REQUIRE(results != nullptr);
			const ImGuiID candidate = ImHashStr("Mesh Renderer", 0, InspectorDesignIntId(results->ID, static_cast<int>(FNV1a32("MeshRenderer"))));
			const ImRect entry = InspectorDesignItem(test.Ui, draw, results->Name, candidate);
			REQUIRE(test.Ui.Click(draw, entry.GetCenter()));
			CHECK_FALSE(scene.FindEntityByID(first).HasComponent<MeshRendererComponent>());
			REQUIRE(test.Layer.OnSafePoint(0.0));
			for (const UUID id : { first, second })
				CHECK(scene.FindEntityByID(id).HasComponent<MeshRendererComponent>());
			CHECK(context.Editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(test.Frame());
			const ImGuiID header = ImHashStr("ComponentHeader", 0, InspectorDesignComponentId("MeshRenderer"));
			REQUIRE(test.Ui.Click(draw, InspectorDesignItem(test, ImHashStr("...##Options", 0, header)).GetCenter()));
			REQUIRE(test.Frame());
			ImGuiWindow* options = InspectorDesignPopup();
			REQUIRE(options != nullptr);
			const ImRect remove = InspectorDesignItem(test.Ui, draw, options->Name, ImHashStr("Remove component", 0, options->ID));
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

		TEST_CASE("EditorInspectorDesign: absent script values use their defaults and preserve unknown overrides")
		{
			InspectorDesignHarness test;
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
			const ImGuiID checkbox = ImHashStr("##Value", 0, InspectorDesignPropertyId(context, "Script", "Fields[Enabled]", true));
			REQUIRE(test.Ui.Click(draw, InspectorDesignItem(test, checkbox).GetCenter()));
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

		TEST_CASE("EditorInspectorDesign: material and import settings commit through the existing asset commands")
		{
			InspectorDesignHarness test;
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
			const std::string path = "1:" + id->ToString() + ":.Roughness:Edit:" + InspectorDesignOwnerIdentity(context, true);
			const ImGuiID property = ImHashStr("Property", 0, InspectorDesignIntId(inspector->ID, static_cast<int>(FNV1a32(path))));
			const ImVec2 control = InspectorDesignItem(test, ImHashStr("##Value", 0, property)).GetCenter();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			REQUIRE(test.Ui.Click(draw, control));
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			REQUIRE(test.Frame());
			InspectorDesignReplaceText(test.Ui, draw, "0.25");
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
			REQUIRE(test.Ui.Click(draw, InspectorDesignItem(test, ImHashStr("Import settings", 0, inspector->ID)).GetCenter()));
			const std::string importPath = "2:" + textureId->ToString() + ":.GenerateMips:Edit:" + InspectorDesignOwnerIdentity(context, true);
			const ImGuiID importScope = ImHashStr("ImportSettings", 0, inspector->ID);
			const ImGuiID importProperty = ImHashStr("Property", 0, InspectorDesignIntId(importScope, static_cast<int>(FNV1a32(importPath))));
			REQUIRE(test.Ui.Click(draw, InspectorDesignItem(test, ImHashStr("##Value", 0, importProperty)).GetCenter()));
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

		TEST_CASE("EditorInspectorDesign: primary Euler rotation offers the serialized prefab override revert")
		{
			InspectorDesignHarness test;
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
			CHECK(overrides.front().PrefabEntityID == context.Editor.GetScene().FindEntityByID(id).GetComponent<PrefabLinkComponent>().PrefabEntityID);
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(test.Frame());
			REQUIRE(test.Frame());
			const auto draw = [&test]()
			{
				return test.Draw();
			};
			const auto before = context.Editor.GetHistory().GetUndoCount();
			const ImGuiID revert = ImHashStr("Revert", 0, ImHashStr("EulerAngles", 0, InspectorDesignComponentId("Transform")));
			REQUIRE(test.Ui.Click(draw, InspectorDesignItem(test, revert).GetCenter()));
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

		TEST_CASE("EditorInspectorDesign: vector axes fit narrow and scaled property columns")
		{
			Test::EditorTestFixture fixture("InspectorVectorWidths");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* field = types.FindComponent<TransformComponent>()->FindField("Translation");
			REQUIRE(field != nullptr);
			for (const float scale : { 1.0f, 1.5f })
			{
				for (const float width : { 220.0f, 320.0f, 420.0f })
				{
					INFO(scale, " at ", width);
					Test::PanelInteractionUi ui(false);
					Utils::ApplyEditorStyle(scale);
					Value value = Value::FromVec3(glm::vec3(12.5f, -24.25f, 123.0f));
					const Value original = value;
					ImGuiID property = 0;
					const auto draw = [&types, field, &value, width, &property]() -> Status
					{
						ImGui::SetNextWindowSize(ImVec2(width, 700.0f));
						ImGui::Begin("Vector design");
						ImGui::PushID(static_cast<int>(FNV1a32("Transform.Translation")));
						property = ImGui::GetID("Property");
						ImGui::PopID();
						const auto result = DrawReflectedValue(*field, value, { .Types = types, .Path = "Transform.Translation", .DisplayLabel = "Position" });
						ImGui::End();
						return result ? Status{} : Status(std::unexpected(result.error()));
					};
					REQUIRE(ui.Frame(draw));
					REQUIRE(ui.Frame(draw));
					std::array<ImRect, 3> axes;
					for (int axis = 0; axis < 3; ++axis)
					{
						axes[static_cast<size_t>(axis)] = InspectorDesignItem(ui, draw, "Vector design", ImHashStr("##Axis", 0, InspectorDesignIntId(property, axis)));
						const ImGuiWindow* window = ImGui::FindWindowByName("Vector design");
						const auto& rectangle = axes[static_cast<size_t>(axis)];
						CHECK(rectangle.Min.x >= window->InnerRect.Min.x);
						CHECK(rectangle.Max.x <= window->InnerRect.Max.x);
						CHECK(rectangle.GetWidth() >= ImGui::CalcTextSize("X -1.23").x);
					}
					CHECK_FALSE(axes[0].Overlaps(axes[1]));
					CHECK_FALSE(axes[1].Overlaps(axes[2]));
					CHECK(value == original);
				}
			}
		}
		TEST_CASE("EditorInspectorDesign: authored hash markers keep controls distinct and read-only values unchanged")
		{
			Test::EditorTestFixture fixture("InspectorAuthoredIds");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* field = types.FindComponent<MeshRendererComponent>()->FindField("Visible");
			REQUIRE(field != nullptr);
			for (const bool readOnly : { false, true })
			{
				Test::PanelInteractionUi ui;
				Value first = Value::FromBool(true);
				Value second = Value::FromBool(true);
				std::array<ImGuiID, 2> ids{};
				std::array<ReflectedDrawerResult, 2> results{};
				const auto draw = [&types, field, readOnly, &first, &second, &ids, &results]() -> Status
				{
					for (int index = 0; index < 2; ++index)
					{
						const std::string path = index == 0 ? "Fields[First###Enabled]" : "Fields[Second###Enabled]";
						ImGui::PushID(static_cast<int>(FNV1a32(path)));
						ImGui::PushID("Property");
						ids[static_cast<size_t>(index)] = ImGui::GetID("##Value");
						ImGui::PopID();
						ImGui::PopID();
						ENGINE_TRY_ASSIGN(results[static_cast<size_t>(index)], DrawReflectedValue(*field, index == 0 ? first : second, { .Types = types, .Path = path, .ReadOnly = readOnly, .DisplayLabel = index == 0 ? "First###Enabled" : "Second###Enabled" }));
					}
					return {};
				};
				REQUIRE(ui.Frame(draw));
				CHECK(ids[0] != ids[1]);
				const auto item = InspectorDesignItem(ui, draw, "Panel interaction", ids[0]);
				REQUIRE(ui.Click(draw, item.GetCenter()));
				CHECK(first.AsBool() == readOnly);
				CHECK(second.AsBool());
				CHECK(results[0].Changed == !readOnly);
				CHECK(results[0].Committed == !readOnly);
				CHECK_FALSE(results[1].Changed);
			}
		}

		TEST_CASE("EditorInspectorDesign: narrow references preserve paste and clear with readable lookup")
		{
			Test::EditorTestFixture fixture("InspectorReferenceDesign");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* field = types.FindComponent<MeshRendererComponent>()->FindField("Mesh");
			REQUIRE(field != nullptr);
			Test::PanelInteractionUi ui(false);
			Utils::ApplyEditorStyle();
			Value value = Value::FromAssetRef(BuiltinAssetHandles::CubeMesh);
			Result<ReflectedDrawerResult> result;
			ImGuiID property = 0;
			UUID described{};
			const auto draw = [&types, field, &value, &result, &property, &described]() -> Status
			{
				ImGui::SetNextWindowSize(ImVec2(220.0f, 600.0f));
				ImGui::Begin("Reference design");
				ImGui::PushID(static_cast<int>(FNV1a32("Mesh")));
				property = ImGui::GetID("Property");
				ImGui::PopID();
				result = DrawReflectedValue(*field, value, { .Types = types, .Path = "Mesh", .DescribeReference = [&described](const FieldInfo&, UUID id)
				{
					described = id;
					return std::optional(ReflectedReferenceCandidate{ id, "Readable###resource name", "engine://Meshes/Cube" });
				} });
				ImGui::End();
				return result ? Status{} : Status(std::unexpected(result.error()));
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			CHECK(described == BuiltinAssetHandles::CubeMesh);
			const ImRect browse = InspectorDesignItem(ui, draw, "Reference design", ImHashStr("...##Browse", 0, property));
			const auto* window = ImGui::FindWindowByName("Reference design");
			CHECK(browse.Max.x <= window->InnerRect.Max.x);
			REQUIRE(ui.Click(draw, browse.GetCenter()));
			REQUIRE(ui.Frame(draw));
			ImGuiWindow* popup = InspectorDesignPopup();
			REQUIRE(popup != nullptr);
			const ImRect input = InspectorDesignItem(ui, draw, popup->Name, ImHashStr("##ReferenceId", 0, popup->ID));
			REQUIRE(ui.Click(draw, input.GetCenter()));
			InspectorDesignReplaceText(ui, draw, BuiltinAssetHandles::SphereMesh.ToString().c_str());
			CHECK(value.AsUUID() == BuiltinAssetHandles::CubeMesh);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
			REQUIRE(ui.Frame(draw));
			REQUIRE(result);
			CHECK(result->Committed);
			CHECK(value.AsUUID() == BuiltinAssetHandles::SphereMesh);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
			REQUIRE(ui.Frame(draw));
			const ImRect clear = InspectorDesignItem(ui, draw, "Reference design", ImHashStr("x##Clear", 0, property));
			CHECK(clear.Max.x <= window->InnerRect.Max.x);
			REQUIRE(ui.Click(draw, clear.GetCenter()));
			REQUIRE(result);
			CHECK(result->Committed);
			CHECK_FALSE(value.AsUUID().IsValid());
		}
	}

}
