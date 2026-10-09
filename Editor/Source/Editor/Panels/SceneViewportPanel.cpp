#include "EditorPCH.h"
#include "Editor/Panels/SceneViewportPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "Editor/Viewport/GizmoOverlay.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/EditorUiState.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/EditorCamera.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace Engine {

	Status SceneViewportPanel::QueueAction(EditorPanelContext& context, std::string_view method, const Json& params)
	{
		ENGINE_TRY_ASSIGN(const uint64_t ticket, context.Actions.Submit(method, params));
		m_ActionTickets.push_back(ticket);
		return {};
	}

	Status SceneViewportPanel::CollectActions(EditorPanelContext& context)
	{
		Status status;
		std::erase_if(m_ActionTickets, [&context, &status](uint64_t ticket)
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

	Status SceneViewportPanel::FrameSelection(EditorPanelContext& context)
	{
		const SceneTarget target = context.Editor.GetPlay().IsPlaying() ? SceneTarget::Play : SceneTarget::Edit;
		if (context.Editor.GetSelectionTarget() != target || context.Editor.GetSelection().empty())
			return MakeError(ErrorCode::InvalidState, "Select entities in the displayed scene to frame them");
		Json entities = Json::array();
		for (const UUID id : context.Editor.GetSelection())
			entities.push_back(id.ToString());
		return QueueAction(context, "viewport.frame", Json{ { "entities", std::move(entities) }, { "ifRevision", context.Editor.GetRevision() } });
	}

	Status SceneViewportPanel::AcceptAssetDrop(EditorPanelContext& context)
	{
		if (!ImGui::BeginDragDropTarget())
			return {};
		std::optional<AssetHandle> handle;
		bool invalidPayload = false;
		// Content browser boundary: canonical handle text, including its trailing NUL. No borrowed payload survives Draw.
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENGINE_ASSET"))
		{
			if (payload->Data == nullptr || payload->DataSize != static_cast<int>(UUID::TextLength + 1))
			{
				invalidPayload = true;
			}
			else
			{
				const char* text = static_cast<const char*>(payload->Data);
				if (text[UUID::TextLength] == '\0')
					handle = UUID::FromString(std::string_view(text, UUID::TextLength));
				invalidPayload = !handle || !handle->IsValid();
			}
		}
		ImGui::EndDragDropTarget();
		if (invalidPayload)
			return MakeError(ErrorCode::InvalidArgument, "The dropped asset needs a canonical asset handle");
		if (!handle)
			return {};
		if (context.Editor.IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "Assets cannot be dropped into a read-only scene");
		PlaySession* session = context.Editor.GetPlay().GetSession();
		const Scene* scene = session ? &session->GetScene() : context.Editor.HasScene() ? &context.Editor.GetScene()
																						: nullptr;
		if (!scene)
			return MakeError(ErrorCode::InvalidState, "Open a scene before dropping an asset");
		const std::string reference = handle->ToString();
		const char* target = session ? "play" : "edit";
		const uint64_t revision = context.Editor.GetRevision();
		const AssetType type = context.Editor.GetAssets().GetAssetType(*handle);
		if (type == AssetType::Prefab)
		{
			if (session)
				return MakeError(ErrorCode::InvalidState, "Stop play before instantiating a prefab into the edit scene");
			return QueueAction(context, "prefab.instantiate", Json{ { "prefab", reference }, { "ifRevision", revision } });
		}
		if (type == AssetType::Mesh)
		{
			return QueueAction(context, "entity.create", Json{ { "name", "Mesh" }, { "target", target }, { "ifRevision", revision }, { "components", { { "MeshRenderer", { { "Mesh", reference } } } } } });
		}
		if (type == AssetType::Environment)
		{
			UUID environment;
			for (const UUID id : scene->GetCanonicalOrder())
			{
				const ConstEntity entity = scene->FindEntityByID(id);
				if (entity && entity.HasComponent<EnvironmentComponent>())
				{
					environment = id;
					break;
				}
			}
			Json params{ { "target", target }, { "ifRevision", revision }, { "components", { { "Environment", { { "Environment", reference } } } } } };
			if (environment.IsValid())
			{
				params["entity"] = environment.ToString();
				return QueueAction(context, "entity.update", params);
			}
			params["name"] = "Environment";
			return QueueAction(context, "entity.create", params);
		}
		if (type == AssetType::None)
			return MakeError(ErrorCode::NotFound, "The dropped asset {} is no longer available", *handle);
		return MakeError(ErrorCode::InvalidArgument, "Drop a mesh, prefab or environment into the scene view");
	}

	Status SceneViewportPanel::Draw(EditorPanelContext& context)
	{
		Status status = CollectActions(context);
		const auto record = [&status](Status next)
		{
			if (!next && status)
				status = std::unexpected(std::move(next).error());
		};
		EditorViewportState& state = context.Editor.GetViewportState();
		const std::string title(EditorPanelToString(EditorPanel::SceneViewport));
		bool open = true;
		const bool visible = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (!visible || !open)
		{
			context.Viewports.SetRectangle(ViewportView::Scene, {});
			record(DrawGizmoOverlay(context.Editor, context.Gizmos, {}, {}, m_GizmoSettings));
			m_DragSelection.clear();
			ImGui::End();
			if (!open)
				record(context.Editor.GetUiState().SetPanelOpen(EditorPanel::SceneViewport, false));
			return status;
		}
		const ImGuiIO& io = ImGui::GetIO();
		EditorViewportOptions options = state.GetOptions();
		if (ImGui::Button("View"))
			ImGui::OpenPopup("SceneViewOptions");
		if (ImGui::BeginPopup("SceneViewOptions"))
		{
			ImGui::Checkbox("Grid", &options.Grid);
			ImGui::Checkbox("Gizmos", &options.Gizmos);
			ImGui::Checkbox("Colliders", &options.Colliders);
			ImGui::Checkbox("Icons", &options.Icons);
			ImGui::Checkbox("Wireframe", &options.Wireframe);
			state.SetOptions(options);
			ImGui::Separator();
			ExplicitRenderCamera camera = state.GetCamera();
			bool orthographic = camera.Projection == RenderProjection::Orthographic;
			if (ImGui::Checkbox("Orthographic", &orthographic))
			{
				camera.Projection = orthographic ? RenderProjection::Orthographic : RenderProjection::Perspective;
				record(state.SetCamera(camera));
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(145.0f);
		const std::string debugLabel(RenderDebugViewToString(state.GetDebugView()));
		if (ImGui::BeginCombo("##SceneDebugView", debugLabel.c_str()))
		{
			for (uint32_t index = 0; index < RenderDebugViewCount; ++index)
			{
				const auto view = static_cast<RenderDebugView>(index);
				const std::string label(RenderDebugViewToString(view));
				if (ImGui::Selectable(label.c_str(), view == state.GetDebugView()))
					record(state.SetDebugView(view));
			}
			ImGui::EndCombo();
		}
		const bool draggingAtStart = context.Gizmos.IsDragging();
		const bool playing = context.Editor.GetPlay().IsPlaying();
		ImGui::SameLine();
		ImGui::BeginDisabled(draggingAtStart);
		if (ImGui::Button("Frame (F)"))
			record(FrameSelection(context));
		ImGui::EndDisabled();
		ImGui::BeginDisabled(draggingAtStart || context.Editor.IsReadOnly() || !options.Gizmos);
		if (ImGui::RadioButton("Move (W)", m_GizmoSettings.Operation == GizmoOperation::Translate))
			m_GizmoSettings.Operation = GizmoOperation::Translate;
		ImGui::SameLine();
		if (ImGui::RadioButton("Rotate (E)", m_GizmoSettings.Operation == GizmoOperation::Rotate))
			m_GizmoSettings.Operation = GizmoOperation::Rotate;
		ImGui::SameLine();
		if (ImGui::RadioButton("Scale (R)", m_GizmoSettings.Operation == GizmoOperation::Scale))
			m_GizmoSettings.Operation = GizmoOperation::Scale;
		ImGui::SameLine();
		bool local = m_GizmoSettings.Space == GizmoSpace::Local;
		if (ImGui::Checkbox("Local (X)", &local))
			m_GizmoSettings.Space = local ? GizmoSpace::Local : GizmoSpace::World;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Checkbox("Snap (Ctrl)", &m_GizmoSettings.Snap);
		if (playing)
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Runtime scene: changes are discarded on Stop");
		if (!m_LastError.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_LastError.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Dismiss##SceneError"))
				m_LastError.clear();
		}
		const EditorViewportImage image = context.Viewports.GetImage(ViewportView::Scene);
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const ImVec2 minimum = ImGui::GetCursorScreenPos();
		EditorViewportRect rectangle;
		if (std::isfinite(available.x) && std::isfinite(available.y) && available.x > 0.0f && available.y > 0.0f)
		{
			rectangle.Min = { minimum.x, minimum.y };
			rectangle.Size = { available.x, available.y };
			if (image.Texture != 0 && image.Width != 0 && image.Height != 0)
			{
				rectangle.ImageSize = { image.Width, image.Height };
				ImGui::Image(image.Texture, ImVec2(available.x, available.y));
				rectangle.Hovered = ImGui::IsItemHovered();
				if (!draggingAtStart)
					record(AcceptAssetDrop(context));
			}
			else
			{
				ImGui::Dummy(available);
				ImGui::GetWindowDrawList()->AddText(ImVec2(minimum.x + 12.0f, minimum.y + 12.0f), IM_COL32(220, 220, 220, 255),
					context.Editor.HasScene() || playing ? "Scene image unavailable" : "Open a scene to begin");
			}
			rectangle.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		}
		context.Viewports.SetRectangle(ViewportView::Scene, rectangle);
		// A changed selection must not redirect an already active gesture to a different set of entities.
		if (draggingAtStart && (m_DragInPlay != playing || !std::ranges::equal(m_DragSelection, context.Editor.GetSelection())))
		{
			record(DrawGizmoOverlay(context.Editor, context.Gizmos, image.Camera, {}, m_GizmoSettings));
			record(MakeError(ErrorCode::Conflict, "The selection changed during the gizmo drag"));
		}
		else
		{
			record(DrawGizmoOverlay(context.Editor, context.Gizmos, image.Camera, rectangle, m_GizmoSettings));
		}
		if (context.Gizmos.IsDragging() && !draggingAtStart)
		{
			m_DragSelection.assign(context.Editor.GetSelection().begin(), context.Editor.GetSelection().end());
			m_DragInPlay = playing;
		}
		if (!context.Gizmos.IsDragging())
			m_DragSelection.clear();
		const bool inputAvailable = rectangle.Focused && rectangle.Hovered && !io.WantTextInput && !ImGui::IsAnyItemActive()
			&& !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
			&& ImGui::GetDragDropPayload() == nullptr && !draggingAtStart && !context.Gizmos.IsDragging() && !ImGuizmo::IsUsingAny() && !ImGuizmo::IsOver();
		if (inputAvailable)
		{
			const bool fly = ImGui::IsMouseDown(ImGuiMouseButton_Right);
			const bool orbit = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
			const bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
			if (fly || orbit || pan || io.MouseWheel != 0.0f)
			{
				EditorCameraInput input;
				input.PointerDelta = { io.MouseDelta.x, io.MouseDelta.y };
				input.FlyAxis = {
					static_cast<float>(ImGui::IsKeyDown(ImGuiKey_D)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_A)),
					static_cast<float>(ImGui::IsKeyDown(ImGuiKey_E)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_Q)),
					static_cast<float>(ImGui::IsKeyDown(ImGuiKey_W)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_S))
				};
				input.Wheel = io.MouseWheel;
				input.DeltaSeconds = io.DeltaTime;
				input.MoveSpeed = io.KeyShift ? 20.0f : 5.0f;
				input.Fly = fly;
				input.Orbit = orbit && !fly;
				input.Pan = pan && !fly && !orbit;
				Result<ExplicitRenderCamera> navigated = EditorCamera::Navigate(state.GetCamera(), input);
				if (navigated)
					record(state.SetCamera(*navigated));
				else
					record(std::unexpected(std::move(navigated).error()));
			}
			else if (!io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				if (const auto pixel = ToViewportPixel(rectangle, { io.MousePos.x, io.MousePos.y }))
				{
					if (m_ClickSequence == std::numeric_limits<uint64_t>::max())
					{
						record(MakeError(ErrorCode::InvalidState, "Viewport click sequence is exhausted"));
					}
					else
					{
						record(context.Viewports.RequestPick({ .View = ViewportView::Scene, .Pixel = *pixel, .FrameIndex = image.FrameIndex, .SceneRevision = image.SceneRevision, .Sequence = ++m_ClickSequence, .ViewGeneration = image.Generation, .ExtendSelection = io.KeyCtrl || io.KeyShift }));
					}
				}
			}
			if (!fly && !orbit && !pan && ImGui::IsKeyPressed(ImGuiKey_F))
				record(FrameSelection(context));
		}
		if (!status)
			m_LastError = status.error().ToString();
		ImGui::End();
		return status;
	}

}
