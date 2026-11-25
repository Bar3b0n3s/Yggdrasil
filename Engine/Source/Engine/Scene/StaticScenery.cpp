#include "EnginePCH.h"
#include "Engine/Scene/StaticScenery.h"

#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/BoneAttachmentComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/StaticComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <format>
#include <optional>
#include <unordered_map>
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

#if defined(ENGINE_DEBUG)
	void VerifyHierarchyStaticTags(const Scene& scene)
	{
		const entt::registry& registry = scene.GetRegistry();
		const auto* statics = registry.storage<StaticComponent>();
		const auto* tags = registry.storage<HierarchyStaticTag>();
		if ((statics == nullptr || statics->empty()) && (tags == nullptr || tags->empty()))
			return;

		// What each entity passes down to its children: Subtree passes "static", NotStatic "not static", and EntityOnly (or no
		// Static) what its parent passed. Parents precede their children in canonical order; the map is lookup only.
		std::unordered_map<entt::entity, bool> passed;
		passed.reserve(scene.GetEntityCount());
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			const entt::entity handle = entity.GetHandle();
			bool inherited = false;
			if (const UUID parent = registry.get<RelationshipComponent>(handle).Parent; parent.IsValid())
			{
				const auto found = passed.find(scene.FindEntityByID(parent).GetHandle());
				ENGINE_CORE_VERIFY(found != passed.end(), "The parent of entity {} follows it in canonical order", id);
				inherited = found->second;
			}
			bool isStatic = inherited;
			bool passes = inherited;
			if (const StaticComponent* component = registry.try_get<StaticComponent>(handle))
			{
				isStatic = component->Scope != StaticScope::NotStatic;
				if (component->Scope != StaticScope::EntityOnly)
					passes = component->Scope == StaticScope::Subtree;
			}
			passed.emplace(handle, passes);
			const bool tagged = registry.all_of<HierarchyStaticTag>(handle);
			ENGINE_CORE_VERIFY(tagged == isStatic,
				"Stale static tag of entity '{}' ({}) in scene '{}': the Static rule makes it {} but it {} HierarchyStaticTag: a "
				"Static was written past the scene's change hooks (add, write or remove it through Entity's component templates, "
				"ComponentAccess or a component host operation), or the scene missed a change",
				scene.GetEntityPath(entity), id, scene.GetName(), isStatic ? "static" : "not static", tagged ? "holds" : "lacks");
		}
	}
#endif

}
