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
	// PrefabEntityID), so external references into an instance survive updates. Internal references, i.e. EntityRef
	// values and Entity-kind script field values (PrefabOptions::Schemas) that name a prefab-local ID of the same prefab,
	// are remapped with the same function whenever members are built. The root gets PrefabInstanceComponent, every member
	// PrefabLinkComponent.
	//
	// Overrides (PrefabOverride) are field-level {PrefabEntityID, Component, Field, Value}, component-level {AddComponent |
	// RemoveComponent} and entity keys {EntityKey: Name, Active or Tags}, so a deactivated or renamed member stays so
	// across updates. Root Name, Transform and Parent are always implicit overrides. Entities under an instance without
	// PrefabLinkComponent are user children and survive updates. Deleting members inside an instance is a non-goal (§1.2:
	// deactivate or unpack instead).
	//
	// Comparisons between an instance and its prefab (ComputeOverrides, RefreshOverrides) are made in instance space: the
	// prefab entity's JSON is first remapped as instantiation would remap it, so a fresh instance has no overrides and an
	// internal reference that still points at the same member is not an override. Override values and ApplyOverrides map
	// instance IDs back to prefab-local IDs through the member table (PrefabLinkComponent), so neither overrides nor the
	// prefab ever store derived IDs; a reference to an entity outside the instance is kept as it is.
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
		// member, instance.RootTransform applied. Recorded by the change tracker. Errors: InvalidArgument for an empty prefab
		// or an invalid RootID; InvalidState when a derived ID is already used in the scene (astronomically unlikely, reported
		// rather than re-hashed so IDs stay a pure function of the root); Validation for prefab data the registry rejects.
		[[nodiscard]] static Result<Entity> Instantiate(Scene& scene, const Prefab& prefab, const PrefabInstantiateOptions& instance,
			const PrefabOptions& options, LoadReport& report);

		// Rebuilds the instance rooted at `instanceRoot` as `prefab` plus its recorded overrides after the prefab changed
		// (§5.5 "Update"): members keep their derived IDs and internal references are remapped again; members whose prefab
		// entity no longer exists disappear (their user children move to the nearest surviving ancestor); new prefab entities
		// appear; Field, component and EntityKey overrides are applied (through ComponentAccess, so script field overrides
		// resolve against the member's own ScriptComponent); overrides that no longer match a field, component or entity key
		// are dropped with a warning in `report`; user children, the root's Name, Transform and Parent, and external
		// references into the instance are preserved. Errors: InvalidArgument when `instanceRoot` is not an instance root;
		// Validation.
		[[nodiscard]] static Status UpdateInstance(Scene& scene, Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options,
			LoadReport& report);

		// The overrides the instance currently has relative to `prefab`, compared in instance space (see the class
		// comment): one Field override per serialized field whose JSON differs from the remapped prefab entity's,
		// AddComponent for components the prefab entity lacks, RemoveComponent for prefab components the member lacks, and
		// one EntityKey override per entity key (Name, Active, Tags) that differs; implicit root overrides excluded; sorted
		// by (PrefabEntityID, Component, Kind, Field). Errors: InvalidArgument when `instanceRoot` is not an instance root.
		[[nodiscard]] static Result<std::vector<PrefabOverride>> ComputeOverrides(Entity instanceRoot, const Prefab& prefab,
			const PrefabOptions& options);

		// Stores ComputeOverrides into the root's PrefabInstanceComponent (after a tracked edit touched instance members,
		// §5.5 "When an edit commits on an instance member, the change tracker records field-level overrides").
		[[nodiscard]] static Status RefreshOverrides(Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options);

		// prefab.revert: clears the overrides and rebuilds the instance from `prefab` (user children kept).
		[[nodiscard]] static Status Revert(Scene& scene, Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options,
			LoadReport& report);

		// prefab.unpack: removes PrefabInstanceComponent from the root and PrefabLinkComponent from every member; the
		// entities and their data stay. Errors: InvalidArgument when `instanceRoot` is not an instance root.
		[[nodiscard]] static Status Unpack(Entity instanceRoot);

		// prefab.apply: `prefab` with the instance's overrides written back (the instance's members become the prefab's
		// entities, entity keys included; user children are not added), with internal references mapped back to
		// prefab-local IDs. The caller writes the asset and then updates every instance.
		[[nodiscard]] static Result<Prefab> ApplyOverrides(Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options);
	};

}
