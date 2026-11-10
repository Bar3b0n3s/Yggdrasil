#include "EditorPCH.h"
#include "Editor/Panels/AnimationPreviewPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Animation/AnimationPreview.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace Engine {

	Status AnimationPreviewPanel::Draw(EditorPanelContext& context)
	{
		const auto report = [this](const Error& error)
		{
			m_Error = error.ToString();
			ENGINE_ERROR("Animation preview: {}", error);
		};
		if (m_Ticket != 0)
		{
			auto result = context.Actions.TakeResult(m_Ticket);
			if (!result || result->has_value())
			{
				m_Ticket = 0;
				if (!result)
					report(result.error());
				else if (!**result)
					report((**result).error());
			}
		}

		// The active edit scene's previewed entity, else its one selected entity when it holds an Animator; nothing while
		// playing, since the play session poses its Animators itself.
		EditorContext& editor = context.Editor;
		if (!editor.HasProject() || !editor.HasScene() || editor.GetPlay().GetSession() != nullptr)
			return {};
		Scene& scene = editor.GetScene();
		const std::optional<AnimationPreviewState>& preview = editor.GetAnimationPreview().GetState();
		const bool previewing = preview.has_value() && preview->Scene == editor.GetActiveSceneId();
		Entity entity;
		if (previewing)
			entity = scene.FindEntityByID(preview->Entity);
		else if (editor.GetSelectionTarget() == SceneTarget::Edit && editor.GetSelection().size() == 1)
			entity = scene.FindEntityByID(editor.GetSelection().front());
		if (!entity.IsValid() || !entity.HasComponent<AnimatorComponent>())
			return {};

		ImGui::SetNextWindowSize(ImVec2(360.0f * Utils::EditorUiScale(), 0.0f), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Animation Preview"))
		{
			const std::string reference = entity.GetUUID().ToString();
			const auto submit = [this, &context, &report](const Json& params)
			{
				auto ticket = context.Actions.Submit("animation.preview", params);
				if (ticket)
				{
					m_Ticket = *ticket;
					m_Error.clear();
				}
				else
					report(ticket.error());
			};
			ImGui::TextUnformatted(scene.GetEntityPath(entity).c_str());
			if (!previewing)
			{
				m_PendingTime.reset();
				ImGui::TextWrapped("Edit scenes draw skinned meshes in their bind pose. The preview poses this Animator without changing the scene.");
				ImGui::BeginDisabled(m_Ticket != 0);
				if (Utils::EditorToolbarButton("Preview", "Pose the Animator at its layers' times, without an undo step or a scene change."))
					submit(Json{ { "entity", reference }, { "time", 0.0 }, { "playing", false } });
				ImGui::EndDisabled();
			}
			else
			{
				ImGui::BeginDisabled(m_Ticket != 0);
				if (Utils::EditorToolbarButton(preview->Playing ? "Pause" : "Play",
						preview->Playing ? "Hold the preview at its time." : "Advance the preview with the editor's frames.", preview->Playing))
					submit(Json{ { "entity", reference }, { "playing", !preview->Playing } });
				ImGui::SameLine();
				if (Utils::EditorToolbarButton("Stop", "End the preview: the entity draws its bind pose again."))
				{
					m_PendingTime.reset();
					submit(Json{ { "entity", reference }, { "stop", true } });
				}
				ImGui::EndDisabled();
				// The slider runs over the longest loaded clip and follows the preview unless it is dragged; a dragged time is
				// sent (paused) whenever no request is pending, so the last one always arrives.
				const float duration = std::max(preview->Duration, 0.0f);
				if (!m_Dragging && !m_PendingTime.has_value())
					m_SliderTime = std::min(preview->Time, duration);
				ImGui::SetNextItemWidth(-1.0f);
				ImGui::BeginDisabled(duration <= 0.0f);
				if (ImGui::SliderFloat("##Time", &m_SliderTime, 0.0f, duration, "%.3f s"))
					m_PendingTime = std::clamp(m_SliderTime, 0.0f, duration);
				m_Dragging = ImGui::IsItemActive();
				ImGui::EndDisabled();
				if (m_PendingTime.has_value() && m_Ticket == 0)
					submit(Json{ { "entity", reference }, { "time", *std::exchange(m_PendingTime, std::nullopt) }, { "playing", false } });
				if (duration <= 0.0f)
					ImGui::TextDisabled("No layer plays a clip that loads.");
			}
			if (!m_Error.empty())
				ImGui::TextWrapped("%s", m_Error.c_str());
		}
		ImGui::End();
		return {};
	}

}
