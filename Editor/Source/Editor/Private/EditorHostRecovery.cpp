#include "EditorPCH.h"
#include "Editor/Private/EditorHostRecovery.h"

#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Assert.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		// Every open scene with its revision, in tab order: what a recovery offer is bound to (decision 15), since Accept
		// replaces any open scene the offer names, the active one or not.
		static std::vector<std::pair<OpenSceneId, uint64_t>> ListOpenSceneRevisions(const EditorContext& editor)
		{
			std::vector<std::pair<OpenSceneId, uint64_t>> revisions;
			for (const OpenSceneId id : editor.GetOpenScenes())
			{
				const Result<OpenSceneInfo> info = editor.DescribeOpenScene(id);
				revisions.emplace_back(id, info.has_value() ? info->Revision : 0);
			}
			return revisions;
		}

	}

	EditorHostRecovery::EditorHostRecovery(EditorContext& editor, Autosave& saves)
		: m_Editor(editor), m_Saves(saves)
	{
	}

	Status EditorHostRecovery::Inspect(uint64_t projectEpoch)
	{
		if (projectEpoch == 0)
			return MakeError(ErrorCode::InvalidArgument, "A recovery inspection needs a nonzero project epoch");
		if (!m_Editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "Open a project before inspecting recovery");
		if (projectEpoch == m_ProjectEpoch)
		{
			if (m_Project != &m_Editor.GetProject())
				return MakeError(ErrorCode::Conflict, "A replacement project needs a new recovery epoch");
			return {};
		}
		Reset();
		m_ProjectEpoch = projectEpoch;
		m_Project = &m_Editor.GetProject();
		// Opening with recover=true has already adopted the scenes through this long-lived autosave service.
		if (m_Editor.IsAnySceneDirty())
			return {};
		// Every scene with a recoverable generation (M20, Docs/Decisions/0031-m20-contract.md decision 15): the offer names
		// them all, oldest first, and holds the newest.
		ENGINE_TRY_ASSIGN(std::vector<AutosaveRecoveryInfo> recoveries, m_Saves.FindRecoveries());
		if (recoveries.empty())
			return {};
		ENGINE_VERIFY(m_NextOffer < std::numeric_limits<uint64_t>::max(), "recovery offer IDs exhausted");
		std::vector<std::string> names;
		names.reserve(recoveries.size());
		for (const AutosaveRecoveryInfo& recovery : recoveries)
			names.push_back(recovery.SceneName);
		m_Offer = EditorRecoveryOffer{ .Id = ++m_NextOffer, .Recovery = recoveries.back(), .SceneNames = std::move(names) };
		m_Recoveries = std::move(recoveries);
		m_OfferEpoch = m_ProjectEpoch;
		m_Revisions = Utils::ListOpenSceneRevisions(m_Editor);
		return {};
	}

	std::optional<EditorRecoveryOffer> EditorHostRecovery::GetOffer() const
	{
		return m_Offer;
	}

	Status EditorHostRecovery::CheckBinding() const
	{
		if (!m_Editor.HasProject() || m_Project != &m_Editor.GetProject() || m_ProjectEpoch == 0
			|| m_ProjectEpoch != m_OfferEpoch || m_Revisions != Utils::ListOpenSceneRevisions(m_Editor))
			return MakeError(ErrorCode::Conflict, "The project or an open scene changed after recovery was offered");
		// A test run leaves the binding intact but owns the edit scene until it finishes.
		if (m_Editor.GetPlay().IsTestRunActive())
			return MakeError(ErrorCode::InvalidState, "A test run owns the editor; recover after it finishes");
		return {};
	}

	Status EditorHostRecovery::QueueDecision(const EditorRecoveryOffer& offer, EditorRecoveryDecision decision)
	{
		if (offer.Id == 0 || (decision != EditorRecoveryDecision::Accept && decision != EditorRecoveryDecision::Decline))
			return MakeError(ErrorCode::InvalidArgument, "Invalid recovery decision");
		if (!m_Offer || m_Offer->Id != offer.Id)
			return MakeError(ErrorCode::NotFound, "The recovery offer was withdrawn");
		if (m_Decision)
			return MakeError(ErrorCode::InvalidState, "A recovery decision is already queued");
		if (decision == EditorRecoveryDecision::Accept)
		{
			Status binding = CheckBinding();
			if (!binding)
			{
				m_Offer->Failure = binding.error();
				return binding;
			}
			if (m_Editor.IsReadOnly())
			{
				Status refused = MakeError(ErrorCode::PermissionDenied, "Read-only projects cannot recover");
				m_Offer->Failure = refused.error();
				return refused;
			}
		}
		m_Decision = decision;
		m_Offer->DecisionPending = true;
		m_Offer->Failure.reset();
		return {};
	}

	Status EditorHostRecovery::Pump()
	{
		if (!m_Decision || !m_Offer)
			return {};
		const auto decision = *std::exchange(m_Decision, std::nullopt);
		m_Offer->DecisionPending = false;
		Status applied;
		if (decision == EditorRecoveryDecision::Accept)
		{
			// Each offered scene in its order, then the first of them active, as a recovering project.open leaves them. A
			// failure stops there: the scenes recovered before it stay recovered (each recovery is whole on its own).
			applied = CheckBinding();
			OpenSceneId first = NoOpenScene;
			for (const AutosaveRecoveryInfo& recovery : m_Recoveries)
			{
				if (!applied)
					break;
				applied = m_Saves.Recover(recovery);
				if (applied && first == NoOpenScene)
					first = m_Editor.GetActiveSceneId();
			}
			if (applied && first != NoOpenScene && m_Recoveries.size() > 1)
				applied = m_Editor.ActivateScene(first);
		}
		if (!applied)
		{
			m_Offer->Failure = applied.error();
			return applied;
		}
		m_Offer.reset();
		return {};
	}

	void EditorHostRecovery::Reset()
	{
		m_Offer.reset();
		m_Recoveries.clear();
		m_Decision.reset();
		m_Project = nullptr;
		m_ProjectEpoch = 0;
		m_OfferEpoch = 0;
		m_Revisions.clear();
	}

}
