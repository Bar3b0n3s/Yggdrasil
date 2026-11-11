#include "EditorPCH.h"
#include "EditorCore/Automation/TerrainMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// terrain.edit and terrain.importHeightmap (Docs/Decisions/0037-m26-contract.md decisions 20 and 23). The wire types and the
// registration are the contract's, written but not called; the handlers are contract stubs, implemented by streams T-E
// (terrain.edit) and T-A (terrain.importHeightmap).

namespace Engine {

	namespace Automation {

		Result<TerrainEditMethodResult> TerrainEditMethod(EditorMethodContext& /*context*/, const TerrainEditParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "M26 terrain contract (Docs/Decisions/0037-m26-contract.md decision 23, stream T-E)");
		}

		Result<TerrainImportHeightmapResult> TerrainImportHeightmapMethod(EditorMethodContext& /*context*/, const TerrainImportHeightmapParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "M26 terrain contract (Docs/Decisions/0037-m26-contract.md decision 20, stream T-A)");
		}

	}

	void RegisterTerrainMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<TerrainTool>("TerrainTool", "What a terrain stroke does.")
			.Entry(TerrainTool::Raise, "Raise", "Raises the surface under the brush by Strength times the falloff, in metres.")
			.Entry(TerrainTool::Lower, "Lower", "Lowers the surface under the brush by Strength times the falloff, in metres.")
			.Entry(TerrainTool::Smooth, "Smooth", "Moves each sample under the brush towards the mean of its neighbours.")
			.Entry(TerrainTool::Flatten, "Flatten", "Moves the surface under the brush towards Height.")
			.Entry(TerrainTool::Paint, "Paint", "Paints Layer's weight into the splat map under the brush, keeping the weights normalized.");
		registry.Struct<TerrainEditParams>("TerrainEditParams", "The params of terrain.edit.")
			.Field("entity", &TerrainEditParams::Entity, "The entity whose Terrain to edit: its id, a unique id prefix or its path.")
			.Field("tool", &TerrainEditParams::Tool, "The stroke's tool.")
			.Field("points", &TerrainEditParams::Points,
				"The brush's centres in world space, 1 to 4,096 in stroke order; their height above the terrain is ignored.", { .Unit = "m" })
			.Field("radius", &TerrainEditParams::Radius, "The brush's radius in metres in the terrain's local space.", { .Min = 0.001, .Max = 1000.0, .Unit = "m" })
			.Field("strength", &TerrainEditParams::Strength, "The stroke's strength, from 0 to 1.", { .Min = 0.0, .Max = 1.0 })
			.Field("height", &TerrainEditParams::Height, "Flatten's target height above the terrain's origin, in metres.", { .Min = 0.0, .Unit = "m" })
			.Field("layer", &TerrainEditParams::Layer, "Paint's layer, 0 to 3 (the splat map's red, green, blue or alpha).", { .Min = 0.0, .Max = 3.0 });
		registry.Struct<TerrainEditMethodResult>("TerrainEditMethodResult", "What terrain.edit wrote.")
			.Field("entity", &TerrainEditMethodResult::Entity, "The edited terrain's entity.")
			.Field("path", &TerrainEditMethodResult::Path, "The rewritten source, project-relative: the heightfield's, or the splat map's PNG.")
			.Field("createdSplatMap", &TerrainEditMethodResult::CreatedSplatMap, "Whether the paint made and assigned the terrain's splat map.")
			.Field("changed", &TerrainEditMethodResult::Changed, "The samples or texels whose value changed.");
		registry.Struct<TerrainImportHeightmapParams>("TerrainImportHeightmapParams", "The params of terrain.importHeightmap.")
			.Field("source", &TerrainImportHeightmapParams::Source, "The heightmap: a 16-bit .png, or a .raw or .r16 of little-endian 16-bit samples.")
			.Field("path", &TerrainImportHeightmapParams::Path, "The .heightfield to write, project-relative below Assets/.");
		registry.Struct<TerrainImportHeightmapResult>("TerrainImportHeightmapResult", "The heightfield terrain.importHeightmap wrote.")
			.Field("asset", &TerrainImportHeightmapResult::Asset, "The imported heightfield.")
			.Field("resolution", &TerrainImportHeightmapResult::Resolution, "Its samples along each side.");
	}

	void RegisterTerrainMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "terrain.edit",
				.Description = "Sculpts (Raise, Lower, Smooth, Flatten) or paints (Paint) an edit scene's Terrain with one brush stroke, exactly as "
							   "the editor's terrain tools do: rewrites the heightfield's or splat map's source and records one undo step.",
				.RequiredParams = { "entity", "points" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsSceneTarget = true,
				.Examples = { { .Description = "Raise a hill at the terrain's middle.",
					.Params = { { "entity", "/Terrain" }, { "tool", "Raise" }, { "points", Json::array({ Json::array({ 32, 0, 32 }) }) }, { "radius", 8 },
						{ "strength", 0.5 } } } },
			},
			&Automation::TerrainEditMethod);
		methods.Add(
			{
				.Name = "terrain.importHeightmap",
				.Description = "Converts a 16-bit PNG or RAW heightmap (at most 1025 x 1025 samples) into a .heightfield asset that a Terrain can use.",
				.RequiredParams = { "source", "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.Examples = { { .Description = "Import a heightmap beside the scenes.",
					.Params = { { "source", "Heightmaps/Valley.png" }, { "path", "Assets/Terrain/Valley.heightfield" } } } },
			},
			&Automation::TerrainImportHeightmapMethod);
	}

}
