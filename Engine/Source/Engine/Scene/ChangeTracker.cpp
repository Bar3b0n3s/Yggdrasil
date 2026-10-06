#include "EnginePCH.h"
#include "Engine/Scene/ChangeTracker.h"

#include "Engine/Core/Assert.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace Engine {

	void ChangeTracker::Begin()
	{
		ENGINE_CORE_ASSERT(!m_IsTracking, "ChangeTracker::Begin while an edit is already being tracked");
		m_Changes.clear();
		m_ChildOrdersBefore.clear();
		m_IsTracking = true;
	}

	std::vector<EntityChange> ChangeTracker::End()
	{
		ENGINE_CORE_ASSERT(m_IsTracking, "ChangeTracker::End without a tracked edit");

		std::vector<EntityChange> changes;
		changes.reserve(m_Changes.size());
		for (auto& [id, change] : m_Changes)
		{
			// SiblingIndexBefore refers to the child lists at Begin: when the parent's list was recorded before its first
			// hierarchy change, the entity's position in that list is the authoritative index.
			if (change.Kind != EntityChangeKind::Created)
			{
				const auto order = m_ChildOrdersBefore.find(change.ParentBefore);
				if (order != m_ChildOrdersBefore.end())
				{
					const auto position = std::ranges::find(order->second, id);
					ENGINE_CORE_ASSERT(position != order->second.end(), "Entity {} is missing from the recorded child list of {}", id,
						change.ParentBefore);
					if (position != order->second.end())
						change.SiblingIndexBefore = static_cast<uint32_t>(position - order->second.begin());
				}
			}
			changes.push_back(std::move(change));
		}

		std::ranges::sort(changes, [](const EntityChange& a, const EntityChange& b)
		{
			return a.EntityID < b.EntityID;
		});

		m_Changes.clear();
		m_ChildOrdersBefore.clear();
		m_IsTracking = false;
		return changes;
	}

	bool ChangeTracker::NeedsSnapshot(UUID entity) const
	{
		return m_IsTracking && !m_Changes.contains(entity);
	}

	bool ChangeTracker::NeedsChildOrder(UUID parent) const
	{
		return m_IsTracking && !m_ChildOrdersBefore.contains(parent);
	}

	void ChangeTracker::RecordChildOrder(UUID parent, std::span<const UUID> children)
	{
		ENGINE_CORE_ASSERT(NeedsChildOrder(parent), "The child order of {} is recorded twice or outside a tracked edit", parent);
		m_ChildOrdersBefore.emplace(parent, std::vector<UUID>(children.begin(), children.end()));
	}

	void ChangeTracker::RecordSnapshot(UUID entity, Ref<const Json> before, UUID parent, uint32_t siblingIndex)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "RecordSnapshot needs a valid entity ID");
		ENGINE_CORE_ASSERT(NeedsSnapshot(entity), "Entity {} is snapshotted twice or outside a tracked edit", entity);

		EntityChange change;
		change.EntityID = entity;
		change.Kind = EntityChangeKind::Modified;
		change.Before = std::move(before);
		change.ParentBefore = parent;
		change.SiblingIndexBefore = siblingIndex;
		m_Changes.emplace(entity, std::move(change));
	}

	void ChangeTracker::RecordCreated(UUID entity)
	{
		ENGINE_CORE_ASSERT(m_IsTracking, "RecordCreated outside a tracked edit");
		ENGINE_CORE_ASSERT(entity.IsValid(), "RecordCreated needs a valid entity ID");

		const auto existing = m_Changes.find(entity);
		if (existing != m_Changes.end())
		{
			// An entity that existed at Begin, was destroyed and is created again under the same ID existed before and after
			// the edit: its snapshot stays the "before" state.
			ENGINE_CORE_ASSERT(existing->second.Kind == EntityChangeKind::Destroyed, "Entity {} is created twice in one edit", entity);
			existing->second.Kind = EntityChangeKind::Modified;
			return;
		}

		EntityChange change;
		change.EntityID = entity;
		change.Kind = EntityChangeKind::Created;
		m_Changes.emplace(entity, std::move(change));
	}

	void ChangeTracker::RecordDestroyed(UUID entity)
	{
		ENGINE_CORE_ASSERT(m_IsTracking, "RecordDestroyed outside a tracked edit");

		const auto existing = m_Changes.find(entity);
		ENGINE_CORE_ASSERT(existing != m_Changes.end(), "Entity {} is destroyed without a snapshot or a creation record", entity);
		if (existing == m_Changes.end())
			return;

		ENGINE_CORE_ASSERT(existing->second.Kind != EntityChangeKind::Destroyed, "Entity {} is destroyed twice in one edit", entity);
		if (existing->second.Kind == EntityChangeKind::Created)
			m_Changes.erase(existing); // created and destroyed within the edit: nothing to undo
		else
			existing->second.Kind = EntityChangeKind::Destroyed;
	}

	void ChangeTracker::RecordComponent(UUID entity, std::string_view component)
	{
		ENGINE_CORE_ASSERT(m_IsTracking, "RecordComponent outside a tracked edit");
		ENGINE_CORE_ASSERT(!component.empty(), "RecordComponent needs a component name");

		const auto existing = m_Changes.find(entity);
		ENGINE_CORE_ASSERT(existing != m_Changes.end(), "Component '{}' of entity {} is recorded before the entity's first touch", component,
			entity);
		if (existing == m_Changes.end())
			return;

		std::vector<std::string>& components = existing->second.Components;
		if (std::find(components.begin(), components.end(), component) == components.end())
			components.emplace_back(component);
	}

}
