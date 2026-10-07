#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// entity.* (Architecture §13.5): create, get, update, destroy, duplicate, reparent (M4) and bounds (M6, which brings
// meshes). Every edit-scene mutation is exactly one SceneEditCommand labelled for the agent and
// reports its undoIndex (§13.4); every mutation supports dry runs. Undo labels (CommandHistory adds "[agent] "):
// "Create Entity '<name>'", "Update Entity '<name>'", "Reparent Entity '<name>'", and for the list methods
// "Destroy Entity '<name>'" / "Duplicate Entity '<name>'" for one entity or "Destroy <n> Entities" /
// "Duplicate <n> Entities" for more. Entity references follow §13.4 (16 hex digits, a unique
// prefix of at least 6, or a path). Component maps use ResolveComponentValue (convention 10 of MethodRegistry.h), so
// "/components/RigidBody/Mas" is an unknown-field InvalidParams with the hint "did you mean 'Mass'?".

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// entity.create {name?, parent?, index?, active?, tags?, components?, target?}: a new entity (a fresh id from the editor's
	// generator) under `parent` (empty: a root) at sibling `index` (absent: last), with `components` added and set
	// (missing fields keep their defaults; Transform is always present and may be given). All or nothing.
	struct EntityCreateParams
	{
		std::string Name{}; // required (§13.5)
		std::string Parent{};
		uint32_t Index = 0; // only when given (MethodContext::HasParam)
		bool Active = true;
		std::vector<std::string> Tags{};
		std::map<std::string, VariantValue> Components{};
		SceneTarget Target = SceneTarget::Edit;
	};

	struct EntityCreateResult
	{
		EntitySummary Entity{};
		uint32_t UndoIndex = 0;
	};

	// Registry struct "EntityDetails": one entity as entity.get and entity.update report it.
	struct EntityDetails
	{
		std::string Id{};
		std::string Name{};
		std::string Path{};
		std::string Parent{}; // the parent's id; empty for a root
		bool Active = true;   // its own flag ("Active" in files)
		bool ActiveInHierarchy = true;
		std::vector<std::string> Tags{};
		std::map<std::string, VariantValue> Components{}; // the selected components' canonical JSON, entity-level ones excluded
		std::vector<EntitySummary> Children{};            // only with children: true
	};

	// entity.get {entity, components?, children?, target?}: `components` is an array of registry names or "all" (absent:
	// "all").
	struct EntityGetParams
	{
		std::string Entity{};
		VariantValue Components{};
		bool Children = false;
		SceneTarget Target = SceneTarget::Edit;
	};

	struct EntityGetResult
	{
		EntityDetails Entity{};
	};

	// entity.update {entity, name?, active?, tags?, components?, removeComponents?, target?}: changes only what is given
	// (MethodContext::HasParam). `components` merges each listed component's fields into the existing component, adding
	// components that are missing (§13.5 "adds missing components"); `removeComponents` removes components (Required ones and
	// ones another component requires are InvalidState). Removals apply before additions. All or nothing.
	struct EntityUpdateParams
	{
		std::string Entity{};
		std::string Name{};
		bool Active = true;
		std::vector<std::string> Tags{}; // replaces the tag list
		std::map<std::string, VariantValue> Components{};
		std::vector<std::string> RemoveComponents{};
		SceneTarget Target = SceneTarget::Edit;
	};

	struct EntityUpdateResult
	{
		EntityDetails Entity{}; // after the update, with every component
		uint32_t UndoIndex = 0;
	};

	// entity.destroy {entities, target?}: destroys each entity with its subtree, in one command. An entity listed twice, or
	// inside another listed entity's subtree, is destroyed once.
	struct EntityDestroyParams
	{
		std::vector<std::string> Entities{};
		SceneTarget Target = SceneTarget::Edit;
	};

	struct EntityDestroyResult
	{
		std::vector<std::string> Destroyed{}; // ids of every destroyed entity, subtrees included, sorted
		uint32_t UndoIndex = 0;
	};

	// entity.duplicate {entities, target?}: copies each entity with its subtree through the serializer, with fresh ids and
	// internal EntityRefs remapped to the copies, inserted right after the original among its siblings and named like it.
	struct EntityDuplicateParams
	{
		std::vector<std::string> Entities{};
		SceneTarget Target = SceneTarget::Edit;
	};

	struct EntityDuplicateResult
	{
		std::vector<EntitySummary> Entities{}; // the copies' roots, in the order of `entities`
		uint32_t UndoIndex = 0;
	};

	// entity.reparent {entity, parent, index?, keepWorld?, target?}: Scene::SetParent; `parent` "" moves the entity to the
	// root list. Cycles are InvalidArgument (§5.2).
	struct EntityReparentParams
	{
		std::string Entity{};
		std::string Parent{};
		uint32_t Index = 0; // only when given
		bool KeepWorld = true;
		SceneTarget Target = SceneTarget::Edit;
	};

	struct EntityReparentResult
	{
		EntitySummary Entity{}; // with its new path
		uint32_t UndoIndex = 0;
	};

	// Registry struct "EntityWorldBounds": one entity's world AABB (entity.bounds).
	struct EntityWorldBounds
	{
		EntitySummary Entity{};
		bool HasBounds = false;   // false when neither it nor (with includeDescendants) a descendant has a mesh
		std::vector<float> Min{}; // [x, y, z] in metres; empty without bounds
		std::vector<float> Max{};
		std::vector<float> Center{};
		std::vector<float> Size{};
	};

	// entity.bounds {entities, includeDescendants?, target?} (§13.5 "world AABBs", §13.7 layout feedback): for each entity in
	// the order given, ComputeEntityWorldBounds (Scene/EntityBounds.h): the world AABB of its MeshRenderer mesh and, with
	// includeDescendants (default true), of every descendant's, skipping effectively disabled entities. A missing mesh counts
	// with the placeholder cube's bounds (and records ASSET_MISSING). Reads the play scene while playing unless target says
	// otherwise. Available in the Runtime subset (§13.5; its declarations move to Engine/Automation/Methods in M7).
	struct EntityBoundsParams
	{
		std::vector<std::string> Entities{};
		bool IncludeDescendants = true;
		SceneTarget Target = SceneTarget::Edit;
	};

	struct EntityBoundsResult
	{
		std::vector<EntityWorldBounds> Bounds{};
	};

	namespace Automation {

		// entity.create. Errors: NotFound (parent, or an unknown component with suggestions); InvalidState (a Requires or
		// Excludes violation, a second unique-per-scene component); InvalidArgument (an entity-level or hidden component);
		// Validation (field values).
		[[nodiscard]] Result<EntityCreateResult> EntityCreate(EditorMethodContext& context, const EntityCreateParams& params);
		// entity.get. Errors: NotFound; InvalidArgument for a malformed components value.
		[[nodiscard]] Result<EntityGetResult> EntityGet(EditorMethodContext& context, const EntityGetParams& params);
		// entity.update. Errors: as entity.create, plus InvalidState for a removal that is not allowed.
		[[nodiscard]] Result<EntityUpdateResult> EntityUpdate(EditorMethodContext& context, const EntityUpdateParams& params);
		// entity.destroy. Errors: NotFound for any reference (nothing destroyed then); InvalidArgument for an empty list.
		[[nodiscard]] Result<EntityDestroyResult> EntityDestroy(EditorMethodContext& context, const EntityDestroyParams& params);
		// entity.duplicate. Errors: as entity.destroy; InvalidState when a unique-per-scene component would be duplicated.
		[[nodiscard]] Result<EntityDuplicateResult> EntityDuplicate(EditorMethodContext& context, const EntityDuplicateParams& params);
		// entity.reparent. Errors: NotFound; InvalidArgument for a cycle or an unrepresentable keepWorld transform.
		[[nodiscard]] Result<EntityReparentResult> EntityReparent(EditorMethodContext& context, const EntityReparentParams& params);
		// entity.bounds. Errors: NotFound for any reference (nothing reported then); InvalidArgument for an empty list.
		[[nodiscard]] Result<EntityBoundsResult> EntityBounds(EditorMethodContext& context, const EntityBoundsParams& params);

	}

	// Registers EntityDetails and the params and result structs above (EntityWorldBounds and entity.bounds' params and
	// result from M6).
	void RegisterEntityMethodTypes(TypeRegistry& registry);

	// Registers the seven methods: all are tools and AllowedInBatch (every effect goes through EditorContext::Execute); every
	// one but entity.get and entity.bounds mutates and supports dry runs; entity.get and entity.bounds are available in the
	// Runtime (M7; their declarations move to Engine/Automation/Methods then, ADR 0008 decision 26).
	void RegisterEntityMethods(MethodRegistry& methods);

}
