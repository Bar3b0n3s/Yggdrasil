#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Engine {

	enum class EntityChangeKind : uint8_t
	{
		Modified, // existed before and after
		Created,  // created while tracking (Before is null)
		Destroyed // destroyed while tracking (it may also have been modified first)
	};

	// What happened to one entity during a tracked edit (Architecture §12.3 SceneEditCommand, §5.5 override recording):
	// its canonical entity JSON from before its first change (SceneSerializer::EntityToJson), its place in the hierarchy
	// when the edit began, and the registry names of the components that were added, patched or removed ("Active" for
	// DisabledTag). An entity created and destroyed within the same edit does not appear.
	//
	// SiblingIndexBefore is relative to the parent's child list (or the root list) as it was when the edit began, not
	// when the entity was first touched, so it is independent of the order in which one edit moved or destroyed siblings.
	// Undo restores an exact prior order by taking the touched entities out of their current lists and re-inserting each
	// parent's restored children in ascending SiblingIndexBefore order: the untouched siblings kept their relative order,
	// so every restored child lands at its original index.
	struct EntityChange
	{
		UUID EntityID;
		EntityChangeKind Kind = EntityChangeKind::Modified;
		Ref<const Json> Before;              // null for Created (and if the entity could not be serialized, which logs an error)
		UUID ParentBefore;                   // the invalid UUID for a root or a Created entity
		uint32_t SiblingIndexBefore = 0;     // 0 for a Created entity
		std::vector<std::string> Components; // unique, in the order first recorded
	};

	// Records which entities an edit touches (Architecture §5.1, §12.3). The Scene owns one and feeds it from every
	// mutation path: Entity's component templates, Entity::Patch, the reflected setters (ComponentAccess), hierarchy
	// operations, creation and destruction. Between Begin and End the first change of each entity records a snapshot of it
	// first, and the first hierarchy change under a parent records that parent's child order, so SceneEdit (M4) can build
	// an undo step from "before" snapshots and the scene's current state, and the prefab instantiator can derive overrides
	// for instance members (§5.5). Runtime systems that write components directly, and runtime-only components, are not
	// tracked (they only run in play scenes, which are never undone).
	//
	// Not reentrant: Begin while tracking is a programmer error (asserted). Main thread only (§4.11), like the scene.
	class ChangeTracker
	{
	public:
		// Starts a tracked edit (asserts that none is active).
		void Begin();
		[[nodiscard]] bool IsTracking() const { return m_IsTracking; }

		// Ends the tracked edit and returns one EntityChange per touched entity, sorted by UUID (canonical order, §4.12);
		// the tracker is idle and empty afterwards. Asserts that an edit is active.
		[[nodiscard]] std::vector<EntityChange> End();

		// True while tracking when `entity` has not been touched yet in this edit, i.e. when the scene must take a snapshot
		// before changing it.
		[[nodiscard]] bool NeedsSnapshot(UUID entity) const;

		// True while tracking when the child list of `parent` (the invalid UUID: the root list) has not been recorded yet in
		// this edit, i.e. when the scene must record it before the first hierarchy change under that parent (a child
		// created, destroyed, moved in or out, or reordered).
		[[nodiscard]] bool NeedsChildOrder(UUID parent) const;

		// Records the child list of `parent` as it is before the first hierarchy change under it in this edit (once per
		// parent; asserted). End() derives SiblingIndexBefore from these lists.
		void RecordChildOrder(UUID parent, std::span<const UUID> children);

		// Records the pre-change state of an existing entity (first touch only; asserted): its snapshot and its current
		// parent and sibling index. When that parent's child order was recorded earlier in this edit, End() reports the
		// entity's index in the recorded list instead, so SiblingIndexBefore always refers to the list at Begin.
		void RecordSnapshot(UUID entity, Ref<const Json> before, UUID parent, uint32_t siblingIndex);
		// Records that `entity` was created during this edit.
		void RecordCreated(UUID entity);
		// Records that `entity` was destroyed during this edit (after its snapshot, if it existed before).
		void RecordDestroyed(UUID entity);
		// Records that `component` (a registry name) of `entity` was added, patched or removed.
		void RecordComponent(UUID entity, std::string_view component);
	private:
		std::unordered_map<UUID, EntityChange> m_Changes;                // lookup only; End() sorts by UUID before returning
		std::unordered_map<UUID, std::vector<UUID>> m_ChildOrdersBefore; // lookup only; parent -> children at Begin
		bool m_IsTracking = false;
	};

}
