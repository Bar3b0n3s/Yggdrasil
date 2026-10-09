#include "EditorPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

#include <algorithm>

namespace Engine {

	Status EditorContext::SetSelection(std::vector<UUID> selection, SceneTarget target)
	{
		const Scene* scene = nullptr;
		if (target == SceneTarget::Edit)
			scene = m_Scene.get();
		else if (target == SceneTarget::Play)
		{
			if (const PlaySession* session = m_Play->GetSession())
				scene = &session->GetScene();
		}
		else
			return MakeError(ErrorCode::InvalidArgument, "unknown selection target");
		if (scene == nullptr)
			return MakeError(ErrorCode::InvalidState, "selection target has no scene");
		std::vector<UUID> unique;
		for (const UUID id : selection)
		{
			if (!scene->FindEntityByID(id).IsValid())
				return MakeError(ErrorCode::NotFound, "selected entity {} is absent from the target scene", id.ToString());
			if (std::find(unique.begin(), unique.end(), id) == unique.end())
				unique.push_back(id);
		}
		m_Selection = std::move(unique);
		m_SelectionTarget = target;
		m_UiState.SetSelectedAsset({});
		return {};
	}

}
