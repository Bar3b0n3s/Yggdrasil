#include "EditorPCH.h"
#include "Editor/Viewport/GizmoOverlay.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>

namespace Engine {

	namespace {

		struct ViewportGizmoIdScope
		{
			ViewportGizmoIdScope() { ImGuizmo::PushID(0x4d3130); }
			~ViewportGizmoIdScope() { ImGuizmo::PopID(); }
			ViewportGizmoIdScope(const ViewportGizmoIdScope&) = delete;
			ViewportGizmoIdScope& operator=(const ViewportGizmoIdScope&) = delete;
		};

	}

	namespace Utils {

		static void CancelViewportGizmo(GizmoController& controller)
		{
			controller.Cancel();
			ImGuizmo::Enable(false);
			ImGuizmo::Enable(true);
		}

	}

	Status DrawGizmoOverlay(EditorContext& context, GizmoController& controller, const CameraData& camera,
		const EditorViewportRect& rectangle, GizmoSettings& settings)
	{
		const ViewportGizmoIdScope idScope;
		const ImGuiIO& io = ImGui::GetIO();
		const bool inputCaptured = io.WantTextInput || ImGui::GetDragDropPayload() != nullptr
			|| ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
			|| io.KeyAlt || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
		if (!rectangle.Focused || rectangle.Size.x <= 0.0f || rectangle.Size.y <= 0.0f || rectangle.ImageSize.x == 0
			|| rectangle.ImageSize.y == 0 || !context.GetViewportState().GetOptions().Gizmos || context.IsReadOnly()
			|| ImGui::IsKeyPressed(ImGuiKey_Escape) || (controller.IsDragging() && inputCaptured))
		{
			Utils::CancelViewportGizmo(controller);
			return {};
		}
		PlaySession* session = context.GetPlay().GetSession();
		const SceneTarget target = session ? SceneTarget::Play : SceneTarget::Edit;
		const Scene* scene = session ? &session->GetScene() : context.HasScene() ? &context.GetScene()
																				 : nullptr;
		if (!scene || context.GetSelectionTarget() != target || context.GetSelection().empty())
		{
			Utils::CancelViewportGizmo(controller);
			return {};
		}
		if (controller.IsDragging())
		{
			scene = controller.GetPreviewScene();
			if (!scene)
			{
				Utils::CancelViewportGizmo(controller);
				return MakeError(ErrorCode::Conflict, "The scene changed during the gizmo drag");
			}
		}
		if (!inputCaptured && rectangle.Hovered && !ImGui::IsAnyItemActive() && !controller.IsDragging())
		{
			if (ImGui::IsKeyPressed(ImGuiKey_W))
				settings.Operation = GizmoOperation::Translate;
			if (ImGui::IsKeyPressed(ImGuiKey_E))
				settings.Operation = GizmoOperation::Rotate;
			if (ImGui::IsKeyPressed(ImGuiKey_R))
				settings.Operation = GizmoOperation::Scale;
			if (ImGui::IsKeyPressed(ImGuiKey_X))
				settings.Space = settings.Space == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World;
		}
		glm::dvec3 center(0.0);
		glm::mat4 pivot(1.0f);
		size_t count = 0;
		for (const UUID id : context.GetSelection())
		{
			const ConstEntity entity = scene->FindEntityByID(id);
			if (!entity)
			{
				Utils::CancelViewportGizmo(controller);
				return MakeError(ErrorCode::Conflict, "The gizmo selection is no longer available");
			}
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(entity);
			if (count == 0)
				pivot = world;
			center += glm::dvec3(world[3]);
			++count;
		}
		pivot[3] = glm::vec4(glm::vec3(center / static_cast<double>(count)), 1.0f);
		const glm::mat4 initial = pivot;
		if (!std::isfinite(camera.NearClip) || !std::isfinite(camera.FarClip) || camera.NearClip <= 0.0f || camera.FarClip <= camera.NearClip
			|| camera.ViewportWidth == 0 || camera.ViewportHeight == 0)
			return MakeError(ErrorCode::InvalidArgument, "The displayed gizmo camera is invalid");
		// ImGuizmo unprojects a conventional OpenGL [-1,+1] forward-depth projection. Rebuild that convention using
		// the displayed camera, replacing reverse-Z instead of handing it the renderer's matrix.
		const float aspect = static_cast<float>(camera.ViewportWidth) / static_cast<float>(camera.ViewportHeight);
		const glm::mat4 projection = camera.ProjectionKind == RenderProjection::Perspective
			? glm::perspectiveRH_NO(glm::radians(camera.VerticalFov), aspect, camera.NearClip, camera.FarClip)
			: glm::orthoRH_NO(-camera.OrthographicSize * aspect, camera.OrthographicSize * aspect,
				  -camera.OrthographicSize, camera.OrthographicSize, camera.NearClip, camera.FarClip);
		ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
		switch (settings.Operation)
		{
			case GizmoOperation::Translate: operation = ImGuizmo::TRANSLATE; break;
			case GizmoOperation::Rotate:    operation = ImGuizmo::ROTATE; break;
			case GizmoOperation::Scale:     operation = ImGuizmo::SCALE; break;
		}
		ImGuizmo::SetDrawlist();
		ImGuizmo::SetRect(rectangle.Min.x, rectangle.Min.y, rectangle.Size.x, rectangle.Size.y);
		ImGuizmo::SetOrthographic(camera.ProjectionKind == RenderProjection::Orthographic);
		ImGuizmo::Enable(!inputCaptured && (!ImGui::IsAnyItemActive() || controller.IsDragging())
			&& (controller.IsDragging() || rectangle.Hovered));
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const int clipDepth = drawList->_ClipRectStack.Size;
		const float step = settings.Operation == GizmoOperation::Translate ? settings.TranslationSnap
			: settings.Operation == GizmoOperation::Rotate                 ? settings.RotationSnap
																		   : settings.ScaleSnap;
		const glm::vec3 snap(step);
		const bool changed = ImGuizmo::Manipulate(glm::value_ptr(camera.View), glm::value_ptr(projection), operation,
			settings.Space == GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(pivot), nullptr,
			(settings.Snap || io.KeyCtrl) ? glm::value_ptr(snap) : nullptr);
		// The pinned upstream has a behind-camera early return before PopClipRect (Vendor/ImGuizmo/VENDOR.md).
		while (drawList->_ClipRectStack.Size > clipDepth)
			drawList->PopClipRect();
		const bool usingGizmo = ImGuizmo::IsUsing();
		if (usingGizmo && !controller.IsDragging())
		{
			GizmoSettings gesture = settings;
			// The UI adapter snaps while Ctrl is held, including mid-drag changes. CPU callers can use Begin's fixed snap.
			gesture.Snap = false;
			const Status begun = controller.Begin(context.GetSelection(), initial, gesture);
			if (!begun)
			{
				Utils::CancelViewportGizmo(controller);
				return begun;
			}
		}
		if (changed && controller.IsDragging())
		{
			const Status updated = controller.Update(pivot);
			if (!updated)
			{
				if (updated.error().GetCode() == ErrorCode::Conflict)
					Utils::CancelViewportGizmo(controller);
				return updated;
			}
		}
		if (!usingGizmo && controller.IsDragging())
		{
			ENGINE_TRY(controller.End());
		}
		return {};
	}

}
