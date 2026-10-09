#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Renderer/Private/SceneRendererState.h"

#include <limits>

namespace Engine {

	nvrhi::ITexture* SceneRenderer::GetEntityIdTexture() const
	{
		return m_State->RecordedPicking ? m_State->Targets.EntityId.Get() : nullptr;
	}

	uint64_t SceneRenderer::GetViewGeneration() const
	{
		return m_State->Generation;
	}

	Result<PickTicket> SceneRenderer::RequestPick(const PickRequest& request)
	{
		State& state = *m_State;
		if (state.PendingSubmission || !state.SubmittedPicking)
			return MakeError(ErrorCode::InvalidState, "picking requires a submitted picking image");
		if (request.FrameIndex != state.ImageFrame || request.SceneRevision != state.ImageRevision
			|| request.ViewGeneration != state.ImageGeneration || state.ImageGeneration != state.Generation)
			return MakeError(ErrorCode::Conflict, "the clicked image no longer matches this view");
		return state.Picker->Request(*state.Targets.EntityId, state.ImagePickTable, state.Generation, request);
	}

	Result<std::optional<PickResult>> SceneRenderer::PollPick(PickTicket ticket, uint64_t currentFrameIndex)
	{
		return m_State->Picker->Poll(ticket, currentFrameIndex, m_State->Generation);
	}

	void SceneRenderer::CancelPicks()
	{
		State& state = *m_State;
		ENGINE_CORE_VERIFY(state.Generation < std::numeric_limits<uint64_t>::max(), "view generation exhausted");
		++state.Generation;
		state.Picker->CancelAll();
		state.SubmittedPicking = false;
	}

	RenderStats SceneRenderer::GetRenderStats() const
	{
		return m_State->History.GetLatest();
	}

	void SceneRenderer::OnSubmitted(uint64_t frameIndex, uint64_t submissionId)
	{
		State& state = *m_State;
		if (!state.PendingSubmission)
			return;
		ENGINE_CORE_ASSERT(frameIndex == state.ImageFrame, "OnSubmitted must identify the recorded snapshot");
		ENGINE_CORE_ASSERT(submissionId != 0 && submissionId <= state.Device->GetLastSubmissionID(), "OnSubmitted needs a submission of this device");
		if (frameIndex != state.ImageFrame || submissionId == 0 || submissionId > state.Device->GetLastSubmissionID())
			return;
		state.TargetPool->EndFrame();
		if (state.PendingTimingSlot)
			state.TimingSubmissions[*state.PendingTimingSlot] = submissionId;
		state.PendingTimingSlot.reset();
		state.PendingSubmission = false;
		state.SubmittedPicking = state.RecordedPicking && state.ImageGeneration == state.Generation;
	}

}
