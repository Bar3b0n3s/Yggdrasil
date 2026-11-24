#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/Components/RuntimeComponents.h"

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

	// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): an entity's place in the canonical order (§5.1): the ordinal
	// of its root among the root entities, then its index in its root's segment (the root's subtree in depth-first pre-order,
	// the root at 0). Keys compare as canonical positions.
	struct CanonicalKey
	{
		uint32_t Root = 0;
		uint32_t Index = 0;

		[[nodiscard]] auto operator<=>(const CanonicalKey&) const = default;
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
		// once, while Entity::IsReadable stays true until the flush: FindReadableEntityByID) and destroy at the next
		// FlushPendingDestroys (§5.7 step 8). Asserts a valid entity of this scene.
		void DestroyEntity(Entity entity);

		// Destroys every entity marked by DestroyEntity in a runtime scene, children first. No-op for edit scenes.
		void FlushPendingDestroys();

		// Destroys every entity; name, seed and revision counter are kept (the revision increments).
		void Clear();

		// The entity with `id`, or an invalid handle (also for the invalid UUID and pending-destroy entities). The const
		// overload returns a read-only handle.
		[[nodiscard]] Entity FindEntityByID(UUID id);
		[[nodiscard]] ConstEntity FindEntityByID(UUID id) const;

		// The entity with `id` that readers may still use (M20, W-P02; Docs/Decisions/0031-m20-contract.md decision 4): the
		// live entity FindEntityByID returns, or else, in a runtime scene, an entity marked by DestroyEntity, until the
		// FlushPendingDestroys that destroys it. A pending entity's handle is not IsValid but IsReadable: its read accessors
		// (its identity, name, components, tags, active state, parent and children) work until the flush, while its mutating
		// members assert. An invalid handle for the invalid UUID and for an id that names neither. Only this lookup returns a
		// pending entity: FindEntityByID, FindEntityByPath, GetCanonicalOrder, ForEachCanonical and GetEntityCount leave it
		// out.
		[[nodiscard]] Entity FindReadableEntityByID(UUID id);
		[[nodiscard]] ConstEntity FindReadableEntityByID(UUID id) const;

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
		// destruction, reparenting): since M43 the concatenation of the segments, one per root, each rebuilt alone when a
		// change touches its tree. The span is invalidated by the next structural change.
		[[nodiscard]] std::span<const UUID> GetCanonicalOrder() const;

		// M43: the canonical key of a valid entity of this scene (asserted), from the segments, rebuilt lazily as the order is.
		[[nodiscard]] CanonicalKey GetCanonicalKey(ConstEntity entity) const;

		// M43: `out` set to the live entities (pending destruction excluded) that hold every component of T..., in canonical
		// order: the EnTT view of T... sorted by canonical key, O(k log k) for k such entities, so a per-tick pass over a
		// component visits only its holders and keeps today's order (§5.1). Defined in Entity.h.
		template<typename... T>
		void CollectCanonical(std::vector<entt::entity>& out) const;

		// M43: sorts `entities` (live, distinct handles of this scene; asserted) into canonical order.
		void SortCanonical(std::vector<entt::entity>& entities) const;

		// Calls `func(Entity)` (the const overload: `func(ConstEntity)`) for every entity in canonical order, over a snapshot
		// of the order taken at the call (entities created during the iteration are not visited, destroyed ones are
		// skipped). Defined in Entity.h, which callers include for the handle types anyway.
		template<typename Func>
		void ForEachCanonical(Func&& func);
		template<typename Func>
		void ForEachCanonical(Func&& func) const;

		// Increments on every mutation made through the scene, Entity or ComponentAccess (§5.1, automation ifRevision).
		[[nodiscard]] uint64_t GetRevision() const { return m_Revision; }

		// M43: increments only with structural changes: creation, destruction, reparenting, sibling order, enabling or
		// disabling (DisabledTag) and Clear. A data write never moves it, so a system that caches the hierarchy (the physics
		// hierarchy record, the scripts' lifecycle order, the name and tag index) keys on it instead of GetRevision.
		[[nodiscard]] uint64_t GetStructureRevision() const { return m_StructureRevision; }

		// --- M43: change-tracked transforms (§5.2; Docs/Decisions/0061-m43-static-scenery.md decision 3) -------------------
		// TransformComponent's on_construct and on_update (every write through Entity::Patch, the reflected setters,
		// SetParent and EnTT's patch or replace) mark the entity written, and TransformSystem::Update recomputes the written
		// entities' subtrees only. A system that writes a TransformComponent directly calls MarkTransformWritten after the
		// write; only the files of ModuleRules.json's Banned.TransformWriteFiles may (Lint banned-transform-write), and Debug
		// builds' full-recompute check (TransformSystem::Update) fails on a write it missed. Asserts a readable entity of
		// this scene.
		void MarkTransformWritten(Entity entity);

		// A consumer's list of world-matrix changes: every TransformSystem::Update appends to every open list the entities
		// whose world matrix it changed, and the written ones whose matrix kept its bits. Ids are reused after
		// CloseWorldChangeList.
		[[nodiscard]] uint32_t OpenWorldChangeList();
		void CloseWorldChangeList(uint32_t list);
		// Appends to `entities` what the open list `list` (asserted) collected since its last take, and empties it: in no
		// particular order, possibly repeated, possibly destroyed since (callers check the handle). Returns false instead,
		// appending nothing, when the list overflowed (more than four entries an entity of the scene, at least 4,096, since
		// its last take): the consumer then treats every entity as changed, as it did before change tracking.
		[[nodiscard]] bool TakeWorldChanges(uint32_t list, std::vector<entt::entity>& entities);

		// M43's work counters, monotonic over the scene's life: the world matrices TransformSystem::Update recomputed, and the
		// entities the per-entity passes of the play session's systems iterated (each pass adds its count).
		[[nodiscard]] uint64_t GetTransformsUpdated() const { return m_TransformsUpdated; }
		[[nodiscard]] uint64_t GetEntitiesVisited() const { return m_EntitiesVisited; }
		void AddEntitiesVisited(size_t count) { m_EntitiesVisited += count; }

		// XXH64 (seed 0) of the scene's minified canonical serialization (SceneSerializer, JsonStyle::Minified): equal for
		// equal scene content in every configuration (§1.3, §9.1); play sessions add physics state (M7, M11). Implemented
		// with the serializer (Scene/SceneStateHash.cpp). Returns 0 only if the scene cannot be serialized (a non-finite
		// value written directly by a runtime system), which also logs an error.
		[[nodiscard]] uint64_t ComputeStateHash() const;

		// The number of entities, pending-destroy ones excluded.
		[[nodiscard]] size_t GetEntityCount() const;
		// Runtime scenes: the entities destroyed since the last FlushPendingDestroys, which still occupy the registry.
		[[nodiscard]] size_t GetPendingDestroyCount() const { return m_PendingDestroys.size(); }

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
		// after: it records the component's registry name (DisabledTag as the entity key "Active"); for a hierarchy change it
		// invalidates the canonical order, makes every segment again and moves the structure revision (M43); for DisabledTag
		// it refreshes the subtree's HierarchyDisabledTag and moves the structure revision; for Static (M43) it refreshes the
		// subtree's HierarchyStaticTag.
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
		// Rebuilds m_CanonicalOrder: the segments, each rebuilt when dirty (RefreshCanonicalSegments, iteratively, since
		// hierarchies may be deep), concatenated in root order.
		void RebuildCanonicalOrder() const;
		// Makes the runtime-only HierarchyDisabledTag of `root` and its subtree match their effective active state (§5.2),
		// given that the tag of `root`'s parent is current. Neither counted nor tracked.
		void RefreshHierarchyDisabled(entt::entity root);
		// M43: makes HierarchyStaticTag of `root` and its subtree match the Static rule (StaticComponent.h), from the Static
		// components of `root`'s ancestors. Nothing to do while the scene holds neither a Static nor the tag.
		void RefreshHierarchyStatic(entt::entity root);

		// M43: the segments (one per root entity) and the canonical keys. A key lives in m_CanonicalKeys at the entity's
		// index (entt::to_entity) with its handle, so a destroyed entity's stale entry is never read for its successor.
		struct CanonicalSegment
		{
			UUID Root{};
			std::vector<UUID> Order{}; // the root's subtree in depth-first pre-order, current when !Dirty
			uint32_t Ordinal = 0;      // the root's index in m_RootEntities, current when !m_SegmentOrdinalsDirty
			bool Dirty = true;
			bool Live = false;
		};
		struct CanonicalKeySlot
		{
			entt::entity Handle{ entt::null };
			uint32_t Segment = 0;
			uint32_t Index = 0;
			bool TransformWritten = false; // in m_TransformWrites since the last TransformSystem::Update
		};
		struct WorldChangeList
		{
			std::vector<entt::entity> Entities{};
			bool Open = false;
			bool Overflowed = false;
		};
		// The key slot of `entity`, (re)claimed for it when it held another entity's or none.
		[[nodiscard]] CanonicalKeySlot& ClaimKeySlot(entt::entity entity) const;
		// The root of the tree that holds `entity` (the entity itself for a root).
		[[nodiscard]] entt::entity FindRootHandle(entt::entity entity) const;
		// A new segment for the root `root`; `appended` when `root` was just appended to the root list.
		void AllocateSegment(entt::entity root, bool appended);
		// Frees the segment of the root `root`.
		void ReleaseSegment(entt::entity root);
		// Marks the segment of the tree that holds `entity` for rebuilding.
		void MarkSegmentDirty(entt::entity entity);
		// Every segment made again from the root list: for a hierarchy change the scene cannot attribute to one tree.
		void ResetSegments();
		// Rebuilds the root ordinals, the dirty segments and their keys; lazily, from const readers too (the segments and
		// keys are caches beside the registry, as m_CanonicalOrder is).
		void RefreshCanonicalSegments() const;
		// on_construct and on_update of TransformComponent.
		void OnTransformWritten(entt::registry& registry, entt::entity entity);
		void RecordTransformWrite(entt::entity entity);
		// TransformSystem::Update's report: `changed` appended to every open world-change list, `recomputed` counted.
		void PublishWorldChanges(std::span<const entt::entity> changed, size_t recomputed);
	private:
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityIndex; // lookup only, never iterated to produce output (§5.1)
		std::vector<UUID> m_RootEntities;
		mutable std::vector<UUID> m_CanonicalOrder; // cache, the segments concatenated lazily when m_CanonicalOrderDirty
		mutable bool m_CanonicalOrderDirty = true;  // cache flag, see m_CanonicalOrder
		// M43: the segments by slot (free slots reused), the key slots by entity index (see CanonicalKeySlot), and whether a
		// segment or the root ordinals need rebuilding.
		mutable std::vector<CanonicalSegment> m_Segments;
		std::vector<uint32_t> m_FreeSegments;
		mutable std::vector<CanonicalKeySlot> m_CanonicalKeys;
		mutable bool m_SegmentsDirty = false;
		mutable bool m_SegmentOrdinalsDirty = false;
		// M43: the entities written since the last TransformSystem::Update, each once (CanonicalKeySlot::TransformWritten),
		// and the consumers' world-change lists.
		std::vector<entt::entity> m_TransformWrites;
		std::vector<WorldChangeList> m_WorldChangeLists;
		std::vector<entt::entity> m_LastWorldChanges; // TransformSystem::Update's result, until the next update
		uint64_t m_StructureRevision = 0;
		uint64_t m_TransformsUpdated = 0;
		uint64_t m_EntitiesVisited = 0;
		ChangeTracker m_ChangeTracker;
		SceneSpecification m_Specification; // its Registry and IdGenerator are back-references that outlive the scene
		uint64_t m_Revision = 0;
		float m_InterpolationAlpha = 1.0f;
		// Runtime scenes: entities marked by DestroyEntity, in destruction order, destroyed by FlushPendingDestroys. They
		// are already out of the UUID index and the hierarchy.
		std::vector<entt::entity> m_PendingDestroys;
		// Runtime scenes: the same entities by UUID, for FindReadableEntityByID until the flush (lookup only, never iterated
		// to produce output, §5.1). The latest marked entity of an id wins.
		std::unordered_map<UUID, entt::entity> m_PendingIndex;
		std::thread::id m_MainThread; // the creating thread (§4.11)
	private:
		friend class Entity;
		friend class TransformSystem; // M43: reads and clears m_TransformWrites and publishes the world changes
	};

}
