#include "EnginePCH.h"
#include "Engine/Scene/SpriteAnimationSystem.h"

// The M21 contract's stubs (Docs/Decisions/0032-m21-contract.md decision 5): stream B implements sprite animation.

namespace Engine {

	struct SpriteAnimationSystem::State
	{
		Scene* RuntimeScene = nullptr;
		AssetManager* Assets = nullptr;
	};

	uint32_t AdvanceSpriteAnimator(SpriteAnimatorComponent& /*animator*/, const SpriteClip& /*clip*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	SpriteAnimationSystem::SpriteAnimationSystem(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SpriteAnimationSystem::~SpriteAnimationSystem() = default;

	Result<Scope<SpriteAnimationSystem>> SpriteAnimationSystem::Create(const SpriteAnimationSystemSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "sprite animation (Docs/Decisions/0032-m21-contract.md decision 5, stream B)");
	}

	void SpriteAnimationSystem::Step(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::span<const SpriteSceneIssue> SpriteAnimationSystem::GetDiagnostics() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
