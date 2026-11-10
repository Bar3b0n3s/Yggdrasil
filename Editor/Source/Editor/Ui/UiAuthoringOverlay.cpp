#include "EditorPCH.h"
#include "Editor/Ui/UiAuthoringOverlay.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiLayout.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>

#include <array>
#include <span>

namespace Engine {

	namespace Utils {

		// The cursor that shows what dragging `handle` does.
		static ImGuiMouseCursor GetUiRectHandleCursor(UiRectHandle handle)
		{
			switch (handle)
			{
				case UiRectHandle::Move:
					return ImGuiMouseCursor_ResizeAll;
				case UiRectHandle::Left:
				case UiRectHandle::Right:
					return ImGuiMouseCursor_ResizeEW;
				case UiRectHandle::Top:
				case UiRectHandle::Bottom:
					return ImGuiMouseCursor_ResizeNS;
				case UiRectHandle::TopLeft:
				case UiRectHandle::BottomRight:
					return ImGuiMouseCursor_ResizeNWSE;
				case UiRectHandle::TopRight:
				case UiRectHandle::BottomLeft:
					return ImGuiMouseCursor_ResizeNESW;
			}
			return ImGuiMouseCursor_Arrow;
		}

	}

	Status UiAuthoringOverlay::Queue(EditorPanelContext& context, std::string_view method, const Json& params)
	{
		ENGINE_TRY_ASSIGN(const uint64_t ticket, context.Actions.Submit(method, params));
		m_Tickets.push_back(ticket);
		return {};
	}

	Status UiAuthoringOverlay::CollectActions(EditorPanelContext& context)
	{
		Status status;
		std::erase_if(m_Tickets, [&context, &status](uint64_t ticket)
		{
			Result<std::optional<Result<Json>>> completion = context.Actions.TakeResult(ticket);
			if (!completion)
			{
				if (status)
					status = std::unexpected(std::move(completion).error());
				return true;
			}
			if (!completion->has_value())
				return false;
			if (!completion->value() && status)
				status = std::unexpected(std::move(completion->value()).error());
			return true;
		});
		return status;
	}

