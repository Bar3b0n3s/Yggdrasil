#pragma once

#include "EditorCore/OpenSceneId.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Terrain sculpting and painting (M26, C26.1, C-TERR-EDIT; Docs/Decisions/0037-m26-contract.md decision 23): the one
// implementation of a terrain stroke, which terrain.edit and the editor's sculpt and paint tool (a tool of its own,
// Editor/Tools/TerrainBrush, that SceneViewportPanel only calls; ADR 0028's phase-3 ledger) both run, so a stroke through
// terrain.edit is byte-identical to the same stroke made in the editor (the acceptance): the editor's tool collects the
// cursor's points along the terrain (a ray through the output mapping onto the surface, SampleTerrainHeight) into one stroke
// and queues terrain.edit {points} with them once per mouse release, so EditTerrain runs once for it, exactly as for an
// agent's call.
//   - The brush: each point of the stroke, in order, is one dab of radius Radius (metres in the terrain's local XZ plane)
//     with the falloff w(d) = (1 - (d / Radius)^2)^2 inside it (0 outside) at a sample's or texel's local distance d,
//     scaled by Strength; IEEE-exact operations only, every sample rounded to nearest (ties to even) and clamped, so the
//     result is identical in every configuration.
//   - Sculpt (the heightfield): Raise adds and Lower subtracts Strength * w metres; Flatten moves towards Height metres by
//     Strength * w of the difference; Smooth moves towards the mean of the sample's 3 x 3 neighbourhood (clamped at the edges)
//     by Strength * w, every dab reading the samples as the previous dab left them. Metres convert to samples by
//     MaxHeightfieldSample / HeightScale.
//   - Paint (the splat map, RGBA8, red layer 0 to alpha layer 3): the stroke's Layer's channel moves towards 255 by
//     Strength * w of its distance, and the other three are scaled so the four keep their sum at 255 (a texel whose others
//     are all 0 takes the rest), so the weights stay normalized. A terrain without a SplatMap gets one at its first paint:
//     `<heightfield's folder>/<heightfield's stem>.<entity id>.splat.png` (the Terrain's entity id, 16 lowercase hexadecimal
//     digits, so terrains that share a heightfield never share a splat map; when that file exists already, as after a scene
//     was copied with its ids, the first free `<stem>.<entity id>-<n>.splat.png` from n = 2: an existing file is never
//     overwritten), the heightfield's Resolution - 1 texels a side, layer 0 everywhere, imported as a data texture (Usage
//     Linear and Compression None in its .meta, so it samples as RGBA8Unorm: TerrainComponent.h) and assigned to the
//     Terrain in the same edit.
//   - The edit: the heightfield's `.heightfield` source (WriteHeightfieldSource) or the splat map's PNG source (an RGBA8 PNG,
//     Graphics/Image's encoder, deterministic) is rewritten and reimported, and one undo step ("Sculpt Terrain" or "Paint
//     Terrain") restores the previous bytes (and the Terrain's SplatMap when the paint made it) and reimports. No run-time
//     edits: only edit scenes (a kept limit, Backlog §6).
//
// Frozen by the M26 contract C26.1; stream T-E implements it.

namespace Engine {

	class EditorContext;

	// The tools of terrain.edit and the editor's terrain tool (registry enum "TerrainTool", TerrainMethods.h).
	enum class TerrainTool : uint8_t
	{
		Raise,
		Lower,
		Smooth,
		Flatten,
		Paint
	};

	// One stroke (see the file comment).
	struct TerrainStroke
	{
		TerrainTool Tool = TerrainTool::Raise;
		std::vector<glm::vec2> Points{}; // the dabs' centres in the terrain's local XZ plane, metres, in order; 1 to 4,096
		float Radius = 2.0f;             // metres, (0, 1000]
		float Strength = 0.5f;           // [0, 1]
		float Height = 0.0f;             // Flatten's target, metres above the terrain's origin, [0, HeightScale]
		uint32_t Layer = 0;              // Paint's layer, below MaxTerrainLayers
	};

	// An RGBA8 splat map's texels, row 0 first (the PNG's top row: the terrain's +Z rows from 0).
	struct TerrainSplatImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Texels{}; // Width * Height * 4
	};

	// Sculpts `heightfield` by `stroke` (Raise, Lower, Smooth or Flatten; asserted) for a terrain of `extent`. Pure.
	void ApplyTerrainSculpt(HeightfieldData& heightfield, const TerrainExtent& extent, const TerrainStroke& stroke);

	// Paints `splat` by `stroke` (Paint; asserted) for a terrain of `extent`: texel (x, y) of a W x H map covers the local
	// point ((x + 0.5) Size.x / W, (y + 0.5) Size.y / H). Pure.
	void ApplyTerrainPaint(TerrainSplatImage& splat, const TerrainExtent& extent, const TerrainStroke& stroke);

	// What EditTerrain wrote.
	struct TerrainEditResult
	{
		std::string Path{};           // the rewritten source, project-relative
		bool CreatedSplatMap = false; // the paint made the terrain's splat map
		uint32_t Changed = 0;         // samples or texels whose value changed
	};

	// Applies `stroke` to the Terrain of `entity` in the open edit scene `scene` and records its undo step (see the file
	// comment). Errors: NotFound for an unknown scene or entity, or a heightfield (or splat map) whose source is not in the
	// project; InvalidArgument for an entity without a Terrain, a stroke outside its ranges, or a Paint of a splat map not
	// imported as Usage Linear and Compression None (TERRAIN_SPLAT_NOT_RAW's case, the hint naming asset.setImportSettings);
	// InvalidState for the play scene;
	// those of the write (Io) and the reimport (ImportFailed), after which the source is restored.
	[[nodiscard]] Result<TerrainEditResult> EditTerrain(EditorContext& editor, OpenSceneId scene, UUID entity, const TerrainStroke& stroke);

}
