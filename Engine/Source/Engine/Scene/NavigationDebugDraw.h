#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstddef>

// The navigation view (M30, Docs/Decisions/0045-m30-navigation.md decision 12; World Tree's brief 1, "a debug view of the
// mesh in the scene view and in viewport.screenshot captures"): the baked navigation mesh of a scene as the edges of its
// polygons, so an agent sees what was baked. A view that shows it (RenderViewFlags::Navigation: the screenshot annotation
// "navMesh", the editor's viewport option) gets the lines through AppendRenderAnnotations (Scene/RenderAnnotations.h), which
// appends them to the snapshot's AnnotationDraw after the colliders, as debug lines drawn by the Overlays declaration's two
// DebugLines pipelines. Built from the scene's components and the mesh asset, so the edit scene and a play scene show the
// same mesh: the one the play session's queries read.

namespace Engine {

	class AssetManager;
	class DebugDrawList;
	class Scene;

	// How far above its vertices a navigation mesh's edge is drawn (metres), so the floor it lies on does not hide it.
	inline constexpr float NavigationDebugLift = 0.05f;

	// The colour of agent type `agent`'s edges (linear RGBA): cyan, orange, magenta and yellow for the four agent types a
	// mesh may hold; a border edge (no polygon across it) at full opacity, an edge between two polygons at 40 %.
	[[nodiscard]] glm::vec4 GetNavigationDebugColor(size_t agent, bool border);

	// Appends the edges of every agent type's polygons of the NavMesh of `scene`'s first Navigation component (canonical
	// order) on an effectively enabled entity, as debug lines (duration 0, DepthMode Tested) with the colour's alpha
	// multiplied by `alpha` (the snapshot's, in [0, 1]): one line per edge, an edge shared by two polygons drawn once (from
	// the polygon of the lower index), each end NavigationDebugLift above its vertex, in agent then polygon then edge order.
	// Appends nothing for a scene without such a component, without a NavMesh, or whose mesh does not load
	// (NAV_MESH_NOT_BAKED reports it). Returns the number of lines appended (the list drops those past its MaxCommands,
	// counting them). Main thread.
	size_t AppendNavigationDebugDraw(const Scene& scene, AssetManager& assets, float alpha, DebugDrawList& debug);

}
