#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Scene/ChangeTracker.h"

#include <entt/entity/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Engine {

	class ConstEntity;
	class Entity;
	class TypeRegistry;
	class UUIDGenerator;

	struct SceneSpecification
	{
		std::string Name = "Untitled";
		uint32_t Seed = 0; // Scene.Seed of the file (§6.2); a play session seeds its Random with Project Seed ^ Scene Seed
		// Required, frozen, and outliving the scene (documented back-references, §4.7).
		const TypeRegistry* Registry = nullptr;
		// Required and outliving the scene: Random in the editor, Deterministic in play sessions, exports and tests (§4.8).
		UUIDGenerator* IdGenerator = nullptr;
		// A play or exported-game scene: DestroyEntity defers to FlushPendingDestroys (§5.7). Edit scenes destroy at once.
		bool Runtime = false;
	};

	// How a component changed, for the change tracker and the revision counter (Entity's component templates).
	enum class ComponentChangeKind : uint8_t
	{
		Added,
		Patched,
		Removed
	};

	// A scene: one entt::registry plus the UUID index, the ordered hierarchy and the change tracker (Architecture §5.1). The
	// UUID is the identity of an entity; entt::entity is transient, never serialized and never exposed to scripts or
	// automation. Every entity always has IDComponent, NameComponent, TagsComponent, RelationshipComponent and
	// TransformComponent (the Required components plus Tags, so every entity key is always written, §6.2).
	//
	// Canonical order: root entities in stored order, then depth-first through each entity's ordered children. Wherever
	// order is observable (callbacks, serialization, body creation, query results, sort ties) the engine iterates in this
	// order (GetCanonicalOrder, ForEachCanonical), never in EnTT storage order, which depends on insertion history.
	// Loading or copying a scene creates entities in canonical order.
	//
	// Mutation: editor and automation code change components only through Entity::Patch<T> or the reflected setters
	// (ComponentAccess), which notify the change tracker; runtime systems may write components directly. Every mutation
	// through the scene, Entity or ComponentAccess increments the revision (automation ifRevision, §13.4), except changes
	// to runtime-only components (RuntimeComponents.h), which are neither recorded nor counted.
	//
	// Const access: the const members hand out ConstEntity, a read-only handle, so code holding a const Scene& (the
	// serializer, the state hash) reads entities without being able to change them.
	//
	// Threading: main thread only (§4.11); Debug builds assert the thread at entry points. Not copyable or movable (entities
	// hold a back-pointer); the only scene-copy path is the serializer (§1.3, §5.6).
	class Scene
	{
	public:
		// Use Create. Asserts that the specification names a registry (frozen) and a UUID generator.
		explicit Scene(const SceneSpecification& specification);
		~Scene();

		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;
		Scene(Scene&&) = delete;
		Scene& operator=(Scene&&) = delete;

		[[nodiscard]] static Scope<Scene> Create(const SceneSpecification& specification);

		// Creates an entity with a fresh ID from the scene's UUIDGenerator, the given name and the default Required
		// components, appended as the last root or as the last child of `parent` (which must be valid and in this scene,
		// asserted). Recorded by the change tracker; increments the revision. The entity limit of play sessions
		// (Simulation.MaxEntities) is checked by the session before calling (§5.7).
		[[nodiscard]] Entity CreateEntity(std::string_view name = "Entity");
		[[nodiscard]] Entity CreateEntity(std::string_view name, Entity parent);

		// As CreateEntity with a caller-chosen ID (loaders, undo, prefab instantiation). `id` must be valid and unused in
		// this scene: a violation is in-memory API misuse and asserts, with a message containing "needs a valid ID" for the
		// invalid UUID and "is already used" for a used one; loaders pre-validate files instead (§6).
		[[nodiscard]] Entity CreateEntityWithID(UUID id, std::string_view name);
		[[nodiscard]] Entity CreateEntityWithID(UUID id, std::string_view name, Entity parent);

		// Destroys `entity` and its whole subtree in reverse canonical order (every child before its parent, later siblings
		// before earlier ones: R[A[A1, A2], B] destroys B, A2, A1, A, R), and removes it from its parent's children. Edit
		// scenes destroy immediately; runtime scenes add PendingDestroyTag to the subtree (Entity::IsValid becomes false at
		// once) and destroy at the next FlushPendingDestroys (§5.7 step 8). Asserts a valid entity of this scene.
		void DestroyEntity(Entity entity);

		// Destroys every entity marked by DestroyEntity in a runtime scene, children first. No-op for edit scenes.
		void FlushPendingDestroys();

		// Destroys every entity; name, seed and revision counter are kept (the revision increments).
		void Clear();

		// The entity with `id`, or an invalid handle (also for the invalid UUID and pending-destroy entities). The const
		// overload returns a read-only handle.
		[[nodiscard]] Entity FindEntityByID(UUID id);
		[[nodiscard]] ConstEntity FindEntityByID(UUID id) const;

		// The entity at `path`, or an invalid handle when the path is malformed, matches nothing or is ambiguous.
		// Grammar: a path is "/" followed by '/'-separated segments from a root ("/Game/Board"). A segment is an entity name
		// in which '\', '/', '[' and ']' are written with a backslash in front ("\\", "\/", "\[", "\]"); every other
		// character, spaces included, stands for itself, and a backslash before any other character is malformed. A segment
		// may end with an unescaped "[n]" (decimal, no sign, no leading zeros), the zero-based index among the siblings with
		// that name in sibling order ("/Track/Piece[3]"); a segment without an index must match exactly one sibling. Names
		// are unrestricted: the entity named "A/B" is "/A\/B" and the one named "Piece[3]" is "/Piece\[3\]".
		[[nodiscard]] Entity FindEntityByPath(std::string_view path);
		[[nodiscard]] ConstEntity FindEntityByPath(std::string_view path) const;

		// As FindEntityByPath with a reason on failure. Errors: InvalidArgument for a malformed path; NotFound naming the
		// first segment that matches nothing (with "did you mean" suggestions from its siblings); InvalidArgument for an
		// ambiguous segment, listing every candidate path as an ErrorIssue (§13.4 "ambiguous paths fail and list
		// candidates").
		[[nodiscard]] Result<Entity> ResolveEntityPath(std::string_view path);
		[[nodiscard]] Result<ConstEntity> ResolveEntityPath(std::string_view path) const;

		// The shortest path that FindEntityByPath resolves to `entity`, in the grammar above: names escaped, "[n]" only on
		// segments whose name is shared by siblings. Asserts a valid entity of this scene.
		[[nodiscard]] std::string GetEntityPath(ConstEntity entity) const;

		// Moves `child` under `parent` (an invalid Entity means "to the root list") at `siblingIndex` (clamped to the end;
		// the end when absent). keepWorld preserves the world transform by recomputing the local TRS through the new parent's
		// inverse (§5.2); otherwise the local transform is kept. Recorded by the change tracker; increments the revision.
		// Errors, with nothing changed: InvalidArgument when the move would create a cycle (parent is child or one of its
		// descendants), when either entity is invalid or belongs to another scene, or, with keepWorld, when the resulting
		// local transform is not representable (TransformSystem::DecomposeMatrix fails, for example on a scale component
		// below MinTransformScaleMagnitude, which Transform.Scale's MinMagnitude would reject on the next load).
		[[nodiscard]] Status SetParent(Entity child, Entity parent, std::optional<uint32_t> siblingIndex = {}, bool keepWorld = true);

		// The root entities in stored order.
		[[nodiscard]] std::span<const UUID> GetRootEntities() const { return m_RootEntities; }

		// Every entity in canonical order (§5.1), cached and rebuilt lazily after a structural change (creation,
		// destruction, reparenting). The span is invalidated by the next structural change.
		[[nodiscard]] std::span<const UUID> GetCanonicalOrder() const;

		// Calls `func(Entity)` (the const overload: `func(ConstEntity)`) for every entity in canonical order, over a snapshot
		// of the order taken at the call (entities created during the iteration are not visited, destroyed ones are
		// skipped). Defined in Entity.h, which callers include for the handle types anyway.
		template<typename Func>
		void ForEachCanonical(Func&& func);
		template<typename Func>
		void ForEachCanonical(Func&& func) const;

		// Increments on every mutation made through the scene, Entity or ComponentAccess (§5.1, automation ifRevision).
		[[nodiscard]] uint64_t GetRevision() const { return m_Revision; }

		// XXH64 (seed 0) of the scene's minified canonical serialization (SceneSerializer, JsonStyle::Minified): equal for
		// equal scene content in every configuration (§1.3, §9.1); play sessions add physics state (M7, M11). Implemented
		// with the serializer (Scene/SceneStateHash.cpp). Returns 0 only if the scene cannot be serialized (a non-finite
		// value written directly by a runtime system), which also logs an error.
		[[nodiscard]] uint64_t ComputeStateHash() const;

		// The number of entities, pending-destroy ones excluded.
		[[nodiscard]] size_t GetEntityCount() const;

		[[nodiscard]] const std::string& GetName() const { return m_Specification.Name; }
		void SetName(std::string name);
		[[nodiscard]] uint32_t GetSeed() const { return m_Specification.Seed; }
		void SetSeed(uint32_t seed);
		[[nodiscard]] bool IsRuntime() const { return m_Specification.Runtime; }

		// The render interpolation Alpha of the current frame (§5.2), set by the play session before the frame phase and
		// read by Transform.RenderPosition/RenderRotation; 1 outside play and inside the fixed phase.
		[[nodiscard]] float GetInterpolationAlpha() const { return m_InterpolationAlpha; }
		void SetInterpolationAlpha(float alpha);

		[[nodiscard]] const TypeRegistry& GetTypeRegistry() const { return *m_Specification.Registry; }
		[[nodiscard]] UUIDGenerator& GetUUIDGenerator() const { return *m_Specification.IdGenerator; }
		[[nodiscard]] ChangeTracker& GetChangeTracker() { return m_ChangeTracker; }
		[[nodiscard]] const ChangeTracker& GetChangeTracker() const { return m_ChangeTracker; }

		// The underlying EnTT registry, for systems that iterate component views (TransformSystem, PhysicsSystem, render
		// extraction). Writing through it bypasses the change tracker and the revision; editor and automation code never do.
		[[nodiscard]] entt::registry& GetRegistry() { return m_Registry; }
		[[nodiscard]] const entt::registry& GetRegistry() const { return m_Registry; }
	private:
		// Hooks for Entity's component templates, given the component's TypeKey. Both ignore a component type that is
		// neither registered nor DisabledTag (the runtime-only components of RuntimeComponents.h, which TransformSystem and
		// the play session maintain): no revision increment, no snapshot, no record. For any other type,
		// PrepareEntityChange runs before the entity's data changes: it increments the revision and, while tracking,
		// records the entity's snapshot on its first touch (CaptureEntitySnapshot) and, before a hierarchy change, the
		// child order of each parent whose child list changes (ChangeTracker::RecordChildOrder). CommitComponentChange runs
		// after: it records the component's registry name (DisabledTag as the entity key "Active"), invalidates the
		// canonical order when the hierarchy changed and, for DisabledTag, refreshes the subtree's HierarchyDisabledTag.
		void PrepareEntityChange(entt::entity entity, TypeKey component);
		void CommitComponentChange(entt::entity entity, TypeKey component, ComponentChangeKind kind);

		// The entity's canonical JSON (SceneSerializer::EntityToJson) as shared immutable data: the change tracker's
		// "before" snapshot. Implemented with the serializer in Scene/EntitySnapshot.cpp (stream C), so Scene.cpp never
		// depends on serializer internals. Returns null only if the entity cannot be serialized (a non-finite value written
		// directly by a runtime system), which also logs an error.
		[[nodiscard]] Ref<const Json> CaptureEntitySnapshot(entt::entity entity) const;

		// §4.11: asserts that the caller runs on the thread that created the scene.
		void AssertMainThread() const;
		// True when changes to `component` count in the revision and reach the tracker: a registered type or DisabledTag.
		[[nodiscard]] bool IsTrackedComponent(TypeKey component) const;
		// The live handle of `id` (entt::null when no entity of this scene has it).
		[[nodiscard]] entt::entity FindHandle(UUID id) const;
		// The ordered child list of `parent`, or the root list for the invalid UUID; `parent` must exist (asserted).
		[[nodiscard]] std::vector<UUID>& GetChildList(UUID parent);
		// The shared body of CreateEntity and CreateEntityWithID: `parent` is entt::null for a root.
		[[nodiscard]] Entity CreateEntityInternal(UUID id, std::string_view name, entt::entity parent);
		// While tracking: records the child list of `parent` before its first hierarchy change in the edit.
		void RecordChildOrderBeforeChange(UUID parent);
		// While tracking: records the entity's snapshot, parent and sibling index before its first change in the edit.
		void RecordFirstTouch(entt::entity entity);
		// While tracking: records `component`'s registry name ("Active" for DisabledTag) for an already touched entity.
		void RecordComponentName(entt::entity entity, TypeKey component);
		// The subtree of `root` in destruction order: children before parents, later siblings before earlier ones.
		[[nodiscard]] std::vector<entt::entity> CollectSubtreeForDestruction(entt::entity root) const;
		// While tracking: records the child orders, snapshots and destruction of `entities` (all still intact).
		void RecordDestruction(std::span<const entt::entity> entities);
		// Rebuilds m_CanonicalOrder from the root list and the child lists (iteratively; hierarchies may be deep).
		void RebuildCanonicalOrder() const;
		// Makes the runtime-only HierarchyDisabledTag of `root` and its subtree match their effective active state (§5.2),
		// given that the tag of `root`'s parent is current. Neither counted nor tracked.
		void RefreshHierarchyDisabled(entt::entity root);
	private:
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityIndex; // lookup only, never iterated to produce output (§5.1)
		std::vector<UUID> m_RootEntities;
		mutable std::vector<UUID> m_CanonicalOrder; // cache, rebuilt lazily when m_CanonicalOrderDirty
		mutable bool m_CanonicalOrderDirty = true;  // cache flag, see m_CanonicalOrder
		ChangeTracker m_ChangeTracker;
		SceneSpecification m_Specification; // its Registry and IdGenerator are back-references that outlive the scene
		uint64_t m_Revision = 0;
		float m_InterpolationAlpha = 1.0f;
		// Runtime scenes: entities marked by DestroyEntity, in destruction order, destroyed by FlushPendingDestroys. They
		// are already out of the UUID index and the hierarchy.
		std::vector<entt::entity> m_PendingDestroys;
		std::thread::id m_MainThread; // the creating thread (§4.11)
	private:
		friend class Entity;
	};

}
