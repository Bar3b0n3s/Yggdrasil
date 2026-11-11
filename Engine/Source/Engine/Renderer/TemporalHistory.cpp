#include "EnginePCH.h"
#include "Engine/Renderer/TemporalHistory.h"

// The view history of the temporal-state section (TemporalHistory.h). Reset is the contract's; the jitter, the warm-up rule
// and Apply are contract stubs of M26's C26.1, implemented by stream X-C (Docs/Decisions/0037-m26-contract.md decisions 10
// and 11). No host keeps a history until then.

namespace Engine {

	glm::vec2 GetTaaJitter(uint32_t /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	bool NeedsTemporalWarmup(const RenderSnapshot& /*snapshot*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void RenderViewHistory::Apply(RenderSnapshot& /*snapshot*/, float /*deltaTime*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RenderViewHistory::Reset()
	{
		m_ResetPending = true;
	}

}
