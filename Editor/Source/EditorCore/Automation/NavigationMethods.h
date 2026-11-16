#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

// navigation.bake (M30, Docs/Decisions/0045-m30-navigation.md decision 8): the editor's Bake Navigation action as a method
// (EditorCore/Navigation/NavigationBake.h's BakeNavigation, the action's one call), so automation bakes exactly as the editor
// does (Roadmap rule 5), as lighting.bake does. It writes a file and changes the scene's Navigation component through one
// undoable edit, so it Mutates; it writes files directly, so it is no batch op and takes no dry run. It addresses an open
// edit scene, so it takes M20's scene target (SupportsSceneTarget; the scene-target inventory's relation "the target scene's
// Navigation"), and refuses the play scene. CPU only: an editor started with --renderer none bakes too.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// One agent type's baked mesh in the result.
	struct NavigationBakeAgent
	{
		std::string Name{};
		uint32_t Polygons = 0;
		uint32_t Vertices = 0;
	};

	struct NavigationBakeMethodResult
	{
		EntitySummary Entity{}; // the entity whose Navigation component now references the mesh
		std::string Path{};     // the written source, project-relative
		AssetSummary Asset{};
		bool Changed = false;    // whether the file's bytes changed
		std::string InputHash{}; // the bake input's XXH64, 16 lowercase hex digits
		std::vector<NavigationBakeAgent> Agents{};
		std::vector<EntitySummary> SkippedBodies{}; // bodies whose shape could not be described (PHYSICS_INVALID_SHAPE)
	};

	namespace Automation {

		// navigation.bake {} (and "scene", the reserved scene target: the open edit scene to bake, the active one without it):
		// BakeNavigation on the target scene. Errors: those of BakeNavigation (InvalidState for an unsaved scene or the play
		// scene, with their hints; NotFound for a scene without a Navigation component; Validation for the component's
		// settings, an unknown layer or the builder's limits, before anything is written; OutOfMemory, Io and ImportFailed from
		// the bake).
		[[nodiscard]] Result<NavigationBakeMethodResult> NavigationBakeMethod(EditorMethodContext& context, const NoParams& params);

	}

	// Registers the structs above.
	void RegisterNavigationMethodTypes(TypeRegistry& registry);

	// Registers navigation.bake: mutating, no dry run, not a batch op, SupportsSceneTarget, an MCP tool with a 600 s timeout;
	// not available in the Runtime.
	void RegisterNavigationMethods(MethodRegistry& methods);

}
