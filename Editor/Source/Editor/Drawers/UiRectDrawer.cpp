#include "EditorPCH.h"
#include "Editor/Drawers/UiRectDrawer.h"

#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Ui/UiAuthoring.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiLayout.h"

#include <imgui.h>

#include <array>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		static constexpr std::array<std::pair<UiAnchorHorizontal, const char*>, 4> UiAnchorColumns = { { { UiAnchorHorizontal::Left, "Left" },
			{ UiAnchorHorizontal::Center, "Center" },
			{ UiAnchorHorizontal::Right, "Right" },
			{ UiAnchorHorizontal::Stretch, "Stretch" } } };
		static constexpr std::array<std::pair<UiAnchorVertical, const char*>, 4> UiAnchorRows = { { { UiAnchorVertical::Top, "Top" },
			{ UiAnchorVertical::Middle, "Middle" },
			{ UiAnchorVertical::Bottom, "Bottom" },
			{ UiAnchorVertical::Stretch, "Stretch" } } };

		// The parent size of `entity` in canvas units in `table` (its canvas's, or its parent element's), (0, 0) when it is not
		// laid out.
		static glm::vec2 FindUiParentSize(const UiLayoutTable& table, UUID entity)
		{
			const UiLayoutNode* node = FindUiLayoutNode(table, entity);
			const UiCanvasLayout* canvas = node != nullptr ? FindUiCanvasLayout(table, node->Canvas) : nullptr;
			if (canvas == nullptr || canvas->Scale <= 0.0f)
				return glm::vec2(0.0f);
			if (node->Parent == node->Canvas)
				return canvas->Size;
			const UiLayoutNode* parent = FindUiLayoutNode(table, node->Parent);
			return parent != nullptr ? (parent->Max - parent->Min) / canvas->Scale : glm::vec2(0.0f);
		}

		// The cell's glyph inside [min, max]: the parent's outline and the anchors on it, a point for a fixed anchor and a line
		// along a stretched axis.
		static void DrawUiAnchorGlyph(ImDrawList& drawList, ImVec2 min, ImVec2 max, UiAnchorHorizontal horizontal, UiAnchorVertical vertical)
		{
			const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
			const float inset = (max.x - min.x) * 0.2f;
			const ImVec2 boxMin(min.x + inset, min.y + inset);
			const ImVec2 boxMax(max.x - inset, max.y - inset);
			drawList.AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_TextDisabled));
			const auto along = [](float from, float to, float t)
			{
				return from + (to - from) * t;
			};
			const float x = along(boxMin.x, boxMax.x, horizontal == UiAnchorHorizontal::Left ? 0.0f : horizontal == UiAnchorHorizontal::Right ? 1.0f
																																			  : 0.5f);
			const float y = along(boxMin.y, boxMax.y, vertical == UiAnchorVertical::Top ? 0.0f : vertical == UiAnchorVertical::Bottom ? 1.0f
																																	  : 0.5f);
			const bool stretchX = horizontal == UiAnchorHorizontal::Stretch;
			const bool stretchY = vertical == UiAnchorVertical::Stretch;
			if (stretchX && stretchY)
				drawList.AddRectFilled(ImVec2(boxMin.x + 2.0f, boxMin.y + 2.0f), ImVec2(boxMax.x - 2.0f, boxMax.y - 2.0f), color);
			else if (stretchX)
				drawList.AddLine(ImVec2(boxMin.x, y), ImVec2(boxMax.x, y), color, 2.0f);
			else if (stretchY)
				drawList.AddLine(ImVec2(x, boxMin.y), ImVec2(x, boxMax.y), color, 2.0f);
			else
				drawList.AddRectFilled(ImVec2(x - 2.0f, y - 2.0f), ImVec2(x + 2.0f, y + 2.0f), color);
		}

	}

	std::optional<UiRectPresetAction> DrawUiRectAnchorPresets(const EditorContext& editor, const Scene& scene, std::span<const UUID> selection,
		SceneTarget target)
	{
		// The preset every selected element has, highlighted.
		std::optional<UiAnchorPreset> current;
		bool shared = !selection.empty();
		for (const UUID id : selection)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			const std::optional<UiAnchorPreset> preset = entity.IsValid() && entity.HasComponent<UiRectComponent>()
				? FindUiAnchorPreset(entity.GetComponent<UiRectComponent>())
				: std::nullopt;
			if (!preset || (current && *current != *preset))
				shared = false;
			current = preset;
		}
		if (!shared)
			current.reset();

		std::optional<UiAnchorPreset> clicked;
		const float cell = ImGui::GetFrameHeight();
		ImGui::PushID("UiRectAnchorPresets");
		ImGui::TextDisabled("Anchor presets");
		ImGui::BeginDisabled(editor.IsReadOnly());
		ImDrawList& drawList = *ImGui::GetWindowDrawList();
		for (const auto& [vertical, rowName] : Utils::UiAnchorRows)
		{
			for (size_t column = 0; column < Utils::UiAnchorColumns.size(); ++column)
			{
				const auto& [horizontal, columnName] = Utils::UiAnchorColumns[column];
				if (column != 0)
					ImGui::SameLine(0.0f, 2.0f);
				const UiAnchorPreset preset{ horizontal, vertical };
				const std::string id = std::format("##Anchor{}{}", columnName, rowName);
				// Navigable like the inspector's other controls (InvisibleButton is not by default).
				if (ImGui::InvisibleButton(id.c_str(), ImVec2(cell, cell), ImGuiButtonFlags_EnableNav))
					clicked = preset;
				const ImVec2 min = ImGui::GetItemRectMin();
				const ImVec2 max = ImGui::GetItemRectMax();
				const bool highlighted = current == preset;
				drawList.AddRectFilled(min, max, ImGui::GetColorU32(highlighted ? ImGuiCol_ButtonActive : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
																																 : ImGuiCol_FrameBg));
				Utils::DrawUiAnchorGlyph(drawList, min, max, horizontal, vertical);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s %s: keeps the rectangle in place. Shift also sets the pivot; Alt also moves it to the anchors.", columnName,
						rowName);
			}
		}
		ImGui::EndDisabled();
		ImGui::PopID();
		if (!clicked || editor.IsReadOnly())
			return std::nullopt;

		const ImGuiIO& io = ImGui::GetIO();
		const UiAnchorPresetOptions options{ .SetPivot = io.KeyShift, .SetPosition = io.KeyAlt };
		// The layout the UI preview shows: the game frame's, at its scene resolution (a PixelPerfectCamera's reference resolution
		// under one; M22's landing, ComputeGameViewOutputMapping).
		UiLayoutTable table;
		if (editor.HasProject())
		{
			const glm::uvec2 frame = GetUiPreviewFrameSize(editor.GetViewportState().GetGameResolution(), editor.GetProject().GetSettings().Window);
			const RenderOutputMapping output = ComputeGameViewOutputMapping(scene, frame);
			table = ComputeUiLayout(scene, glm::uvec2(output.SceneWidth, output.SceneHeight));
		}
		const char* targetName = target == SceneTarget::Play ? "Play" : "Edit";
		Json updates = Json::array();
		for (const UUID id : selection)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !entity.HasComponent<UiRectComponent>())
				continue;
			const UiRectComponent rect = ApplyUiAnchorPreset(entity.GetComponent<UiRectComponent>(), *clicked, Utils::FindUiParentSize(table, id),
				options);
			Json value{ { "AnchorMin", Json::array({ rect.AnchorMin.x, rect.AnchorMin.y }) },
				{ "AnchorMax", Json::array({ rect.AnchorMax.x, rect.AnchorMax.y }) },
				{ "Pivot", Json::array({ rect.Pivot.x, rect.Pivot.y }) },
				{ "Position", Json::array({ rect.Position.x, rect.Position.y }) },
				{ "Size", Json::array({ rect.Size.x, rect.Size.y }) } };
			updates.push_back(
				Json{ { "entity", id.ToString() }, { "components", Json{ { "UiRect", std::move(value) } } }, { "target", targetName } });
		}
		if (updates.empty())
			return std::nullopt;
		if (updates.size() == 1)
			return UiRectPresetAction{ .Method = "entity.update", .Params = std::move(updates.front()) };
		Json ops = Json::array();
		for (Json& update : updates)
			ops.push_back(Json{ { "method", "entity.update" }, { "params", std::move(update) } });
		return UiRectPresetAction{ .Method = "edit.batch", .Params = Json{ { "label", "Anchor preset" }, { "ops", std::move(ops) } } };
	}

}
