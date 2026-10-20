#include "TestsPCH.h"
#include "Editor/Panels/SceneHierarchyPanel.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/WorkspaceTestUi.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"

#include <imgui_internal.h>

#include <functional>
#include <vector>

namespace Engine {

	// Measures a hierarchy item through ImGui's navigation rectangle; actions still use ordinary mouse input.
	static ImRect LocateHierarchyItem(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, ImGuiID id)
	{
		ImRect rectangle;
		const auto locate = [&draw, id, &rectangle]() -> Status
		{
			ImGuiWindow* window = ImGui::FindWindowByName("Panel interaction");
			REQUIRE(window != nullptr);
			ImGui::SetNavWindow(window);
			ImGui::SetNavID(id, ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
			ImGui::GetCurrentContext()->NavIdIsAlive = false;
			ENGINE_TRY(draw());
			const auto& gui = *ImGui::GetCurrentContext();
			REQUIRE(gui.NavIdIsAlive);
			REQUIRE(gui.NavId == id);
			rectangle = ImGui::WindowRectRelToAbs(gui.NavWindow, gui.NavWindow->NavRectRel[gui.NavLayer]);
			return {};
		};
		REQUIRE(ui.Frame(locate));
		return rectangle;
	}

	static ImGuiID HierarchyRowId(UUID entity, const char* label)
	{
		const ImGuiWindow* window = ImGui::FindWindowByName("Panel interaction");
		REQUIRE(window != nullptr);
		return ImHashStr(label, 0, ImHashStr(entity.ToString().c_str(), 0, window->ID));
	}

	static bool HierarchyUsesColor(ImU32 color)
	{
		const ImGuiWindow* window = ImGui::FindWindowByName("Panel interaction");
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

		TEST_CASE("SceneHierarchyPanel: hierarchy active toggles share the user undo history and do not select the row")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Mixed Case Entity" } }));
			const UUID id = editor.GetScene().GetRootEntities().front();
			test.Draw();
			test.Draw();
			const char* title = Utils::EditorWindowTitle(EditorPanel::SceneHierarchy);
			test.Click(title, "##Search");
			ImGui::GetIO().AddInputCharactersUTF8("mixed case");
			test.Draw();
			auto* hierarchy = test.Window(title);
			const ImGuiID rowId = ImHashStr("##Entity", 0, ImHashStr(id.ToString().c_str(), 0, hierarchy->ID));
			const ImRect row = test.Locate(hierarchy, rowId);
			test.Click(ImVec2(row.Max.x - ImGui::GetTextLineHeight() * 0.5f, row.Min.y + ImGui::GetTextLineHeight() * 0.5f));
			CHECK(editor.GetScene().FindEntityByID(id).IsActiveSelf());
			CHECK(editor.GetSelection().empty());
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK_FALSE(editor.GetScene().FindEntityByID(id).IsActiveSelf());
			CHECK(editor.GetSelection().empty());
			const auto history = editor.GetHistory().GetEntries(10);
			REQUIRE(history.size() == 2);
			CHECK(history[0].Origin == CommandOrigin::Agent);
			CHECK(history[1].Origin == CommandOrigin::User);
			REQUIRE(test.Client.Call("edit.undo", Json::object()));
			CHECK(editor.GetScene().FindEntityByID(id).IsActiveSelf());
			REQUIRE(test.Client.Call("edit.redo", Json::object()));
			CHECK_FALSE(editor.GetScene().FindEntityByID(id).IsActiveSelf());
		}

		TEST_CASE("SceneHierarchyPanel: Delete of a prefab member is refused with its hint and keeps the member")
		{
			// The editor's Delete is entity.destroy, which refuses a member that is not its instance's root (ADR 0028 decision
			// 16); the error notice shows the hint, the ways out a human needs too.
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Lamp" } }));
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/Lamp" } }));
			REQUIRE(test.Client.Call("prefab.create", Json{ { "entity", "/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } }));
			const UUID lamp = editor.GetScene().FindEntityByPath("/Lamp").GetUUID();
			const UUID bulb = editor.GetScene().FindEntityByPath("/Lamp/Bulb").GetUUID();
			const size_t steps = editor.GetHistory().GetUndoCount();
			test.Draw();
			test.Draw();
			// A search opens the instance's row, so the member's row is drawn, inside the instance root's tree node.
			const char* title = Utils::EditorWindowTitle(EditorPanel::SceneHierarchy);
			test.Click(title, "##Search");
			ImGui::GetIO().AddInputCharactersUTF8("Bulb");
			test.Draw();
			auto* hierarchy = test.Window(title);
			const ImGuiID lampNode = ImHashStr("##Entity", 0, ImHashStr(lamp.ToString().c_str(), 0, hierarchy->ID));
			const ImGuiID rowId = ImHashStr("##Entity", 0, ImHashStr(bulb.ToString().c_str(), 0, lampNode));
			test.Click(test.Locate(hierarchy, rowId).GetCenter(), ImGuiMouseButton_Right);
			const auto remove = test.FindText("Delete");
			REQUIRE(remove.has_value());
			const Test::ExpectLog failure(LogLevel::Error, "SceneHierarchyPanel:");
			test.Click(*remove);
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScene().FindEntityByID(bulb).IsValid());
			CHECK(editor.GetHistory().GetUndoCount() == steps);
			// The panel takes the action's result in its next frame, which logs the failure and shows it.
			const std::string text = test.Text();
			CHECK(text.contains("prefab.unpack"));
			CHECK(text.contains("Dismiss"));
			CHECK(failure.GetMatchCount() == 1);
		}

		TEST_CASE("SceneHierarchyPanel: pending hierarchy renames reject replaced runtime scenes with the same entity UUID")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Original entity" } }));
			const UUID entity = editor.GetScene().GetRootEntities().front();
			REQUIRE(test.Client.Call("scene.save", Json::object()));
			REQUIRE(editor.GetAssets().Refresh());
			editor.GetAssets().WaitIdle();
			REQUIRE(test.Client.Call("play.start", Json{ { "paused", true } }));
			const uint64_t revision = editor.GetRevision();
			const uint64_t serial = editor.GetPlay().GetSession()->GetSerial();
			const uint64_t generation = editor.GetPlay().GetSession()->GetSceneGeneration();
			test.Draw();
			test.Draw();
			test.OpenRename(entity);
			test.ReplaceText("Rename entity", "##Name", "Renamed entity");
			bool replaced = false;
			SUBCASE("the original runtime scene still accepts the queued rename")
			{
			}
			SUBCASE("stop and restart replaces the session")
			{
				REQUIRE(test.Client.Call("play.stop", Json::object()));
				REQUIRE(test.Client.Call("play.start", Json{ { "paused", true } }));
				CHECK(editor.GetPlay().GetSession()->GetSerial() != serial);
				CHECK(editor.GetPlay().GetSession()->GetSceneGeneration() == generation);
				replaced = true;
			}
			SUBCASE("gameplay scene load replaces the scene within the same session")
			{
				REQUIRE(test.Client.Call("script.eval", Json{ { "context", "play" }, { "code", "Scene.Load(Assets.Load('Assets/Scenes/Main.scene'), {})" } }));
				REQUIRE(test.Client.Call("play.step", Json{ { "ticks", 1 } }));
				CHECK(editor.GetPlay().GetSession()->GetSerial() == serial);
				CHECK(editor.GetPlay().GetSession()->GetSceneGeneration() > generation);
				replaced = true;
			}
			CHECK(editor.GetRevision() == revision);
			REQUIRE(editor.GetPlay().GetSession()->GetScene().FindEntityByID(entity).IsValid());
			test.Draw();
			if (replaced)
				CHECK(test.Text().contains("The scene changed."));
			test.Click("Rename entity", "Rename");
			CHECK(editor.GetPlay().GetSession()->GetScene().FindEntityByID(entity).GetName() == "Original entity");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetPlay().GetSession()->GetScene().FindEntityByID(entity).GetName() == (replaced ? "Original entity" : "Renamed entity"));
			CHECK(editor.GetScene().FindEntityByID(entity).GetName() == "Original entity");
			CHECK(editor.GetRevision() == revision);
			REQUIRE(test.Client.Call("play.stop", Json::object()));
		}

		TEST_CASE("SceneHierarchyPanel: read-only hierarchy keeps selection available and disables entity mutations")
		{
			Test::WorkspaceTestUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Read-only entity" } }));
			REQUIRE(test.Client.Call("scene.save", Json::object()));
			const auto projectFile = editor.GetProject().GetProjectFile();
			const UUID id = editor.GetScene().GetRootEntities().front();
			REQUIRE(editor.CloseProject());
			auto project = ProjectManager::OpenProject(projectFile,
				{ .ReadOnly = true, .ReadOnlyCacheDirectory = test.Environment.GetDirectory() / "ReadOnlyCache" }, editor.GetTypeRegistry());
			REQUIRE(project);
			REQUIRE(editor.OpenProject(std::move(*project)));
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			REQUIRE(test.Layer.OnSafePoint(0));
			test.Draw();
			test.Draw();
			const char* title = Utils::EditorWindowTitle(EditorPanel::SceneHierarchy);
			test.Click(title, "Create entity");
			auto* hierarchy = test.Window(title);
			const auto rowId = ImHashStr("##Entity", 0, ImHashStr(id.ToString().c_str(), 0, hierarchy->ID));
			const ImRect row = test.Locate(hierarchy, rowId);
			test.Click(ImVec2(row.Max.x - ImGui::GetTextLineHeight() * 0.5f, row.Min.y + ImGui::GetTextLineHeight() * 0.5f));
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScene().GetEntityCount() == 1);
			CHECK(editor.GetScene().FindEntityByID(id).IsActiveSelf());
			CHECK(editor.GetHistory().GetEntries(10).empty());
			test.Click(row.GetCenter());
			CHECK(editor.GetSelection().empty());
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == id);
			CHECK(test.Text().contains("Read-only project"));
		}

