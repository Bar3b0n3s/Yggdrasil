#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Prefab.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class IFieldSchemaSource;
	class Scene;

	// What every PrefabInstantiator operation needs besides the scene and the prefab. Passed explicitly (no default) so a
	// caller cannot silently lose script field remapping.
	struct PrefabOptions
	{
		// Script field schemas (M13; Test::FixtureSchemaSource in tests). They say which ScriptComponent.Fields values are
		// Entity references, which are remapped, diffed and mapped back like EntityRef fields, and they resolve Variant
		// values when member JSON is read. May be null: script field values are then copied verbatim (never remapped) and
		// kept with REFLECTION_VARIANT_UNRESOLVED.
		const IFieldSchemaSource* Schemas = nullptr;
	};

	// A per-slot Field override's target (M20, Docs/Decisions/0031-m20-contract.md decision 19): the index of a slot in a
	// Script component's list and one member of that slot.
	struct ScriptSlotField
	{
		uint32_t Slot = 0;
		std::string Member{}; // "Script", "Fields" or "ExecutionOrder"
	};

	// Where and how Instantiate creates one instance.
	struct PrefabInstantiateOptions
	{
		AssetHandle PrefabHandle; // stored in PrefabInstanceComponent::Prefab (may be null for an in-memory prefab)
		// The instance root's ID: valid and unused in the scene (the editor draws it from its generator, scripts from the
		// session's seeded generator, §11.6).
		UUID RootID;
		Entity Parent;                                   // invalid = a root of the scene
		std::optional<uint32_t> SiblingIndex;            // the end of the parent's children when absent
		std::optional<TransformComponent> RootTransform; // replaces the prefab root's local transform when set
	};

	// Prefab instances (Architecture §5.5). Instantiation is the single path the editor, automation and scripts use (M6 adds
	// Scene-level asset resolution on top of it), and this class is the only code that adds or removes the engine-maintained
	// Prefab and PrefabLink components (ComponentAccess rejects writes to Hidden components).
	//
	// Member IDs are derived deterministically: the root gets RootID and every other member Hash64(RootID,
	// PrefabEntityID) (an entity of a nested instance: fold(Hash64, RootID, its override path), below), so external
	// references into an instance survive updates. Internal references, i.e. EntityRef
	// values and Entity-kind script field values (PrefabOptions::Schemas) that name a prefab-local ID of the same prefab,
	// are remapped with the same function whenever members are built. The root gets PrefabInstanceComponent, every member
	// PrefabLinkComponent.
	//
	// Overrides (PrefabOverride) are field-level {Path, Component, Field, Value}, component-level {AddComponent |
	// RemoveComponent} and entity keys {EntityKey: Name, Active or Tags}, so a deactivated or renamed member stays so
	// across updates. Root Name, Transform and Parent are always implicit overrides. Entities under an instance without
	// PrefabLinkComponent are user children and survive updates. Deleting a member is recorded as a RemoveEntity override
	// (M20, below).
	//
	// Comparisons between an instance and its prefab (ComputeOverrides, RefreshOverrides) are made in instance space: the
	// prefab entity's JSON is first remapped as instantiation would remap it, so a fresh instance has no overrides and an
	// internal reference that still points at the same member is not an override. Override values and ApplyOverrides map
	// instance IDs back to prefab-local IDs through the member table (PrefabLinkComponent), so neither overrides nor the
	// prefab ever store derived IDs; a reference to an entity outside the instance is kept as it is. An outside entity
	// whose ID is also a prefab-local ID of the prefab (its source entity, when the prefab was created from entities that
	// stayed in the scene) cannot be told apart from that prefab entity once stored, so ComputeOverrides, RefreshOverrides
	// and ApplyOverrides reject a reference to it (InvalidState) instead of storing a value the next update would redirect
	// to the instance's own member.
	//
	// A Map of Variants (Script.Fields) holds independently authored values (§11.2), so its Field override records only the
	// keys that differ from the prefab, as an RFC 7386 patch (null for a key the instance removed), and applying it changes
	// only those keys: the prefab's edits to the other keys still reach the instance.
	//
	// M20 (Docs/Decisions/0031-m20-contract.md decisions 7 and 8; the functions above keep their signatures and take this
	// behaviour with streams A1 and A2):
	//   - RemoveEntity. A member deleted from an instance in the edit scene is recorded by the diff, as every override is:
	//     PrefabOverrideKind::RemoveEntity for each prefab entity absent from the instance whose prefab parent is present,
	//     and updates and reverts build neither it nor its prefab descendants until the override goes (prefab.revert).
	//   - Live nested prefabs. A prefab document holds instances of other prefabs exactly as a scene holds them (expanded,
	//     linked, with their overrides). An instance nested in a document is an instance of its prefab at its root's id: a
	//     nested root's id derives at the level that encloses it, Hash64(enclosing root, its id in the enclosing prefab),
	//     which its PrefabLink records as OuterPrefabEntityID (null on every other entity), and its members derive from it.
	//     Every PrefabLink keeps naming the entity's own innermost instance (a root, nested or not, names itself). Only an
	//     instance outermost in its document (OuterPrefabEntityID null) records overrides, each targeting an entity by its
	//     path (PrefabInstance version 2): its id in the outermost prefab, or the ids of the nested roots on the way and then
	//     its id in its own prefab. Updates rebuild nested instances from the outer prefab's document, so a nested prefab's
	//     edit reaches an instance once the prefab assets nesting it are rewritten (EditorContext's prefab updates,
	//     UpdateNestedPrefabInstances). UpdateInstance, Revert, ComputeOverrides, RefreshOverrides, the two-argument
	//     ApplyOverrides and Unpack work on an outermost instance root only: a nested root is refused with InvalidArgument
	//     naming its outermost root. Unpack unpacks one level, and the two-argument ApplyOverrides writes the overrides an
	//     instance records within a nested instance into the document's record of that instance (decision 9).
	//
	// M20 per-slot overrides (S-MULTI; Docs/Decisions/0031-m20-contract.md decision 19; stream M-B). The Script component is
	// an ordered list of slots (Script version 2), and its Field overrides target one slot's member: Field is
	// "Scripts/<slot>/<member>", <slot> the slot's index in the prefab entity's list and <member> one of Script, Fields
	// and ExecutionOrder (ScriptSlotField). When the member's list has as many slots as the prefab entity's, the diff records
	// one such override per differing slot member, a slot's Fields as the RFC 7386 key patch above; when the lengths
	// differ, it records the whole list as one Field override "Scripts" instead, as for any other list field. Applying a
	// slot override whose slot the prefab entity no longer has drops it with PREFAB_STALE_OVERRIDE.
	//
	// Static functions only; main thread, like the scene. Every mutating function is atomic: on error the scene is unchanged.
	class PrefabInstantiator
	{
	public:
		// The ID of the member for prefab entity `prefabEntityID` in the instance rooted at `instanceRootID`:
		// UUID(Hash64(instanceRootID, prefabEntityID)). The root itself uses instanceRootID.
		[[nodiscard]] static UUID DeriveInstanceID(UUID instanceRootID, UUID prefabEntityID);

		// Deep-copies `prefab` into `scene` under instance.Parent: derived IDs, remapped internal references,
		// PrefabInstanceComponent on the root (Prefab = instance.PrefabHandle, no overrides), PrefabLinkComponent on every
		// member, instance.RootTransform applied; unknown components keep the prefab's data and "ComponentVersions" entries.
		// Recorded by the change tracker; read diagnostics (unresolvable script field values) are appended to `report`.
		// Errors: InvalidArgument for an empty prefab, an invalid RootID or a parent of another scene; AlreadyExists when
		// RootID is already used in the scene; InvalidState when a derived ID is (astronomically unlikely, reported rather
		// than re-hashed so IDs stay a pure function of the root); Validation for data the registry rejects (an invalid
		// RootTransform, a unique-per-scene component the scene already has).
		[[nodiscard]] static Result<Entity> Instantiate(Scene& scene, const Prefab& prefab, const PrefabInstantiateOptions& instance,
			const PrefabOptions& options, LoadReport& report);

		// Rebuilds the instance rooted at `instanceRoot` as `prefab` plus its recorded overrides after the prefab changed
		// (§5.5 "Update"): members keep their derived IDs and internal references are remapped again; members whose prefab
		// entity no longer exists, or is removed by a RemoveEntity override with its prefab descendants, disappear (their
		// user children move to the nearest surviving ancestor), and another entity may hold a removed entity's derived ID;
		// new prefab entities appear; Field, component and EntityKey overrides are applied, each validated against the
		// registry on its own as ComponentAccess validates a write (relations, uniqueness, field metadata, type-level rules),
		// with script field overrides resolved against the member's own ScriptComponent and, as in a file read, unresolvable
		// Variant values kept; overrides that no longer match a field, component or entity key, or whose value is no longer
		// valid there, overrides of a removed entity, and RemoveEntity overrides of an entity the prefab no longer has, of
		// its root or of an entity below another removed one, are dropped with a PREFAB_STALE_OVERRIDE warning appended to
		// `report`; user children (which keep their local transform when their member disappears), the root's Name,
		// Transform and Parent, and external references into the instance are preserved. Member children follow the
		// prefab's sibling order, user children keep their places among them; unknown components take the prefab's data
		// and versions. Errors: InvalidArgument when `instanceRoot` is not an outermost instance root, is an entity of
		// another scene than `scene`, or `prefab` is empty; InvalidState when the derived ID of a new prefab entity is used
		// outside the instance; Validation when a unique-per-scene component of the instance, after its overrides, collides
		// with one elsewhere in the scene.
		[[nodiscard]] static Status UpdateInstance(Scene& scene, Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options,
			LoadReport& report);

		// The overrides the instance currently has relative to `prefab`, compared in instance space (see the class
		// comment): one Field override per serialized field whose JSON differs from the remapped prefab entity's,
		// AddComponent for components the prefab entity lacks, RemoveComponent for prefab components the member lacks, one
		// EntityKey override per entity key (Name, Active, Tags) that differs, and one RemoveEntity per prefab entity without
		// a member in the instance whose prefab parent has one (the top of each deleted subtree); implicit root overrides
		// excluded; sorted by (Path, Component, Kind, Field). Errors: InvalidArgument when `instanceRoot` is not an
		// outermost instance root or `prefab` is empty; InvalidState for a reference to an entity outside the instance whose ID is a
		// prefab-local ID (see the class comment), located at the member.
		[[nodiscard]] static Result<std::vector<PrefabOverride>> ComputeOverrides(Entity instanceRoot, const Prefab& prefab,
			const PrefabOptions& options);

		// Stores ComputeOverrides into the root's PrefabInstanceComponent (after a tracked edit touched instance members,
		// §5.5 "When an edit commits on an instance member, the change tracker records field-level overrides"); unchanged
		// overrides are not written. Errors: as ComputeOverrides, with the stored overrides unchanged.
		[[nodiscard]] static Status RefreshOverrides(Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options);

		// prefab.revert: clears the overrides and rebuilds the instance from `prefab` (user children kept), so deleted members
		// come back with their derived IDs. Errors: as UpdateInstance.
		[[nodiscard]] static Status Revert(Scene& scene, Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options,
			LoadReport& report);

		// prefab.unpack: removes PrefabInstanceComponent from the root and PrefabLinkComponent from every member of its own;
		// the entities and their data stay. One level (decision 8): each instance nested directly in it becomes outermost,
		// its OuterPrefabEntityID cleared, taking over the overrides the root recorded within it (re-based onto it, its own
		// Name and Transform left out; the editor's commit then recomputes them against its prefab). Errors: InvalidArgument
		// when `instanceRoot` is not an outermost instance root.
		[[nodiscard]] static Status Unpack(Entity instanceRoot);

		// prefab.apply: `prefab` with the instance's overrides written back (the instance's members become the prefab's
		// entities, entity keys included; user children are not added; a prefab entity deleted from the instance is left out
		// with its descendants, and a reference to its deleted member keeps its prefab-local ID), with internal references
		// mapped back to prefab-local IDs and unknown components at their recorded versions (the prefab's when the member
		// records none). An instance nested in it stays live in the document, outermost there, its record there composed with
		// the overrides the instance records within it (decision 9).
		// The caller writes the asset and then updates every instance. Errors: InvalidArgument when `instanceRoot` is not an
		// outermost instance root or `prefab` is empty; InvalidState for a reference that ComputeOverrides rejects; Validation when
		// the written-back document fails to load as a prefab.
		[[nodiscard]] static Result<Prefab> ApplyOverrides(Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options);

		// --- Nesting (M20, E-PFB-NEST; Docs/Decisions/0031-m20-contract.md decisions 8 and 9) ----------------------------

		// The instance roots that `entity` belongs to, from the outermost to the innermost: the instance root that encloses
		// every other, then each nested instance root on the way down, ending with the innermost instance that has `entity`
		// as its root or a member. An outermost instance's own member gives one root; a member of a nested instance gives
		// the outermost root, then each nested root down to its own; an entity outside every instance (a user child
		// included) gives an empty list. These are the levels prefab.apply offers (decision 9). Errors: InvalidArgument for an
		// invalid entity; InvalidState for a broken link chain (a PrefabLink naming no instance root, an OuterPrefabEntityID
		// that no enclosing instance root derives the entity's id from).
		[[nodiscard]] static Result<std::vector<Entity>> GetInstanceLevels(Entity entity);

		// The override path of `entity` within its outermost instance (PrefabOverride::Path): for an entity of the outermost
		// instance itself (its root included), its id in the outermost prefab; for an entity of a nested instance, the
		// OuterPrefabEntityID of each nested root from the outermost level inwards, then the entity's own PrefabEntityID
		// unless it is that nested instance's root. So the outermost root's path is its prefab root's id, a nested root's
		// ends with its id in the prefab that nests it, and fold(Hash64, outermost root, path) is the entity's id for every
		// entity but the outermost root. Errors: InvalidArgument for an entity outside every instance; those of
		// GetInstanceLevels.
		[[nodiscard]] static Result<std::vector<UUID>> GetOverridePath(ConstEntity entity);

		// prefab.apply at a level (decision 9): `levelPrefab`, the prefab of the instance rooted at `level`, with the
		// overrides of `instance`'s outermost instance that target `instance` or entities below it in the instance (all of
		// them when `instance` is the outermost root) written back, their paths re-based onto `level`'s prefab (the part of
		// the path that leads to `level` dropped). An override of `level` itself, whose path ends at `level`, is re-based to
		// [the id of `levelPrefab`'s root], except the level root's Name, Transform and Parent: those are its placement in
		// the prefab that encloses it, implicit at its own level as every instance root's are, so they are never written
		// into `levelPrefab` and stay recorded on the outermost root (the root's Active, Tags and components are written
		// like any member's). An override of an instance nested in `level`'s prefab is written as that nested instance's
		// own override in `levelPrefab`'s document; a RemoveEntity removes the entity and its descendants from it. `level`
		// is one of GetInstanceLevels(instance), `instance` itself included; ApplyOverrides above is the
		// case `level` = `instance` = the outermost root. The caller writes the asset, rewrites the prefab assets nesting it
		// and updates their instances. Errors: InvalidArgument when `instance` is not an instance root, `level` is not one of
		// its levels or `levelPrefab` is empty; InvalidState for a reference that ComputeOverrides rejects; Validation when
		// the written-back document fails to load as a prefab.
		[[nodiscard]] static Result<Prefab> ApplyOverrides(Entity instance, Entity level, const Prefab& levelPrefab, const PrefabOptions& options);

		// --- Script slots (M20, S-MULTI; Docs/Decisions/0031-m20-contract.md decision 19) ----------------------------------

		// The slot and member a Field override of the Script component names when its Field is "Scripts/<slot>/<member>":
		// <slot> a decimal index without leading zeros (0 to 2^32 - 1) and <member> Script, Fields or ExecutionOrder;
		// nullopt for anything else ("Scripts" itself included). The override resolver, the diff, the instantiator and the
		// inspector's override list read Field through it.
		[[nodiscard]] static std::optional<ScriptSlotField> ParseScriptSlotField(std::string_view field);
		// The Field of a per-slot override: "Scripts/<slot>/<member>". `member` is Script, Fields or ExecutionOrder (asserted).
		[[nodiscard]] static std::string MakeScriptSlotField(uint32_t slot, std::string_view member);
	};

}
