#include "EditorPCH.h"
#include "Editor/Ui/UiPreview.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Viewport/EditorViewportState.h"
#include "Engine/Scene/RenderExtraction.h"

#include <imgui.h>

namespace Engine {

	bool IsUiPreviewShown(const EditorContext& editor)
	{
		return editor.GetViewportState().GetOptions().UiPreview && editor.HasProject();
	}

	void DrawUiPreviewOption(EditorViewportOptions& options)
	{
		ImGui::Checkbox("UI preview", &options.UiPreview);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Show the scene as the game frames it, through its primary camera with its UI, to select and resize UI elements");
	}

	std::optional<UiPreviewFrame> FitUiPreviewImage(const EditorContext& editor, EditorViewportRect& rectangle)
	{
		if (!IsUiPreviewShown(editor))
			return std::nullopt;
		const UiPreviewFrame frame = ComputeUiPreviewFrame(rectangle.ImageSize, rectangle.Min, rectangle.Size);
		rectangle.Min = frame.Min;
		rectangle.Size = frame.Size;
		return frame;
	}

	std::optional<glm::uvec2> GetUiPreviewRenderSize(const EditorContext& editor)
	{
		if (!IsUiPreviewShown(editor))
			return std::nullopt;
		const glm::uvec2 frame = GetUiPreviewFrameSize(editor.GetViewportState().GetGameResolution(), editor.GetProject().GetSettings().Window);
		return glm::clamp(frame, glm::uvec2(1), glm::uvec2(8192));
	}

	bool ConfigureUiPreviewRequest(const EditorContext& editor, RenderExtractionRequest& request)
	{
		if (!IsUiPreviewShown(editor))
			return false;
		request.Camera = RenderCameraSource::Primary;
		request.Flags = RenderViewFlags::None;
		request.SelectedEntities.clear();
		return true;
	}

}
