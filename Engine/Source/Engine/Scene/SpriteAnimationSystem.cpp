#include "EnginePCH.h"
#include "Engine/Scene/SpriteAnimationSystem.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/SpriteClipLookup.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	struct SpriteAnimationSystem::State
	{
		Scene* RuntimeScene = nullptr;
		AssetManager* Assets = nullptr;
		// The SPRITE_UNKNOWN_CLIP findings raised, in the order raised, and the (entity, clip name) pairs they name, so each is
		// raised and logged once.
		std::vector<SpriteSceneIssue> Diagnostics{};
		std::set<std::pair<UUID, std::string>, std::less<>> RaisedClips{};
		// The entities the current step animates, in canonical order (kept to reuse its capacity).
		std::vector<Entity> Animated{};

		void RaiseUnknownClip(Entity entity, const std::string& clip, const Detail::SpriteClipLookup& lookup);
	};

	void SpriteAnimationSystem::State::RaiseUnknownClip(Entity entity, const std::string& clip, const Detail::SpriteClipLookup& lookup)
	{
		if (Diagnostics.size() >= MaxSpriteAnimationDiagnostics)
			return;
		if (!RaisedClips.emplace(entity.GetUUID(), clip).second)
			return;
		if (Diagnostics.size() + 1 == MaxSpriteAnimationDiagnostics)
		{
			ENGINE_CORE_WARN("{}: {} findings reached; later unknown clips of this session's scene are neither kept nor logged", SpriteUnknownClipCode,
				MaxSpriteAnimationDiagnostics);
		}
		SpriteSceneIssue issue = Detail::MakeUnknownClipIssue(entity.GetUUID(), RuntimeScene->GetEntityPath(entity), clip, lookup);
		ENGINE_CORE_WARN("{}: {}", issue.Code, issue.Message);
		Diagnostics.push_back(std::move(issue));
	}

	uint32_t AdvanceSpriteAnimator(SpriteAnimatorComponent& animator, const SpriteClip& clip)
	{
		ENGINE_CORE_ASSERT(!clip.Frames.empty(), "AdvanceSpriteAnimator: the clip '{}' has no steps", clip.Name);
		if (clip.Frames.empty())
			return 0;
		const uint32_t lastStep = static_cast<uint32_t>(clip.Frames.size() - 1);
		const uint32_t step = std::min(animator.Step, lastStep);
		const SpriteClipFrame& shown = clip.Frames[step];
		// A step shows for at least one tick (a sheet's Ticks are 1 or more; ValidateSpriteSheet); a StepTicks at or past the
		// step's Ticks, left by another clip, ends the step after this tick.
		const uint32_t ticks = std::max(shown.Ticks, 1u);
		animator.Step = step;
		if (animator.StepTicks < ticks - 1)
		{
			++animator.StepTicks;
			return shown.Frame;
		}
		animator.StepTicks = 0;
		if (step < lastStep)
			animator.Step = step + 1;
		else if (clip.Loop)
			animator.Step = 0;
		else
			animator.Playing = false;
		return shown.Frame;
	}

	SpriteAnimationSystem::SpriteAnimationSystem(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SpriteAnimationSystem::~SpriteAnimationSystem() = default;

	Result<Scope<SpriteAnimationSystem>> SpriteAnimationSystem::Create(const SpriteAnimationSystemSpecification& specification)
	{
		if (specification.RuntimeScene == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "SpriteAnimationSystem needs the play session's runtime scene");
		if (specification.Assets == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "SpriteAnimationSystem needs an asset manager to load sprite sheets");
		Scope<SpriteAnimationSystem> system = CreateScope<SpriteAnimationSystem>(ConstructionKey());
		system->m_State->RuntimeScene = specification.RuntimeScene;
		system->m_State->Assets = specification.Assets;
		return system;
	}

	void SpriteAnimationSystem::Step(const SimStep& /*step*/)
	{
		State& state = *m_State;
		Scene& scene = *state.RuntimeScene;
		// A scene without animators (every 3D game's) costs one check.
		if (scene.GetRegistry().view<SpriteAnimatorComponent>().empty())
			return;

		// The playing animators of effectively enabled entities, gathered first so the writes below never run while the
		// canonical order is being read.
		state.Animated.clear();
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const Entity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || entity.HasComponent<HierarchyDisabledTag>() || !entity.HasComponent<SpriteRendererComponent>())
				continue;
			const SpriteAnimatorComponent* animator = entity.TryGetComponent<SpriteAnimatorComponent>();
			if (animator != nullptr && animator->Playing && !animator->Clip.empty())
				state.Animated.push_back(entity);
		}

		for (const Entity entity : state.Animated)
		{
			const std::string clip = entity.GetComponent<SpriteAnimatorComponent>().Clip;
			const AssetHandle sheet = entity.GetComponent<SpriteRendererComponent>().Sheet.GetHandle();
			const Detail::SpriteClipLookup lookup = Detail::FindSpriteClip(*state.Assets, sheet, clip);
			if (lookup.Status != Detail::SpriteClipStatus::Found)
			{
				state.RaiseUnknownClip(entity, clip, lookup);
				continue;
			}
			// ValidateSpriteSheet gives every clip of a loaded sheet a step; one built in code without any plays nothing.
			if (lookup.Clip->Frames.empty())
				continue;
			// Through the scene (Entity::Patch), so the change tracker and the revision see every write.
			uint32_t frame = 0;
			entity.Patch<SpriteAnimatorComponent>([&frame, &lookup](SpriteAnimatorComponent& animator)
			{
				frame = AdvanceSpriteAnimator(animator, *lookup.Clip);
			});
			if (entity.GetComponent<SpriteRendererComponent>().Frame != frame)
			{
				entity.Patch<SpriteRendererComponent>([frame](SpriteRendererComponent& renderer)
				{
					renderer.Frame = frame;
				});
			}
		}
	}

	std::span<const SpriteSceneIssue> SpriteAnimationSystem::GetDiagnostics() const
	{
		return m_State->Diagnostics;
	}

}
