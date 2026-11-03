#include "EnginePCH.h"
#include "Engine/Animation/AnimationSampling.h"

// Joint transforms, quaternion interpolation, layer time and events (AnimationSampling.h). Contract stubs of M23's C23.3,
// implemented by stream B (Docs/Decisions/0034-m23-contract.md decision 26): until then nothing is sampled.

namespace Engine {

	glm::mat4 ComposeJointTransform(const JointTransform& /*transform*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::mat4(1.0f);
	}

	glm::quat SlerpQuaternion(const glm::quat& from, const glm::quat& /*to*/, float /*t*/)
	{
		ENGINE_CONTRACT_STUB();
		return from;
	}

	AnimationTimeStep AdvanceAnimationTime(float time, float /*delta*/, float /*duration*/, bool /*loop*/)
	{
		ENGINE_CONTRACT_STUB();
		return AnimationTimeStep{ .PreviousTime = time, .Time = time };
	}

	void CollectAnimationEvents(const AnimationClipData& /*clip*/, const AnimationTimeStep& /*step*/, std::vector<uint32_t>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
