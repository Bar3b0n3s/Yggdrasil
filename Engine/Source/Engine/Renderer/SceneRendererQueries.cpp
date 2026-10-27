#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Renderer/Private/SceneRendererState.h"

#include <format>
#include <limits>
#include <optional>

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
		// M18 (decision 6): the request names a view pixel of the submitted image; EntityId is at the scene resolution of its
		// output mapping (the Output stage of that Render set OutputMapping).
		const RenderOutputMapping& mapping = state.OutputMapping;
		if (mapping.IsIdentity())
			return state.Picker->Request(*state.Targets.EntityId, state.ImagePickTable, state.Generation, request);
		if (request.X >= mapping.ViewWidth || request.Y >= mapping.ViewHeight)
		{
			ErrorLocation location{};
			location.JsonPointer = request.X >= mapping.ViewWidth ? "/x" : "/y";
			return std::unexpected(Error(ErrorCode::InvalidArgument,
				std::format("Picking pixel ({}, {}) is outside {} x {}", request.X, request.Y, mapping.ViewWidth, mapping.ViewHeight))
					.WithLocation(std::move(location)));
		}
		PickRequest scene = request;
		if (const std::optional<glm::uvec2> pixel = MapViewPixelToScenePixel(mapping, request.X, request.Y))
		{
			scene.X = pixel->x;
			scene.Y = pixel->y;
			return state.Picker->Request(*state.Targets.EntityId, state.ImagePickTable, state.Generation, scene);
		}
		// A letterbox pixel shows no scene: it picks the background, through the texel every picking render clears to 0. A
		// render whose output targets could not be created (out of memory, decision 44) keeps an earlier render's texel; the
		// first such render at a scaled output has none, which is an external failure, not a programmer error (M18's
		// milestone review): the pick fails alone.
		if (state.OutputTargets.LetterboxEntityId == nullptr)
			return MakeError(ErrorCode::InvalidState, "the letterbox of this view has no picking texel: its output targets could not be created; pick again after the next render");
		scene.X = 0;
		scene.Y = 0;
		return state.Picker->Request(*state.OutputTargets.LetterboxEntityId, state.ImagePickTable, state.Generation, scene);
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

	RenderOutputMapping SceneRenderer::GetOutputMapping() const
	{
		// The last Render's mapping (its Output stage set it), while it was made for the renderer's current size; the
		// identity at that size before the first Render and after a Resize to another size.
		const RenderOutputMapping& mapping = m_State->OutputMapping;
		if (mapping.ViewWidth == GetWidth() && mapping.ViewHeight == GetHeight())
			return mapping;
		return ComputeRenderOutputMapping(GetWidth(), GetHeight(), {});
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
