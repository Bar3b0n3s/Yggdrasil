#include "EditorPCH.h"
#include "Editor/Icons.h"

namespace Engine {

	void DrawEditorIcon(ImDrawList& /*drawList*/, EditorIcon /*icon*/, const glm::vec2& /*minimum*/, float /*size*/, uint32_t /*color*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AppendEditorIcon(DebugDrawList& /*drawList*/, EditorIcon /*icon*/, const glm::mat4& /*world*/, float /*size*/, const glm::vec4& /*color*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	EditorIcon AssetTypeToEditorIcon(AssetType /*type*/)
	{
		ENGINE_CONTRACT_STUB();
		return EditorIcon::Folder;
	}

}
