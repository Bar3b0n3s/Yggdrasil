#include "EditorPCH.h"
#include "Editor/EditorLayer.h"

namespace Engine {

	EditorLayer::EditorLayer(EditorPanelContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	EditorLayer::~EditorLayer()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status EditorLayer::OnSafePoint(double /*nowSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status EditorLayer::OnImGuiRender()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void EditorLayer::RequestFrame()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool EditorLayer::IsFrameRequested() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
