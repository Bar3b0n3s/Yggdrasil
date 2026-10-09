#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

namespace Engine {

	nvrhi::ITexture* SceneRenderer::GetEntityIdTexture() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	uint64_t SceneRenderer::GetViewGeneration() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Result<PickTicket> SceneRenderer::RequestPick(const PickRequest&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<std::optional<PickResult>> SceneRenderer::PollPick(PickTicket, uint64_t)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	void SceneRenderer::CancelPicks()
	{
		ENGINE_CONTRACT_STUB();
	}

	RenderStats SceneRenderer::GetRenderStats() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void SceneRenderer::OnSubmitted(uint64_t, uint64_t)
	{
		ENGINE_CONTRACT_STUB();
	}

}
