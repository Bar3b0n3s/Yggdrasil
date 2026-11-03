#include "EnginePCH.h"
#include "Engine/Animation/RootMotion.h"

// Root motion (RootMotion.h). Contract stubs of M23's C23.3, implemented by stream B (Docs/Decisions/0034-m23-contract.md
// decision 26): until then no motion is taken out of a pose.

namespace Engine {

	RootMotionDelta ComputeRootMotion(const AnimationClipData& /*clip*/, const SkeletonData& /*skeleton*/, const AnimationTimeStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void HoldRootMotion(const AnimationClipData& /*clip*/, const SkeletonData& /*skeleton*/, AnimationPose& /*layerPose*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void BlendRootMotion(RootMotionDelta& /*result*/, const RootMotionDelta& /*layer*/, uint8_t /*keyed*/, float /*weight*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
