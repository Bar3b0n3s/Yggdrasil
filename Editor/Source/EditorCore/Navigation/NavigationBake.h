#pragma once

#include "EditorCore/OpenSceneId.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <string>
#include <vector>

// navigation.bake's work (M30, Docs/Decisions/0045-m30-navigation.md decision 8): bakes an open edit scene's navigation mesh
// and commits it beside the scene. The editor's Bake Navigation action (a menu item in a file of its own,
// Editor/Panels/NavigationBakeAction, that EditorLayer only calls; ADR 0042's phase-5 ledger) and the method call this one
// function, so automation bakes exactly as the editor does (Roadmap rule 5). CPU only: it works in an editor without a GPU
// device (--renderer none).
//   - The scene must be saved (its file names the bake's file): Assets/Navigation/ followed by the scene's project path below
//     Assets/ with the extension .navmesh in place of its own (Assets/Scenes/Arena.scene bakes into
//     Assets/Navigation/Scenes/Arena.navmesh), so two scenes of one name in different folders never share a file.
//   - The bake input (Scene/NavigationBakeInput.h) is collected with the project's physics layers through the editor's asset
//     manager, and the mesh is built (EditorCore/Navigation/NavMeshBuilder.h) before anything is written, so an input or a
//     build that fails writes nothing.
//   - The file is written only where its bytes changed, a .meta is written for a new source (the importer's defaults, a
//     handle kept across bakes: a rewritten source keeps its .meta), and it is imported when either changed; then the
//     Navigation component's NavMesh is set to the asset in one undoable edit of the scene ("Bake Navigation"). A rebake of an
//     unchanged scene writes nothing, changes no component and records no undo step (the acceptance: byte-identical
//     rebakes), so an undo after it takes back the edit before it.
// Main thread (the scene and the asset manager).

namespace Engine {

	class EditorContext;

	// One agent type's baked mesh, by its counts.
	struct NavigationBakeAgentSummary
	{
		std::string Name{};
		uint32_t Polygons = 0;
		uint32_t Vertices = 0;
	};

	struct NavigationBakeResult
	{
		UUID Entity{};      // the entity whose Navigation component the bake assigned
		std::string Path{}; // the written source, project-relative ("Assets/Navigation/Scenes/Arena.navmesh")
		AssetHandle Asset{};
		bool Changed = false;                             // whether the file's bytes changed (false for a rebake of an unchanged scene)
		uint64_t InputHash = 0;                           // the bake input's hash, which the mesh stores (NAV_MESH_STALE compares it)
		std::vector<NavigationBakeAgentSummary> Agents{}; // in the component's order
		std::vector<UUID> SkippedBodies{};                // owners of bodies whose shape could not be described
	};

	// Bakes the open edit scene `scene` (see the file comment). Errors: InvalidState without a project, for the play scene and
	// for a scene never saved (the hint names scene.save); NotFound for an unknown open scene and for a scene without a
	// Navigation component (CollectNavigationBakeInput's); Validation for the project's physics layers, the component's
	// settings and layer names and the builder's limits (CollectNavigationBakeInput's and BuildNavMesh's), before anything is
	// written; OutOfMemory from the builder; those of the write (Io, or AlreadyExists naming a file where the bake's directory
	// goes), after which nothing is written and no component changes; those of the import (ImportFailed), after which the
	// written file stays and no component changes.
	[[nodiscard]] Result<NavigationBakeResult> BakeNavigation(EditorContext& editor, OpenSceneId scene);

}
