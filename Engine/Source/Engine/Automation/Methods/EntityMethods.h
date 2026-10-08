#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

// The entity domain's reads (Architecture §13.5, §13.7): entity.get and entity.bounds, shared by the Editor and the
// Runtime (§13.5 "Runtime subset"; moved here from EditorCore with M7, unchanged in name and wire format,
// Docs/Decisions/0012-m7-decisions.md decision 12), with EntityDetails, which the editor's entity.update reports too
// (EditorCore/Automation/EntityMethods.h, through MakeEntityDetails). Entity references follow §13.4 (16 hex digits, a
// unique prefix of at least 6, or a path; AutomationMethodContext::ResolveEntity). Conventions as in MethodRegistry.h.

namespace Engine {

	class AutomationMethodContext;
	class ComponentInfo;
	class ConstEntity;
	class MethodRegistry;
	class TypeRegistry;

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
	// the order given, ComputeEntityWorldBounds (Scene/EntityBounds.h) through the host's asset manager
	// (AutomationMethodContext::GetAssets, idle first): the world AABB of its MeshRenderer mesh and, with includeDescendants
	// (default true), of every descendant's, skipping effectively disabled entities. A missing mesh counts with the
	// placeholder cube's bounds (and records ASSET_MISSING). Reads the play scene while playing unless target says otherwise.
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

		// entity.get. Errors: NotFound; InvalidArgument for a malformed components value; those of the target.
		[[nodiscard]] Result<EntityGetResult> EntityGet(AutomationMethodContext& context, const EntityGetParams& params);
		// entity.bounds. Errors: NotFound for any reference (nothing reported then); InvalidArgument for an empty list; those
		// of the target; Unsupported on a host without an asset manager (AutomationMethodContext::GetAssets).
		[[nodiscard]] Result<EntityBoundsResult> EntityBounds(AutomationMethodContext& context, const EntityBoundsParams& params);

		// `entity` (valid) as entity.get and entity.update report it: its members, the JSON of the `selection` components
		// it has (nullopt: every component results report, Utils::GetEntityComponents) and, with `children`, its children's
		// summaries. Errors: those of ComponentAccess::GetComponentJson.
		[[nodiscard]] Result<EntityDetails> MakeEntityDetails(const AutomationMethodContext& context, ConstEntity entity,
			const std::optional<std::vector<const ComponentInfo*>>& selection, bool children);

	}

	// Registers EntityDetails, EntityWorldBounds and the params and result structs above.
	void RegisterEntityMethodTypes(TypeRegistry& registry);

	// Registers entity.get and entity.bounds: tools (§13.8 entity_get, entity_bounds), AllowedInBatch, available in the
	// Runtime; neither mutates.
	void RegisterEntityMethods(MethodRegistry& methods);

}