		TEST_CASE("SceneHierarchyPanel: drag reorder inserts before the target row in both directions")
		{
			Test::PanelInteractionFixture fixture("HierarchyReorder");
			auto& context = fixture.GetContext();
			auto& scene = context.Editor.GetScene();
			const UUID a = scene.CreateEntity("A").GetUUID();
			const UUID b = scene.CreateEntity("B").GetUUID();
			const UUID c = scene.CreateEntity("C").GetUUID();
			const UUID d = scene.CreateEntity("D").GetUUID();
			SceneHierarchyPanel panel;
			Test::PanelInteractionUi ui;
			const std::function<Status()> draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			const auto order = [&scene]()
			{
				return std::vector<UUID>(scene.GetRootEntities().begin(), scene.GetRootEntities().end());
			};
			// Each row's thin strip right above it means "insert before this row". The strip is not a navigation item, so
			// it is found from the row it precedes: one item spacing above the row's top.
			const auto drag = [&](UUID moved, UUID before)
			{
				const ImVec2 source = LocateHierarchyItem(ui, draw, HierarchyRowId(moved, "##Entity")).GetCenter();
				const ImRect row = LocateHierarchyItem(ui, draw, HierarchyRowId(before, "##Entity"));
				const ImVec2 target(row.GetCenter().x, row.Min.y - ImGui::GetStyle().ItemSpacing.y - 1.0f);
				ImGui::GetIO().AddMousePosEvent(source.x, source.y);
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddMousePosEvent(target.x, target.y);
				REQUIRE(ui.Frame(draw));
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				REQUIRE(ui.Frame(draw));
				context.Actions.Pump();
				REQUIRE(ui.Frame(draw));
			};
			drag(a, c);
			CHECK(order() == std::vector<UUID>{ b, a, c, d });
			drag(d, a);
			CHECK(order() == std::vector<UUID>{ b, d, a, c });
			const size_t steps = context.Editor.GetHistory().GetUndoCount();
			CHECK(steps == 2);
			// Dropping just below itself leaves the row where it is, without an undo step.
			drag(b, d);
			CHECK(order() == std::vector<UUID>{ b, d, a, c });
			CHECK(context.Editor.GetHistory().GetUndoCount() == steps);
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(order() == std::vector<UUID>{ b, a, c, d });
		}

