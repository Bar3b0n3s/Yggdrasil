#include "EnginePCH.h"
#include "Engine/Scene/AnimationSystem.h"

#include "Engine/Scene/Entity.h"

#include <vector>

// The AnimationSystem phase and the pose queries (AnimationSystem.h). The class's construction, its listener and its
// diagnostics are the contract's; Create, Step, the pose writer and the queries are contract stubs of M23's C23.3,
// implemented by stream B (Docs/Decisions/0034-m23-contract.md decision 29): until then no session creates the system and
// nothing is posed.

namespace Engine {

	struct AnimationSystem::State
	{
		Scene* RuntimeScene = nullptr;               // documented back-reference
		AssetManager* Assets = nullptr;              // documented back-reference
		IAnimationEventListener* Listener = nullptr; // documented back-reference, or null
		std::vector<AnimationSceneIssue> Diagnostics{};
	};

	AnimationSystem::AnimationSystem(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	AnimationSystem::~AnimationSystem() = default;

	Result<Scope<AnimationSystem>> AnimationSystem::Create(const AnimationSystemSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 29, stream B)");
	}

	void AnimationSystem::Step(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AnimationSystem::SetEventListener(IAnimationEventListener* listener)
	{
		m_State->Listener = listener;
	}

	std::span<const AnimationSceneIssue> AnimationSystem::GetDiagnostics() const
	{
		return m_State->Diagnostics;
	}

	Status PoseAnimator(Entity /*animator*/, AssetManager& /*assets*/, float /*previewTime*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 29, stream B)");
	}

	ConstEntity FindDrivingAnimator(ConstEntity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<glm::mat4> FindAnimatorJointWorldTransform(ConstEntity /*animator*/, std::string_view /*joint*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}
