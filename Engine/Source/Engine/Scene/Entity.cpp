#include "EnginePCH.h"
#include "Engine/Scene/Entity.h"

#include "Engine/Core/Assert.h"

// M3 contract stub (Roadmap rule 3): stream B (Scene core) implements the non-template Entity and ConstEntity members.
// The component templates in Entity.h are complete and call the scene's change hooks.

namespace Engine {

	bool ConstEntity::IsValid() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	UUID ConstEntity::GetUUID() const
	{
		ENGINE_CONTRACT_STUB();
		return UUID();
	}

	const std::string& ConstEntity::GetName() const
	{
		ENGINE_CONTRACT_STUB();
		static const std::string EmptyName;
		return EmptyName;
	}

	ConstEntity ConstEntity::GetParent() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::span<const UUID> ConstEntity::GetChildren() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint32_t ConstEntity::GetSiblingIndex() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	bool ConstEntity::IsActiveSelf() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool ConstEntity::IsActive() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool ConstEntity::HasTag(std::string_view /*tag*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::span<const std::string> ConstEntity::GetTags() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void ConstEntity::AssertValid() const
	{
		ENGINE_CORE_ASSERT(IsValid(), "Use of an invalid or destroyed entity");
	}

	bool Entity::IsValid() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	UUID Entity::GetUUID() const
	{
		ENGINE_CONTRACT_STUB();
		return UUID();
	}

	const std::string& Entity::GetName() const
	{
		ENGINE_CONTRACT_STUB();
		static const std::string EmptyName;
		return EmptyName;
	}

	void Entity::SetName(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	Entity Entity::GetParent() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::span<const UUID> Entity::GetChildren() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint32_t Entity::GetSiblingIndex() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	bool Entity::IsActiveSelf() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool Entity::IsActive() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void Entity::SetActive(bool /*active*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	bool Entity::HasTag(std::string_view /*tag*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void Entity::AddTag(std::string_view /*tag*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	void Entity::RemoveTag(std::string_view /*tag*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	std::span<const std::string> Entity::GetTags() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void Entity::AssertValid() const
	{
		ENGINE_CORE_ASSERT(IsValid(), "Use of an invalid or destroyed entity");
	}

}
