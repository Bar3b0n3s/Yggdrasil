#pragma once

#include "EditorCore/Terrain/TerrainEditing.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// terrain.edit and terrain.importHeightmap (M26, C26.1, C-TERR, C-TERR-EDIT; Docs/Decisions/0037-m26-contract.md decisions
// 20 and 23): the editor's terrain tools as methods (Roadmap rule 5).
//   - terrain.edit is the sculpt and paint tools' one action (EditorCore/Terrain/TerrainEditing.h's EditTerrain, which the
//     tools call with the stroke they collected), so a stroke through it is byte-identical to the same stroke made in the
//     editor. It rewrites an asset source and records one undo step: Mutates, no dry run (a stroke's result is its files),
//     not a batch op (it writes files directly); it addresses an open edit scene's entity, so it takes M20's scene target
//     (the scene-target inventory's relation "the target scene's entity"). Stream T-E.
//   - terrain.importHeightmap converts a 16-bit PNG or RAW heightmap into a `.heightfield` source (ConvertHeightmap,
//     AssetPipeline/Importers/HeightfieldImporter.h) and imports it: the terrain inspector's Import Heightmap action. It
//     writes a project file: Mutates, no dry run, not a batch op; it addresses no scene (the inventory's relation "none").
//     Stream T-A.
// Neither is available in the Runtime (no run-time edits, a kept limit). Frozen by the M26 contract C26.1: the wire format
// below. The types and the methods are written here; streams T-E and T-A implement the handlers (each its own) and call
// the registration functions from RegisterMethods.cpp (T-A at W26.1's integration, T-E's handler joining at W26.2's), with
// their C++ round trips, Python tests (Tests/Automation/test_terrain.py), inventory entries and the regenerated references.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// terrain.edit {entity, tool, points, radius?, strength?, height?, layer?} (and "scene", the reserved scene target).
	struct TerrainEditParams
	{
		std::string Entity{}; // required: an entity of the target scene holding a Terrain (an EntityRef)
		TerrainTool Tool = TerrainTool::Raise;
		std::vector<glm::vec3> Points{}; // required: the dabs' centres in world space, 1 to 4,096; each is taken to the terrain's
										 // local space and its local Y ignored, so a point picked on the surface works as is
		float Radius = 2.0f;             // metres in the terrain's local space, (0, 1000]
		float Strength = 0.5f;           // [0, 1]
		float Height = 0.0f;             // Flatten's target, metres above the terrain's origin
		uint32_t Layer = 0;              // Paint's layer, 0 to 3
	};

	struct TerrainEditMethodResult
	{
		EntitySummary Entity{};
		std::string Path{};           // the rewritten source, project-relative
		bool CreatedSplatMap = false; // the paint made the terrain's splat map
		uint32_t Changed = 0;         // samples or texels whose value changed
	};

	// terrain.importHeightmap {source, path}.
	struct TerrainImportHeightmapParams
	{
		std::string Source{}; // required: the heightmap, a project-relative or absolute path to a 16-bit .png or a .raw or .r16
		std::string Path{};   // required: the `.heightfield` to write, project-relative below Assets/ (an existing one is replaced)
	};

	struct TerrainImportHeightmapResult
	{
		AssetSummary Asset{};
		uint32_t Resolution = 0; // samples a side
	};

	namespace Automation {

		// terrain.edit: EditTerrain on the target scene's `entity` with the params as a TerrainStroke. Errors: those of
		// EditTerrain, located at the param they concern (/entity, /points, /radius, /strength, /height, /layer); InvalidState
		// for the play scene as the target.
		[[nodiscard]] Result<TerrainEditMethodResult> TerrainEditMethod(EditorMethodContext& context, const TerrainEditParams& params);

		// terrain.importHeightmap: ConvertHeightmap of `source` (its kind by extension: .png, or .raw and .r16), written as
		// `path` and imported. Errors: NotFound at /source for a missing file; InvalidArgument at /source for another extension
		// and ConvertHeightmap's InvalidArgument and Parse, at /path for a path outside Assets/ or not ending in .heightfield;
		// AlreadyExists at /path when the path is registered as an asset of another type (a hand-edited .meta), nothing
		// written; Io for the write; ImportFailed for the import.
		[[nodiscard]] Result<TerrainImportHeightmapResult> TerrainImportHeightmapMethod(EditorMethodContext& context,
			const TerrainImportHeightmapParams& params);

	}

	// Registers the structs above and the registry enum TerrainTool.
	void RegisterTerrainMethodTypes(TypeRegistry& registry);

	// Registers terrain.edit (mutating, no dry run, not a batch op, SupportsSceneTarget, an MCP tool) and
	// terrain.importHeightmap (mutating, no dry run, not a batch op, an MCP tool); neither in the Runtime.
	void RegisterTerrainMethods(MethodRegistry& methods);

}