		TEST_CASE("SceneHierarchyPanel: the running scene draws with the play accent")
		{
			Test::PanelInteractionFixture fixture("HierarchyPlayAccent");
			auto& context = fixture.GetContext();
			const UUID id = context.Editor.GetScene().CreateEntity("Player").GetUUID();
			SceneHierarchyPanel panel;
			Test::PanelInteractionUi ui;
			const std::function<Status()> draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(HierarchyUsesColor(Utils::EditorPlayAccentColor()));
			REQUIRE(context.Editor.SetSelection({ id }, SceneTarget::Edit));
			REQUIRE(fixture.GetAutomation().Call("play.start", Json{ { "lockstep", true } }));
			REQUIRE(ui.Frame(draw));
			CHECK(HierarchyUsesColor(Utils::EditorPlayAccentColor()));
			REQUIRE(fixture.GetAutomation().Call("play.stop", Json::object()));
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(HierarchyUsesColor(Utils::EditorPlayAccentColor()));
		}

		TEST_CASE("SceneHierarchyPanel: an action error clears on dismiss, a scene change or a later success")
		{
			Test::PanelInteractionFixture fixture("HierarchyErrors");
			auto& context = fixture.GetContext();
			auto& automation = fixture.GetAutomation();
			SceneHierarchyPanel panel;
			Test::PanelInteractionUi ui;
			std::string text;
			const std::function<Status()> draw = [&panel, &context, &text]()
			{
				ImGui::LogToBuffer(0);
				const Status status = panel.Draw(context);
				text = ImGui::GetCurrentContext()->LogBuffer.c_str();
				ImGui::LogFinish();
				return status;
			};
			REQUIRE(ui.Frame(draw));
			const auto button = [&ui, &draw](const char* label)
			{
				ImGuiWindow* window = ImGui::FindWindowByName("Panel interaction");
				REQUIRE(window != nullptr);
				return LocateHierarchyItem(ui, draw, window->GetID(label)).GetCenter();
			};
			const Test::ExpectLog failures(LogLevel::Error, "SceneHierarchyPanel:");
			// An entity queued for the running scene fails when play has stopped before the action runs.
			const auto failCreate = [&]()
			{
				REQUIRE(automation.Call("play.start", Json{ { "lockstep", true } }));
				REQUIRE(ui.Frame(draw));
				REQUIRE(ui.Click(draw, button("Create entity")));
				REQUIRE(automation.Call("play.stop", Json::object()));
				context.Actions.Pump();
				REQUIRE(ui.Frame(draw));
			};
			failCreate();
			CHECK(failures.GetMatchCount() == 1);
			CHECK(text.contains("Dismiss"));
			REQUIRE(ui.Frame(draw));
			CHECK(text.contains("Dismiss"));
			REQUIRE(ui.Click(draw, button("Dismiss##EditorError")));
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(text.contains("Dismiss"));
			failCreate();
			CHECK(failures.GetMatchCount() == 2);
			REQUIRE(automation.Call("play.start", Json{ { "lockstep", true } }));
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(text.contains("Dismiss"));
			REQUIRE(automation.Call("play.stop", Json::object()));
			failCreate();
			CHECK(failures.GetMatchCount() == 3);
			CHECK(text.contains("Dismiss"));
			REQUIRE(ui.Click(draw, button("Create entity")));
			context.Actions.Pump();
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(text.contains("Dismiss"));
			CHECK(context.Editor.GetScene().GetEntityCount() == 1);
		}

