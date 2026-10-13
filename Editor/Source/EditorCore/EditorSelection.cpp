#include "EditorPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

#include <algorithm>

namespace Engine {

	namespace Utils {

		// A Play selection addresses one runtime scene: the session it was made in, at that session's scene generation.
		static bool IsSelectionOfReplacedScene(SceneTarget target, const PlaySession* session, uint64_t serial, uint64_t generation)
		{
			return target == SceneTarget::Play && session != nullptr && (session->GetSerial() != serial || session->GetSceneGeneration() != generation);
		}

	}

	std::span<const UUID> EditorContext::GetSelection() const
	{
		if (Utils::IsSelectionOfReplacedScene(m_SelectionTarget, m_Play->GetSession(), m_SelectionSession, m_SelectionSceneGeneration))
			return {};
		return m_Selection;
	}

	Status EditorContext::SetSelection(std::vector<UUID> selection, SceneTarget target)
	{
		const Scene* scene = nullptr;
		const PlaySession* session = m_Play->GetSession();
		if (target == SceneTarget::Edit)
			scene = m_Scene.get();
		else if (target == SceneTarget::Play)
		{
			if (session != nullptr)
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
		m_SelectionSession = target == SceneTarget::Play ? session->GetSerial() : 0;
		m_SelectionSceneGeneration = target == SceneTarget::Play ? session->GetSceneGeneration() : 0;
		m_UiState.SetSelectedAsset({});
		return {};
	}

	void EditorContext::RetargetSelectionToPlay()
	{
		const PlaySession* session = m_Play->GetSession();
		if (session == nullptr)
			return;
		std::vector<UUID> kept;
		if (m_SelectionTarget == SceneTarget::Edit)
		{
			for (const UUID id : m_Selection)
			{
				if (session->GetScene().FindEntityByID(id).IsValid())
					kept.push_back(id);
			}
		}
		m_Selection = std::move(kept);
		m_SelectionTarget = SceneTarget::Play;
		m_SelectionSession = session->GetSerial();
		m_SelectionSceneGeneration = session->GetSceneGeneration();
	}

	void EditorContext::RestoreSelection(std::vector<UUID> selection, SceneTarget target)
	{
		const PlaySession* session = m_Play->GetSession();
		const Scene* scene = target == SceneTarget::Play ? (session != nullptr ? &session->GetScene() : nullptr) : m_Scene.get();
		std::vector<UUID> kept;
		if (scene != nullptr)
		{
			for (const UUID id : selection)
			{
				if (scene->FindEntityByID(id).IsValid() && std::find(kept.begin(), kept.end(), id) == kept.end())
					kept.push_back(id);
			}
		}
		const bool play = scene != nullptr && target == SceneTarget::Play;
		if (!kept.empty())
			m_UiState.SetSelectedAsset({});
		m_Selection = std::move(kept);
		m_SelectionTarget = play ? SceneTarget::Play : SceneTarget::Edit;
		m_SelectionSession = play ? session->GetSerial() : 0;
		m_SelectionSceneGeneration = play ? session->GetSceneGeneration() : 0;
	}

	void EditorContext::PruneSelection()
	{
		if (m_Play->IsTestRunActive() || m_Selection.empty())
			return;
		const PlaySession* session = m_Play->GetSession();
		if (Utils::IsSelectionOfReplacedScene(m_SelectionTarget, session, m_SelectionSession, m_SelectionSceneGeneration))
		{
			m_Selection.clear();
			return;
		}
		// A Play selection without a session only exists inside Stop, which re-addresses it to the edit scene itself.
		const Scene* scene = m_SelectionTarget == SceneTarget::Play ? (session != nullptr ? &session->GetScene() : nullptr) : m_Scene.get();
		if (scene == nullptr)
		{
			if (m_SelectionTarget == SceneTarget::Edit)
				m_Selection.clear();
			return;
		}
		std::erase_if(m_Selection, [scene](UUID id)
		{
			return !scene->FindEntityByID(id).IsValid();
		});
	}

}