	Status UiAuthoringOverlay::Draw(EditorPanelContext& context, const UiPreviewFrame& frame, glm::uvec2 imageSize, bool& active)
	{
		active = false;
		Status status = CollectActions(context);
		const auto record = [&status](Status next)
		{
			if (!next && status)
				status = std::unexpected(std::move(next).error());
		};
		EditorContext& editor = context.Editor;
		// The scene the scene view shows (M20 decisions 13 and 23), which the preview's image renders.
		PlaySession* session = editor.ShowsPlayScene() ? editor.GetPlay().GetSession() : nullptr;
		const Scene* scene = session != nullptr ? &session->GetScene() : editor.HasScene() ? &editor.GetScene()
																						   : nullptr;
		if (scene == nullptr || frame.Scale <= 0.0f || imageSize.x == 0 || imageSize.y == 0)
		{
			m_Drag.reset();
			m_Click.reset();
			return status;
		}
		const SceneTarget target = session != nullptr ? SceneTarget::Play : SceneTarget::Edit;
		const UiLayoutTable table = ComputeUiLayout(*scene, imageSize);
		const auto toPanel = [&frame](glm::vec2 point)
		{
			return frame.Min + point * frame.Scale;
		};
		// C22.3 (the review of C22.3): a World canvas's elements are in its own canvas pixels, not the frame's, so the preview
		// neither outlines nor picks them, nor gives one the rect tool (UiAuthoring.h's PickUiElement).
		const auto isWorldElement = [&table](const UiLayoutNode& node)
		{
			const UiCanvasLayout* canvas = FindUiCanvasLayout(table, node.Canvas);
			return canvas != nullptr && canvas->RenderMode == UiCanvasRenderMode::World;
		};
		ImDrawList& drawList = *ImGui::GetWindowDrawList();
		const float uiScale = Utils::EditorUiScale();
		for (const UiLayoutNode& node : table.Nodes)
		{
			if (node.Hidden || isWorldElement(node))
				continue;
			const glm::vec2 min = toPanel(node.Min);
			const glm::vec2 max = toPanel(node.Max);
			drawList.AddRect(ImVec2(min.x, min.y), ImVec2(max.x, max.y), IM_COL32(255, 255, 255, 96));
		}

		// One element selected in the shown scene: the rect tool's subject.
		const std::span<const UUID> selection = editor.GetSelectionTarget() == target ? editor.GetSelection() : std::span<const UUID>();
		const UiLayoutNode* selected = selection.size() == 1 ? FindUiLayoutNode(table, selection.front()) : nullptr;
		if (selected != nullptr && isWorldElement(*selected))
			selected = nullptr;
		const UiCanvasLayout* canvas = selected != nullptr ? FindUiCanvasLayout(table, selected->Canvas) : nullptr;
		const ConstEntity selectedEntity = selected != nullptr ? scene->FindEntityByID(selected->Entity) : ConstEntity();
		const bool editable = canvas != nullptr && canvas->Scale > 0.0f && selectedEntity.IsValid()
			&& selectedEntity.HasComponent<UiRectComponent>() && !editor.IsReadOnly();
		const float grab = 5.0f * uiScale;

		ImGui::SetCursorScreenPos(ImVec2(frame.Min.x, frame.Min.y));
		ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
		ImGui::InvisibleButton("##UiAuthoring", ImVec2(frame.Size.x, frame.Size.y), ImGuiButtonFlags_MouseButtonLeft);
		ImGui::PopItemFlag();
		active = ImGui::IsItemActive();
		const bool pressed = ImGui::IsItemActivated();
		const bool released = ImGui::IsItemDeactivated();
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 mouseImGui = ImGui::GetIO().MousePos;
		const glm::vec2 mouse(mouseImGui.x, mouseImGui.y);

		// The selected element's rectangle in panel pixels, and the handle under the pointer.
		glm::vec2 selectedMin(0.0f);
		glm::vec2 selectedMax(0.0f);
		std::optional<UiRectHandle> handle;
		if (selected != nullptr)
		{
			selectedMin = toPanel(selected->Min);
			selectedMax = toPanel(selected->Max);
			if (editable)
				handle = FindUiRectHandle(selectedMin, selectedMax, mouse, grab);
		}

		if (pressed)
		{
			m_Drag.reset();
			m_Click = mouse;
			if (handle)
			{
				m_Drag = Drag{ .Entity = selected->Entity,
					.Handle = *handle,
					.Start = selectedEntity.GetComponent<UiRectComponent>(),
					.StartSize = (selected->Max - selected->Min) / canvas->Scale,
					.StartMin = selected->Min,
					.CanvasScale = canvas->Scale,
					.PressPoint = mouse,
					.Pointer = mouse,
					.Revision = editor.GetRevision(),
					// An edge or a corner drags from the press; the body only once the pointer moves, so a click on it selects.
					.Started = *handle != UiRectHandle::Move };
			}
		}
		// A drag ends without a write when it is cancelled or its element can no longer take it.
		if (m_Drag && (ImGui::IsKeyPressed(ImGuiKey_Escape) || editor.IsReadOnly() || selected == nullptr || selected->Entity != m_Drag->Entity))
		{
			m_Drag.reset();
			m_Click.reset();
		}
		if (m_Drag && !m_Drag->Started && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		{
			m_Drag->Started = true;
			m_Click.reset();
		}
		if (m_Drag && m_Drag->Started)
			m_Click.reset();

		// The pointer the drag follows: its last valid position, so a frame without one (the cursor lost by the platform)
		// never turns into a non-finite delta.
		if (m_Drag && ImGui::IsMousePosValid())
			m_Drag->Pointer = mouse;
		std::optional<UiRectComponent> dragged;
		if (m_Drag && m_Drag->Started)
		{
			const glm::vec2 delta = (m_Drag->Pointer - m_Drag->PressPoint) / (frame.Scale * m_Drag->CanvasScale);
			dragged = DragUiRect(m_Drag->Start, m_Drag->StartSize, m_Drag->Handle, delta);
			// Where the dragged rectangle lies: its size and Position changed by what the drag changed, the pivot point moving
			// with Position (UiRectComponent.h's rule), in scene pixels and then panel pixels.
			const glm::vec2 sizeChange = dragged->Size - m_Drag->Start.Size;
			const glm::vec2 positionChange = dragged->Position - m_Drag->Start.Position;
			const glm::vec2 min = m_Drag->StartMin + (positionChange - m_Drag->Start.Pivot * sizeChange) * m_Drag->CanvasScale;
			const glm::vec2 max = min + (m_Drag->StartSize + sizeChange) * m_Drag->CanvasScale;
			selectedMin = toPanel(min);
			selectedMax = toPanel(max);
		}
		if (selected != nullptr)
		{
			const ImU32 accent = IM_COL32(255, 196, 64, 255);
			drawList.AddRect(ImVec2(selectedMin.x, selectedMin.y), ImVec2(selectedMax.x, selectedMax.y), accent, 0.0f, 0, 2.0f);
			if (editable)
			{
				const glm::vec2 centre = (selectedMin + selectedMax) * 0.5f;
				const std::array<glm::vec2, 8> handles = { selectedMin, glm::vec2(centre.x, selectedMin.y), glm::vec2(selectedMax.x, selectedMin.y),
					glm::vec2(selectedMax.x, centre.y), selectedMax, glm::vec2(centre.x, selectedMax.y), glm::vec2(selectedMin.x, selectedMax.y),
					glm::vec2(selectedMin.x, centre.y) };
				const float half = 3.0f * uiScale;
				for (const glm::vec2 point : handles)
				{
					drawList.AddRectFilled(ImVec2(point.x - half, point.y - half), ImVec2(point.x + half, point.y + half),
						IM_COL32(255, 255, 255, 255));
					drawList.AddRect(ImVec2(point.x - half, point.y - half), ImVec2(point.x + half, point.y + half), accent);
				}
			}
		}
		if (m_Drag && m_Drag->Started)
			ImGui::SetMouseCursor(Utils::GetUiRectHandleCursor(m_Drag->Handle));
		else if (hovered && handle)
			ImGui::SetMouseCursor(Utils::GetUiRectHandleCursor(*handle));

		if (released)
		{
			if (m_Drag && m_Drag->Started && dragged && (dragged->Position != m_Drag->Start.Position || dragged->Size != m_Drag->Start.Size))
			{
				Json rect{ { "Position", Json::array({ dragged->Position.x, dragged->Position.y }) },
					{ "Size", Json::array({ dragged->Size.x, dragged->Size.y }) } };
				record(Queue(context, "entity.update",
					Json{ { "entity", m_Drag->Entity.ToString() }, { "components", Json{ { "UiRect", std::move(rect) } } },
						{ "target", target == SceneTarget::Play ? "Play" : "Edit" }, { "ifRevision", m_Drag->Revision } }));
			}
			else if (m_Click)
			{
				const UUID picked = PickUiElement(table, MapUiPreviewPointToScene(frame, *m_Click));
				Json entities = Json::array();
				if (picked.IsValid())
					entities.push_back(picked.ToString());
				Json select{ { "entities", std::move(entities) } };
				// edit.select without a scene target reads the play scene from any tab (M20 decision 16): a click on this tab's
				// edit scene while another scene plays names this tab, as the hierarchy's does.
				if (session == nullptr && editor.GetPlay().GetSession() != nullptr)
					select["scene"] = editor.GetActiveSceneId();
				record(Queue(context, "edit.select", select));
			}
			m_Drag.reset();
			m_Click.reset();
		}
		return status;
	}

}
