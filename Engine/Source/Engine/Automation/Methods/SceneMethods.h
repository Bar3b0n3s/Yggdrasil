#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The scene domain's reads (Architecture §13.5, §13.7): scene.tree, scene.query and scene.get, shared by the Editor and
// the Runtime (§13.5 "Runtime subset"; moved here from EditorCore with M7, unchanged in name and wire format,
// Docs/Decisions/0012-m7-decisions.md decision 12), with SceneSummary, which the editor's scene.new, scene.open and
// scene.save report too (EditorCore/Automation/SceneMethods.h). scene.raycast (M9) joins them. Every read addresses a
// scene through AutomationMethodContext::ResolveTargetScene: the play scene while playing (the Runtime's only scene), else
// the edit scene. Conventions as in MethodRegistry.h.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry struct "SceneSummary": a scene as scene.* results report it (AutomationMethodContext::MakeSceneSummary).
	struct SceneSummary
	{
		std::string Path{}; // project-relative; empty for a scene never saved
		std::string Name{};
		uint32_t Revision = 0;
		bool Dirty = false;
		uint32_t EntityCount = 0;
	};

	// Registry enum "SceneTreeFormat". The JSON format's enumerator is JsonList and its registry name "Json": an enumerator
	// named Json would shadow the alias Engine::Json, which GCC's -Wshadow reports.
	enum class SceneTreeFormat : uint8_t
	{
		Text,    // the token-efficient outline of §13.7
		JsonList // entities as a flat list in canonical order
	};

	// scene.tree {root?, depth?, format?, target?}: the hierarchy below `root` (an EntityRef; empty: every root), at most
	// `depth` levels deep (0: unlimited). Text (§13.7): a header line "<file name>  rev <n>  [dirty  ]<count> entities", then
	// one line per entity with box-drawing indentation, its name, the first 8 hex digits of its id, its components (a short
	// form such as "Camera(Ortho 11)" or "Script(Scripts/Game.luau)" where one exists) and its local translation, and
	// "… <n> children (depth limit)" where the depth limit cuts a subtree.
	struct SceneTreeParams
	{
		std::string Root{};
		uint32_t Depth = 0;
		SceneTreeFormat Format = SceneTreeFormat::Text;
		SceneTarget Target = SceneTarget::Edit;
	};

	// Registry struct "SceneTreeEntry": one entity of the JSON tree.
	struct SceneTreeEntry
	{
		std::string Id{};
		std::string Name{};
		std::string Path{};
		std::string Parent{};                  // the parent's id; empty for a root
		uint32_t Depth = 0;                    // 0 for a root (or for `root` itself)
		bool Active = true;                    // the entity's own active flag
		std::vector<std::string> Components{}; // registry names in registry order, entity-level ones excluded
		uint32_t ChildCount = 0;
		bool ChildrenOmitted = false; // the depth limit cut its children
	};

	struct SceneTreeResult
	{
		SceneSummary Scene{};
		std::string Text{};                     // format "text"
		std::vector<SceneTreeEntry> Entities{}; // format "json"
	};

	// Registry struct "SceneQueryWhere": every given criterion must hold. name: the entity name, '*' matching any run of
	// characters and '?' one character (no wildcard: exact, case-sensitive); tag: the entity has the tag; component: the
	// entity has the component (registry name); path: an entity path whose entity and descendants match.
	struct SceneQueryWhere
	{
		std::string Name{};
		std::string Tag{};
		std::string Component{};
		std::string Path{};
	};

	// scene.query {where, select?, limit?, cursor?, target?}: matching entities in canonical order, paginated. `select` names
	// components whose JSON each result carries ("all" selects every one).
	struct SceneQueryParams
	{
		SceneQueryWhere Where{};
		std::vector<std::string> Select{};
		uint32_t Limit = 100; // 1 to 1000
		std::string Cursor{}; // nextCursor of the previous page; "" for the first
		SceneTarget Target = SceneTarget::Edit;
	};

	// Registry struct "SceneQueryEntity".
	struct SceneQueryEntity
	{
		std::string Id{};
		std::string Name{};
		std::string Path{};
		std::map<std::string, VariantValue> Components{}; // the selected components' canonical JSON, by registry name
	};

	struct SceneQueryResult
	{
		std::vector<SceneQueryEntity> Entities{};
		uint32_t Total = 0;       // matches over every page
		std::string NextCursor{}; // "" when this was the last page
	};

	// scene.get {target?}: the scene's canonical document (§6.2); offloaded when over the result bound (§13.4).
	struct SceneGetParams
	{
		SceneTarget Target = SceneTarget::Edit;
	};

	struct SceneGetResult
	{
		VariantValue Scene{};
	};

	namespace Automation {

		// scene.tree. Errors: those of the target (ResolveTargetScene) and of `root` (ResolveEntity at /root).
		[[nodiscard]] Result<SceneTreeResult> SceneTree(AutomationMethodContext& context, const SceneTreeParams& params);
		// scene.query. Errors: InvalidArgument for an unknown component in where or select, or a malformed cursor; those of
		// the target and of where.path.
		[[nodiscard]] Result<SceneQueryResult> SceneQuery(AutomationMethodContext& context, const SceneQueryParams& params);
		// scene.get. Errors: those of the target; Validation for a scene that cannot be serialized.
		[[nodiscard]] Result<SceneGetResult> SceneGet(AutomationMethodContext& context, const SceneGetParams& params);

	}

	// Registers SceneSummary, SceneTreeFormat and the params and result structs above.
	void RegisterSceneMethodTypes(TypeRegistry& registry);

	// Registers scene.tree, scene.query and scene.get: available in the Runtime and AllowedInBatch; scene.tree and
	// scene.query are tools (§13.8 scene_tree, scene_query).
	void RegisterSceneMethods(MethodRegistry& methods);

}
