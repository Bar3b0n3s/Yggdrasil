#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Automation/Methods/EntityMethods.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The editor's entity.* methods (Architecture §13.5): create, update, destroy, duplicate and reparent. The reads the
// Runtime shares (entity.get, entity.bounds, and EntityDetails, which entity.update reports too) are
// Engine/Automation/Methods/EntityMethods.h's (M7, Docs/Decisions/0012-m7-decisions.md decision 12). Every edit-scene
// mutation is exactly one SceneEditCommand labelled for the agent and reports its undoIndex (§13.4); every mutation
// supports dry runs. Undo labels (CommandHistory adds "[agent] "): "Create Entity '<name>'", "Update Entity '<name>'",
// "Reparent Entity '<name>'", and for the list methods "Destroy Entity '<name>'" / "Duplicate Entity '<name>'" for one
// entity or "Destroy <n> Entities" / "Duplicate <n> Entities" for more. Entity references follow §13.4 (16 hex digits, a
// unique prefix of at least 6, or a path). Component maps use ResolveComponentValue (convention 10 of MethodRegistry.h),
// so "/components/RigidBody/Mas" is an unknown-field InvalidParams with the hint "did you mean 'Mass'?".

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

	namespace Automation {

		// entity.create. Errors: NotFound (parent, or an unknown component with suggestions); InvalidState (a Requires or
		// Excludes violation, a second unique-per-scene component); InvalidArgument (an entity-level or hidden component);
		// Validation (field values).
		[[nodiscard]] Result<EntityCreateResult> EntityCreate(EditorMethodContext& context, const EntityCreateParams& params);
		// entity.update. Errors: as entity.create, plus InvalidState for a removal that is not allowed.
		[[nodiscard]] Result<EntityUpdateResult> EntityUpdate(EditorMethodContext& context, const EntityUpdateParams& params);
		// entity.destroy. Errors: NotFound for any reference (nothing destroyed then); InvalidArgument for an empty list.
		[[nodiscard]] Result<EntityDestroyResult> EntityDestroy(EditorMethodContext& context, const EntityDestroyParams& params);
		// entity.duplicate. Errors: as entity.destroy; InvalidState when a unique-per-scene component would be duplicated.
		[[nodiscard]] Result<EntityDuplicateResult> EntityDuplicate(EditorMethodContext& context, const EntityDuplicateParams& params);
		// entity.reparent. Errors: NotFound; InvalidArgument for a cycle or an unrepresentable keepWorld transform.
		[[nodiscard]] Result<EntityReparentResult> EntityReparent(EditorMethodContext& context, const EntityReparentParams& params);

	}

	// Registers the params and result structs above (after RegisterEntityMethodTypes, whose EntityDetails they use).
	void RegisterEditorEntityMethodTypes(TypeRegistry& registry);

	// Registers the five methods: all are tools, AllowedInBatch (every effect goes through EditorContext::Execute), mutate
	// and support dry runs.
	void RegisterEditorEntityMethods(MethodRegistry& methods);

}
