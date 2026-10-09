#include "EditorPCH.h"
#include "Editor/Private/EditorHostRecovery.h"

#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"

#include <limits>
#include <utility>

namespace Engine {

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
		// Opening with recover=true has already adopted the scene through this long-lived autosave service.
		if (m_Editor.IsSceneDirty())
			return {};
		ENGINE_TRY_ASSIGN(auto recovery, m_Saves.FindRecovery());
		if (!recovery)
			return {};
		ENGINE_VERIFY(m_NextOffer < std::numeric_limits<uint64_t>::max(), "recovery offer IDs exhausted");
		m_Offer = EditorRecoveryOffer{ .Id = ++m_NextOffer, .Recovery = std::move(*recovery) };
		m_OfferEpoch = m_ProjectEpoch;
		m_Revision = m_Editor.GetRevision();
		return {};
	}

	std::optional<EditorRecoveryOffer> EditorHostRecovery::GetOffer() const
	{
		return m_Offer;
	}

	Status EditorHostRecovery::CheckBinding() const
	{
		if (!m_Editor.HasProject() || m_Project != &m_Editor.GetProject() || m_ProjectEpoch == 0
			|| m_ProjectEpoch != m_OfferEpoch || m_Revision != m_Editor.GetRevision())
			return MakeError(ErrorCode::Conflict, "The project or scene changed after recovery was offered");
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
			applied = CheckBinding();
			if (applied)
				applied = m_Saves.Recover(m_Offer->Recovery);
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
		m_Decision.reset();
		m_Project = nullptr;
		m_ProjectEpoch = 0;
		m_OfferEpoch = 0;
		m_Revision = 0;
	}

}