		TEST_CASE("SceneHierarchyPanel: a project-settings change or its undo keeps the action error of the shown scene")
		{
			Test::PanelInteractionFixture fixture("HierarchyErrorSettings");
			auto& context = fixture.GetContext();
			auto& automation = fixture.GetAutomation();
			SceneHierarchyPanel panel;
			Test::PanelInteractionUi ui;
			std::string text;
			const std::function<Status()> draw = [&panel, &context, &text]()
			{
				ImGui::LogToBuffer(0);
				const Status status = panel.Draw(context);
				text = ImGui::GetCurrentContext()->LogBuffer.c_str();
				ImGui::LogFinish();
				return status;
			};
			REQUIRE(ui.Frame(draw));
			ImGuiWindow* window = ImGui::FindWindowByName("Panel interaction");
			REQUIRE(window != nullptr);
			const Test::ExpectLog failure(LogLevel::Error, "SceneHierarchyPanel:");
			// An entity queued for the running scene fails when play has stopped before the action runs.
			REQUIRE(automation.Call("play.start", Json{ { "lockstep", true } }));
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Click(draw, LocateHierarchyItem(ui, draw, window->GetID("Create entity")).GetCenter()));
			REQUIRE(automation.Call("play.stop", Json::object()));
			context.Actions.Pump();
			REQUIRE(ui.Frame(draw));
			REQUIRE(text.contains("Dismiss"));
			// Installing project settings advances the editor's revision, but the shown scene is the same document.
			REQUIRE(automation.Call("project.setSettings", Json{ { "patch", Json{ { "Rendering", Json{ { "ShadowMapSize", 512 } } } } } }));
			REQUIRE(ui.Frame(draw));
			CHECK(text.contains("Dismiss"));
			REQUIRE(automation.Call("edit.undo", Json::object()));
			REQUIRE(context.Editor.GetProject().GetSettings().Rendering.ShadowMapSize != 512);
			REQUIRE(ui.Frame(draw));
			CHECK(text.contains("Dismiss"));
			CHECK(failure.GetMatchCount() == 1);
			// Replacing the shown scene still starts afresh.
			REQUIRE(automation.Call("scene.new", Json{ { "path", "Assets/Scenes/Other.scene" } }));
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(text.contains("Dismiss"));
		}
	}

}
