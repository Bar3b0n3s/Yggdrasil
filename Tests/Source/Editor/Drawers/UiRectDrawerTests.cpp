#include "TestsPCH.h"
#include "Editor/Drawers/UiRectDrawer.h"

#include "Editor/AssetDesignInteraction.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <imgui_internal.h>

#include <functional>
#include <optional>
#include <vector>

// UiRect's anchor presets (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decision 34): the drawer the inspector
// shows above UiRect's fields, clicked with the mouse; its action goes through EditorActions like the inspector's.

namespace Engine {

	namespace {

		// The id of a preset cell (horizontal then vertical name) of the drawer drawn in `window`.
		ImGuiID UiAnchorCellId(const ImGuiWindow& window, const char* cell)
		{
			return ImHashStr(cell, 0, ImHashStr("UiRectAnchorPresets", 0, window.ID));
		}

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("UiRectDrawer: a preset cell applies its anchors to the selection in one undo step, Shift and Alt also setting the pivot and position")
		{
			Test::PanelInteractionFixture fixture("UiRectDrawer");
			EditorPanelContext& context = fixture.GetContext();
			EditorContext& editor = context.Editor;
			// The preview's frame, which lays the parents out: a 400 x 300 canvas at one scene pixel per unit.
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(400, 300)).has_value());
			UUID first;
			UUID second;
			{
				SceneEdit edit(editor, "Create UI");
				Scene& scene = editor.GetScene();
				const Entity canvas = scene.CreateEntity("Canvas");
				canvas.AddComponent<UiCanvasComponent>();
				const Entity element = scene.CreateEntity("Element", canvas);
				// A 100 x 40 element whose top-left corner is at (10, 20), anchored at the canvas's top-left corner.
				element.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
					.AnchorMax = glm::vec2(0.0f),
					.Pivot = glm::vec2(0.0f),
					.Position = glm::vec2(10.0f, 20.0f),
					.Size = glm::vec2(100.0f, 40.0f) });
				const Entity other = scene.CreateEntity("Other", canvas);
				other.AddComponent<UiRectComponent>();
				first = element.GetUUID();
				second = other.GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			std::vector<UUID> selection{ first };
			std::optional<UiRectPresetAction> action;
			const std::function<Status()> draw = [&editor, &selection, &action]() -> Status
			{
				if (std::optional<UiRectPresetAction> clicked = DrawUiRectAnchorPresets(editor, editor.GetScene(), selection, SceneTarget::Edit))
					action = std::move(clicked);
				return {};
			};
			Test::AssetDesignInteraction ui;
			Utils::ApplyEditorStyle();
			REQUIRE(ui.Frame(draw));
			ImGuiWindow* window = ui.Window("Panel interaction");
			const auto click = [&ui, &draw, window, &action](const char* cell)
			{
				// Located first, then clicked with whatever modifiers the caller holds.
				const ImVec2 centre = ui.Locate(draw, window, UiAnchorCellId(*window, cell)).GetCenter();
				action.reset();
				REQUIRE(ui.Click(draw, centre));
				REQUIRE(action.has_value());
			};
			const auto apply = [&context, &action]()
			{
				const Result<uint64_t> ticket = context.Actions.Submit(action->Method, action->Params);
				REQUIRE_MESSAGE(ticket.has_value(), (ticket ? std::string() : ticket.error().ToString()));
				context.Actions.Pump();
				const Result<std::optional<Result<Json>>> result = context.Actions.TakeResult(*ticket);
				REQUIRE(result.has_value());
				REQUIRE(result->has_value());
				REQUIRE_MESSAGE(result->value().has_value(), (result->value() ? std::string() : result->value().error().ToString()));
			};
			const auto rectOf = [&editor](UUID id)
			{
				return editor.GetScene().FindEntityByID(id).GetComponent<UiRectComponent>();
			};

			// Bottom-right keeps the rectangle: the top-left pivot stays 390 and 280 units before the corner.
			const size_t history = editor.GetHistory().GetUndoCount();
			click("##AnchorRightBottom");
			CHECK(action->Method == "entity.update");
			apply();
			CHECK(rectOf(first).AnchorMin == glm::vec2(1.0f));
			CHECK(rectOf(first).AnchorMax == glm::vec2(1.0f));
			CHECK(rectOf(first).Pivot == glm::vec2(0.0f));
			CHECK(rectOf(first).Position == glm::vec2(-390.0f, -280.0f));
			CHECK(rectOf(first).Size == glm::vec2(100.0f, 40.0f));
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);

			// Shift and Alt: the centre with the pivot, the element moved to it.
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
			click("##AnchorCenterMiddle");
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
			apply();
			CHECK(rectOf(first).AnchorMin == glm::vec2(0.5f));
			CHECK(rectOf(first).Pivot == glm::vec2(0.5f));
			CHECK(rectOf(first).Position == glm::vec2(0.0f));
			CHECK(rectOf(first).Size == glm::vec2(100.0f, 40.0f));

			// Two elements stretched over the canvas with Alt: one edit.batch, one undo step.
			selection = { first, second };
			const size_t before = editor.GetHistory().GetUndoCount();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
			click("##AnchorStretchStretch");
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
			CHECK(action->Method == "edit.batch");
			apply();
			for (const UUID id : { first, second })
			{
				CHECK(rectOf(id).AnchorMin == glm::vec2(0.0f));
				CHECK(rectOf(id).AnchorMax == glm::vec2(1.0f));
				CHECK(rectOf(id).Position == glm::vec2(0.0f));
				CHECK(rectOf(id).Size == glm::vec2(0.0f));
			}
			CHECK(editor.GetHistory().GetUndoCount() == before + 1);
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()).has_value());
			CHECK(rectOf(second).AnchorMax == glm::vec2(0.5f));
			CHECK(rectOf(first).AnchorMax == glm::vec2(0.5f));
		}
	}

}
