#include "EnginePCH.h"
#include "Engine/Scene/Entity.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/NameComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TagsComponent.h"

#include <algorithm>

namespace Engine {

	namespace Utils {

		static bool IsLiveEntity(const entt::registry& registry, entt::entity handle)
		{
			return registry.valid(handle) && !registry.all_of<PendingDestroyTag>(handle);
		}

	}

	bool ConstEntity::IsValid() const
	{
		return m_Scene != nullptr && Utils::IsLiveEntity(m_Scene->GetRegistry(), m_Handle);
	}

	UUID ConstEntity::GetUUID() const
	{
		return GetComponent<IDComponent>().ID;
	}

	const std::string& ConstEntity::GetName() const
	{
		return GetComponent<NameComponent>().Name;
	}

	ConstEntity ConstEntity::GetParent() const
	{
		const UUID parent = GetComponent<RelationshipComponent>().Parent;
		return parent.IsValid() ? m_Scene->FindEntityByID(parent) : ConstEntity{};
	}

	std::span<const UUID> ConstEntity::GetChildren() const
	{
		return GetComponent<RelationshipComponent>().Children;
	}

	uint32_t ConstEntity::GetSiblingIndex() const
	{
		const ConstEntity parent = GetParent();
		const std::span<const UUID> siblings = parent.IsValid() ? parent.GetChildren() : m_Scene->GetRootEntities();
		const UUID id = GetUUID();
		const auto position = std::ranges::find(siblings, id);
		ENGINE_CORE_ASSERT(position != siblings.end(), "Entity {} is missing from its parent's child list", id);
		return static_cast<uint32_t>(position - siblings.begin());
	}

	bool ConstEntity::IsActiveSelf() const
	{
		return !HasComponent<DisabledTag>();
	}

	bool ConstEntity::IsActive() const
	{
		for (ConstEntity current = *this; current.IsValid(); current = current.GetParent())
		{
			if (!current.IsActiveSelf())
				return false;
		}
		return true;
	}

	bool ConstEntity::HasTag(std::string_view tag) const
	{
		const std::vector<std::string>& tags = GetComponent<TagsComponent>().Tags;
		return std::find(tags.begin(), tags.end(), tag) != tags.end();
	}

	std::span<const std::string> ConstEntity::GetTags() const
	{
		return GetComponent<TagsComponent>().Tags;
	}

	void ConstEntity::AssertValid() const
	{
		ENGINE_CORE_ASSERT(IsValid(), "Use of an invalid or destroyed entity");
	}

	bool Entity::IsValid() const
	{
		return m_Scene != nullptr && Utils::IsLiveEntity(m_Scene->m_Registry, m_Handle);
	}

	UUID Entity::GetUUID() const
	{
		return ConstEntity(*this).GetUUID();
	}

	const std::string& Entity::GetName() const
	{
		return ConstEntity(*this).GetName();
	}

	void Entity::SetName(std::string_view name) const
	{
		if (GetName() == name)
			return; // not a mutation
		Patch<NameComponent>([name](NameComponent& component)
		{
			component.Name = std::string(name);
		});
	}

	Entity Entity::GetParent() const
	{
		const ConstEntity parent = ConstEntity(*this).GetParent();
		return parent.IsValid() ? Entity(parent.GetHandle(), m_Scene) : Entity{};
	}

	std::span<const UUID> Entity::GetChildren() const
	{
		return ConstEntity(*this).GetChildren();
	}

	uint32_t Entity::GetSiblingIndex() const
	{
		return ConstEntity(*this).GetSiblingIndex();
	}

	bool Entity::IsActiveSelf() const
	{
		return ConstEntity(*this).IsActiveSelf();
	}

	bool Entity::IsActive() const
	{
		return ConstEntity(*this).IsActive();
	}

	void Entity::SetActive(bool active) const
	{
		if (active == IsActiveSelf())
			return; // not a mutation
		if (active)
			RemoveComponent<DisabledTag>();
		else
			AddComponent<DisabledTag>();
	}

	bool Entity::HasTag(std::string_view tag) const
	{
		return ConstEntity(*this).HasTag(tag);
	}

	void Entity::AddTag(std::string_view tag) const
	{
		ENGINE_CORE_ASSERT(!tag.empty(), "Entity::AddTag needs a non-empty tag");
		if (HasTag(tag))
			return;
		Patch<TagsComponent>([tag](TagsComponent& component)
		{
			component.Tags.emplace_back(tag);
		});
	}

	void Entity::RemoveTag(std::string_view tag) const
	{
		ENGINE_CORE_ASSERT(!tag.empty(), "Entity::RemoveTag needs a non-empty tag");
		if (!HasTag(tag))
			return;
		Patch<TagsComponent>([tag](TagsComponent& component)
		{
			std::erase(component.Tags, tag);
		});
	}

	std::span<const std::string> Entity::GetTags() const
	{
		return ConstEntity(*this).GetTags();
	}

	void Entity::AssertValid() const
	{
		ENGINE_CORE_ASSERT(IsValid(), "Use of an invalid or destroyed entity");
	}

}
