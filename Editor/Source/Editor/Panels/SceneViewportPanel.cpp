#include "EditorPCH.h"
#include "Editor/Panels/SceneViewportPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
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
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace Engine {

	int SceneViewportPanel::GetWindowFlags()
	{
		return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	}

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
		// The scene the scene view shows (M20 decisions 13 and 23), which viewport.frame frames.
		const SceneTarget target = context.Editor.ShowsPlayScene() ? SceneTarget::Play : SceneTarget::Edit;
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
		// The shown scene, as the hierarchy shows it: the play scene only while the active tab is the played one (decision 13).
		PlaySession* session = context.Editor.ShowsPlayScene() ? context.Editor.GetPlay().GetSession() : nullptr;
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
		// Drawn into the caller's window (EditorLayer's Begin with GetWindowFlags). Only its title bar or tab moves it: a
		// press on the image, a gizmo handle included, belongs to the view and never drags a floating window along.
		ImGui::GetCurrentWindow()->BgClickFlags = ImGuiWindowBgClickFlags_None;
		const ImGuiIO& io = ImGui::GetIO();
		EditorViewportOptions options = state.GetOptions();
		const float scale = Utils::EditorUiScale();
		const float toolbarRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		const auto nextTool = [toolbarRight](float width)
		{
			if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= toolbarRight)
				ImGui::SameLine();
		};
		const auto buttonWidth = [](const char* label)
		{
			return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		};
		const bool draggingAtStart = context.Gizmos.IsDragging();
		const bool playing = context.Editor.GetPlay().IsPlaying();
		ImGui::BeginDisabled(draggingAtStart || context.Editor.IsReadOnly() || !options.Gizmos);
		if (Utils::EditorToolbarButton("Move", "Move selection (W)", m_GizmoSettings.Operation == GizmoOperation::Translate))
			m_GizmoSettings.Operation = GizmoOperation::Translate;
		nextTool(buttonWidth("Rotate"));
		if (Utils::EditorToolbarButton("Rotate", "Rotate selection (E)", m_GizmoSettings.Operation == GizmoOperation::Rotate))
			m_GizmoSettings.Operation = GizmoOperation::Rotate;
		nextTool(buttonWidth("Scale"));
		if (Utils::EditorToolbarButton("Scale", "Scale selection (R)", m_GizmoSettings.Operation == GizmoOperation::Scale))
			m_GizmoSettings.Operation = GizmoOperation::Scale;
		nextTool(buttonWidth("World"));
		const bool local = m_GizmoSettings.Space == GizmoSpace::Local;
		if (Utils::EditorToolbarButton(local ? "Local###GizmoSpace" : "World###GizmoSpace", "Switch coordinate space (X)", local))
			m_GizmoSettings.Space = local ? GizmoSpace::World : GizmoSpace::Local;
		ImGui::EndDisabled();
		nextTool(buttonWidth("Snap"));
		if (Utils::EditorToolbarButton("Snap", "Snap to 0.5 m, 15 degrees or 0.1 scale. Hold Ctrl for temporary snapping.", m_GizmoSettings.Snap))
			m_GizmoSettings.Snap = !m_GizmoSettings.Snap;
		nextTool(112.0f * scale);
		ImGui::SetNextItemWidth(112.0f * scale);
		const std::string debugLabel = Utils::EditorLabel(RenderDebugViewToString(state.GetDebugView()));
		if (ImGui::BeginCombo("##SceneDebugView", debugLabel.c_str()))
		{
			for (uint32_t index = 0; index < RenderDebugViewCount; ++index)
			{
				const auto view = static_cast<RenderDebugView>(index);
				const std::string label = Utils::EditorLabel(RenderDebugViewToString(view));
				if (ImGui::Selectable(label.c_str(), view == state.GetDebugView()))
					record(state.SetDebugView(view));
			}
			ImGui::EndCombo();
		}
		nextTool(buttonWidth("Overlays"));
		if (Utils::EditorToolbarButton("Overlays", "Grid, gizmos, collision shapes and projection"))
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
		nextTool(buttonWidth("Frame"));
		ImGui::BeginDisabled(draggingAtStart || context.Editor.GetSelection().empty());
		if (Utils::EditorToolbarButton("Frame", "Frame the selected objects (F)"))
			record(FrameSelection(context));
		ImGui::EndDisabled();
		ImGui::Separator();
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
		const ImVec2 maximum(minimum.x + available.x, minimum.y + available.y);
		// Without a scene the view shows its placeholder: an empty image has no picking image to click.
		const bool hasScene = context.Editor.HasScene() || playing;
		const bool hasRegion = std::isfinite(available.x) && std::isfinite(available.y) && available.x > 0.0f && available.y > 0.0f;
		const bool imageShown = hasRegion && hasScene && image.Texture != 0 && image.Width != 0 && image.Height != 0;
		EditorViewportRect rectangle;
		if (hasRegion)
		{
			rectangle.Min = { minimum.x, minimum.y };
			rectangle.Size = { available.x, available.y };
			rectangle.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		}
		if (imageShown)
		{
			rectangle.ImageSize = { image.Width, image.Height };
			// Drawn before the gizmo, which draws over it; the image's input item follows the gizmo (below).
			ImGui::GetWindowDrawList()->AddImage(image.Texture, minimum, maximum);
			rectangle.Hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(minimum, maximum);
		}
		// A changed selection must not redirect an already active gesture to a different set of entities.
		bool gizmoOwnsPointer = false;
		if (draggingAtStart && (m_DragInPlay != playing || !std::ranges::equal(m_DragSelection, context.Editor.GetSelection())))
		{
			record(DrawGizmoOverlay(context.Editor, context.Gizmos, image.Camera, {}, m_GizmoSettings, gizmoOwnsPointer));
			record(MakeError(ErrorCode::Conflict, "The selection changed during the gizmo drag"));
		}
		else
		{
			record(DrawGizmoOverlay(context.Editor, context.Gizmos, image.Camera, rectangle, m_GizmoSettings, gizmoOwnsPointer));
		}
		if (context.Gizmos.IsDragging() && !draggingAtStart)
		{
			m_DragSelection.assign(context.Editor.GetSelection().begin(), context.Editor.GetSelection().end());
			m_DragInPlay = playing;
		}
		if (!context.Gizmos.IsDragging())
			m_DragSelection.clear();
		// The image is one item that owns every press on it (left, right and middle), so a held button keeps the view's
		// navigation going and never becomes a window move. ImGuizmo activates only while no item is active and none was
		// hovered in this frame or the previous one, so over a gizmo handle the view leaves the press to ImGuizmo with an
		// ID-less item instead. The image item also exists only in a frame with a press or while it holds one: hovered in
		// the frame before the pointer reaches a handle, it would keep ImGuizmo from taking a press made in the first frame
		// over that handle. Between presses an ID-less item stands in and the view claims the wheel itself.
		bool imageActive = false;
		bool leftPressed = false;
		if (imageShown)
		{
			const ImGuiID imageId = ImGui::GetID("##SceneImage");
			const bool pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)
				|| ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
			if (gizmoOwnsPointer || (!pressed && ImGui::GetActiveID() != imageId))
			{
				ImGui::Dummy(available);
				// The wheel zooms the camera; it must never also scroll the window.
				if (!gizmoOwnsPointer && rectangle.Hovered && ImGui::TestKeyOwner(ImGuiKey_MouseWheelY, imageId))
					ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, imageId);
			}
			else
			{
				ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
				ImGui::InvisibleButton("##SceneImage", available, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
				ImGui::PopItemFlag();
				imageActive = ImGui::IsItemActive();
				leftPressed = ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
				// The wheel zooms the camera; it must never also scroll the window.
				ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
			}
			// A press on the image focuses the view within this frame, so the first click both focuses and picks.
			rectangle.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			if (!draggingAtStart)
				record(AcceptAssetDrop(context));
		}
		else if (hasRegion)
		{
			ImGui::Dummy(available);
			ImGui::GetWindowDrawList()->AddText(ImVec2(minimum.x + 12.0f, minimum.y + 12.0f), IM_COL32(220, 220, 220, 255),
				hasScene ? "Scene image unavailable" : "Open a scene to begin");
		}
		context.Viewports.SetRectangle(ViewportView::Scene, rectangle);
		const bool otherItemActive = ImGui::IsAnyItemActive() && !imageActive;
		// ImGuizmo::IsOver also reports the previous frame's hover: a press that leaves a handle in that frame still
		// belongs to the gizmo, never to a background pick.
		const bool inputCaptured = io.WantTextInput || otherItemActive
			|| ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
			|| ImGui::GetDragDropPayload() != nullptr || draggingAtStart || context.Gizmos.IsDragging() || ImGuizmo::IsUsingAny()
			|| ImGuizmo::IsOver() || gizmoOwnsPointer;
		if (imageShown && !inputCaptured)
		{
			// Buttons navigate only while they hold the image item they pressed; the wheel zooms the hovered view.
			const bool fly = imageActive && ImGui::IsMouseDown(ImGuiMouseButton_Right);
			const bool orbit = imageActive && io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
			const bool pan = imageActive && ImGui::IsMouseDown(ImGuiMouseButton_Middle);
			const float wheel = rectangle.Hovered ? io.MouseWheel : 0.0f;
			if (fly || orbit || pan || wheel != 0.0f)
			{
				EditorCameraInput input;
				input.PointerDelta = { io.MouseDelta.x, io.MouseDelta.y };
				input.FlyAxis = {
					static_cast<float>(ImGui::IsKeyDown(ImGuiKey_D)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_A)),
					static_cast<float>(ImGui::IsKeyDown(ImGuiKey_E)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_Q)),
					static_cast<float>(ImGui::IsKeyDown(ImGuiKey_W)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_S))
				};
				input.Wheel = wheel;
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
			else if (leftPressed && !io.KeyAlt && image.Picking)
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
			if (!fly && !orbit && !pan && rectangle.Focused && rectangle.Hovered && ImGui::IsKeyPressed(ImGuiKey_F))
				record(FrameSelection(context));
		}
		if (!status)
			m_LastError = status.error().ToString();
		return status;
	}

}
