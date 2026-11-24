#include "EnginePCH.h"
#include "Engine/Scene/StaticScenery.h"

#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/BoneAttachmentComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/StaticComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <format>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		// The component that moves `entity` every tick, and the field that makes it move, if any (the list of
		// StaticScenery.h, in its order).
		static std::optional<std::pair<std::string_view, std::string_view>> FindMovingComponent(ConstEntity entity)
		{
			if (const RigidBodyComponent* body = entity.TryGetComponent<RigidBodyComponent>(); body != nullptr && body->Type != BodyType::Static)
				return std::pair<std::string_view, std::string_view>{ "RigidBody", "Type" };
			if (entity.HasComponent<CharacterControllerComponent>())
				return std::pair<std::string_view, std::string_view>{ "CharacterController", "" };
			if (entity.HasComponent<BoneAttachmentComponent>())
				return std::pair<std::string_view, std::string_view>{ "BoneAttachment", "" };
			if (const AnimatorComponent* animator = entity.TryGetComponent<AnimatorComponent>();
				animator != nullptr && animator->RootMotion == AnimatorRootMotion::Apply)
				return std::pair<std::string_view, std::string_view>{ "Animator", "RootMotion" };
			return std::nullopt;
		}

		// The entity whose Static makes `entity` static: its own, or the nearest ancestor's with Scope Subtree.
		static ConstEntity FindStaticSource(ConstEntity entity)
		{
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
			{
				const StaticComponent* component = current.TryGetComponent<StaticComponent>();
				if (component != nullptr && (current == entity || component->Scope == StaticScope::Subtree))
					return current;
			}
			return entity;
		}

	}

	std::vector<StaticSceneryIssue> FindStaticSceneryIssues(const Scene& scene)
	{
		std::vector<StaticSceneryIssue> issues;
		std::vector<entt::entity> statics;
		scene.CollectCanonical<HierarchyStaticTag>(statics);
		for (const entt::entity handle : statics)
		{
			const ConstEntity entity(handle, &scene);
			const auto moving = Utils::FindMovingComponent(entity);
			if (!moving.has_value())
				continue;
			const ConstEntity source = Utils::FindStaticSource(entity);
			StaticSceneryIssue issue;
			issue.Code = std::string(SceneStaticMovesCode);
			issue.Entity = entity.GetUUID();
			issue.Component = std::string(moving->first);
			issue.Field = std::string(moving->second);
			issue.Message = std::format("entity '{}' is static (the Static of '{}') but its {} moves it every tick: its static cell rebuilds each "
										"tick it moves and it renders without interpolation",
				scene.GetEntityPath(entity), scene.GetEntityPath(source), moving->first);
			issue.Hint = "move it out of the static subtree, or give it (or the moving part's root) a Static with Scope NotStatic";
			issues.push_back(std::move(issue));
		}
		return issues;
	}

}
