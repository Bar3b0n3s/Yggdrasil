#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// The render static set (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3; Architecture §8.2, §8.3): the meshes of a
// scene's effectively static entities (Scene/Components/StaticComponent.h), kept by the scene's RenderStaticCache between
// frames and handed to every view through RenderSnapshot::StaticSet, so extraction does not visit them and the renderer
// prepares them once per cell version. A snapshot header (Scripts/ModuleRules.json's SnapshotHeaders): Core, Asset, glm and
// the standard library only, so Scene includes it without reaching the GPU.

namespace Engine {

	// The side of a static cell, in metres: an engine constant, not a project setting (ADR 0061 decision 4).
	inline constexpr float StaticCellSize = 32.0f;

	// The pick ids of static records start here (2^31): a static record's id is StaticPickIdBase plus its cell's slot and its
	// index in the cell (the encoding is the renderer's, W43.2's record), so the records never change with the frame's
	// dynamic items, whose ids stay dense from 1 in snapshot order.
	inline constexpr uint32_t StaticPickIdBase = 0x80000000u;

	// One cell of the static set: the items whose world origin lies in the XZ square Key * StaticCellSize to (Key + 1) *
	// StaticCellSize, in canonical order of their entities. Immutable once published: a change makes a new cell with a new
	// Version (copy on write). Version is unique only within one scene's set and serves change detection within it (the
	// edit scene and its play copy hand out the same versions); a renderer keys what it prepared from a cell by the cell
	// object, the Ref<const RenderStaticCell> it holds while the preparation is cached. The renderer derives the cell's
	// bounds from its items' meshes, so a large item enlarges its cell (a loose grid).
	struct RenderStaticCell
	{
		glm::ivec2 Key = glm::ivec2(0);
		uint64_t Version = 0;
		// Each static entity's MeshRenderer that is Visible with a Mesh, at its current world matrix (no interpolation, so
		// PreviousWorld stays empty: a static record's previous matrix is its matrix), its MaterialParameters resolved; never
		// skinned, morphed, mirrored glass (PlanarReflection) or scrolling (MaterialParameters.UVScroll non-zero): those stay
		// in RenderSnapshot::Meshes. PickId is 0: the renderer assigns static pick ids (StaticPickIdBase).
		std::vector<MeshDrawItem> Items{};
		// The entity of each item, by item index: what a static pick id resolves to.
		std::vector<UUID> PickTable{};
		// Whether any item dissolves (its Parameters.DissolveAmount above 0), computed once per cell, so a renderer that
		// looks up the dissolve noise only while something dissolves scans cells, not items.
		bool Dissolving = false;
	};

	// The static set of a scene: its cells in key order (x, then z), and a Version that moves whenever a cell is added,
	// removed or replaced: unique only within the scene, for change detection within it, as a cell's.
	struct RenderStaticSet
	{
		std::vector<Ref<const RenderStaticCell>> Cells{};
		uint64_t Version = 0;
	};

}
