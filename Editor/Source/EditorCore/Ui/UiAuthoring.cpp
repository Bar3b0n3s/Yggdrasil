#include "EditorPCH.h"
#include "EditorCore/Ui/UiAuthoring.h"

// UI authoring (UiAuthoring.h): stubs of the M22 contract (C22.2, Docs/Decisions/0033-m22-contract.md decisions 33 and 34)
// that stream D implements.

namespace Engine {

	UiRectComponent ApplyUiAnchorPreset(const UiRectComponent& rect, UiAnchorPreset /*preset*/, glm::vec2 /*parentSize*/, UiAnchorPresetOptions /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return rect;
	}

	std::optional<UiAnchorPreset> FindUiAnchorPreset(const UiRectComponent& /*rect*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::optional<UiRectHandle> FindUiRectHandle(glm::vec2 /*min*/, glm::vec2 /*max*/, glm::vec2 /*point*/, float /*grab*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	UiRectComponent DragUiRect(const UiRectComponent& start, glm::vec2 /*startSize*/, UiRectHandle /*handle*/, glm::vec2 /*delta*/)
	{
		ENGINE_CONTRACT_STUB();
		return start;
	}

	UUID PickUiElement(const UiLayoutTable& /*table*/, glm::vec2 /*point*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	glm::uvec2 GetUiPreviewFrameSize(glm::uvec2 /*gameResolution*/, const WindowSettings& /*window*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::uvec2(1);
	}

	UiPreviewFrame ComputeUiPreviewFrame(glm::uvec2 /*frameSize*/, glm::vec2 /*panelMin*/, glm::vec2 /*panelSize*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	glm::vec2 MapUiPreviewPointToScene(const UiPreviewFrame& /*frame*/, glm::vec2 panelPoint)
	{
		ENGINE_CONTRACT_STUB();
		return panelPoint;
	}

}
