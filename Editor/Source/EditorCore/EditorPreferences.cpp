#include "EditorPCH.h"
#include "EditorCore/EditorPreferences.h"

namespace Engine {

	Result<EditorPreferences> ReadEditorPreferences(const VirtualFileSystem& /*vfs*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status WriteEditorPreferences(VirtualFileSystem& /*vfs*/, const EditorPreferences& /*preferences*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
