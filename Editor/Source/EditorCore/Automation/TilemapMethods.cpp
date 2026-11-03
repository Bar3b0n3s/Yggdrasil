#include "EditorPCH.h"
#include "EditorCore/Automation/TilemapMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// tilemap.paint (Docs/Decisions/0032-m21-contract.md decision 26): the wire format and the registration are the contract's;
// stream T3 implements the handler and calls the registration functions from RegisterMethods.cpp.

namespace Engine {

	namespace Automation {

		Result<TilemapPaintResult> TilemapPaint(EditorMethodContext& /*context*/, const TilemapPaintParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "tilemap.paint (Docs/Decisions/0032-m21-contract.md decision 26, stream T3)");
		}

	}

	void RegisterTilemapMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<TilemapPaintCell>("TilemapPaintCell", "One tilemap cell to write.")
			.Field("x", &TilemapPaintCell::X, "The cell's column: x grows to the right, cell 0's left edge at the tilemap entity's origin.")
			.Field("y", &TilemapPaintCell::Y, "The cell's row: y grows upwards, cell 0's bottom edge at the tilemap entity's origin.")
			.Field("tile", &TilemapPaintCell::Tile, "The new cell value: 0 erases the cell, k paints the tilemap's palette entry k - 1.");
		registry.Struct<TilemapPaintParams>("TilemapPaintParams", "The params of tilemap.paint.")
			.Field("tilemap", &TilemapPaintParams::Tilemap, "The Tilemap asset: its id, its \"Assets/...\" path or another asset reference.")
			.Field("cells", &TilemapPaintParams::Cells,
				"The cells to write, 1 to 65536 of them, in order: a later entry for the same cell wins.");
		registry.Struct<TilemapPaintResult>("TilemapPaintResult", "The outcome of a tilemap.paint.")
			.Field("tilemap", &TilemapPaintResult::Tilemap, "The tilemap painted.")
			.Field("changed", &TilemapPaintResult::Changed, "How many cells changed value; 0 writes nothing.")
			.Field("chunks", &TilemapPaintResult::Chunks, "How many chunks the tilemap holds after the edit (a chunk exists while one of its cells is not empty).")
			.Field("undoIndex", &TilemapPaintResult::UndoIndex, "The edit's undo step; 0 in a dry run, in a batch or when nothing changed.");
	}

	void RegisterTilemapMethods(MethodRegistry& methods)
	{
		const Json example = { { "tilemap", "Assets/Tilemaps/Level1.tilemap" },
			{ "cells", Json::array({ { { "x", 0 }, { "y", 0 }, { "tile", 1 } }, { { "x", 1 }, { "y", 0 }, { "tile", 1 } }, { { "x", 2 }, { "y", 0 }, { "tile", 0 } } }) } };
		methods.Add(
			{
				.Name = "tilemap.paint",
				.Description = "Writes cells of a Tilemap asset (0 erases a cell, k paints the palette's entry k - 1) and saves it, as one undoable "
							   "command; the cells' chunks are created and removed as needed. The editor's tile brush paints through it. A running "
							   "lockstep, recording, replay or test session sees the change only after it ends.",
				.RequiredParams = { "tilemap", "cells" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Paint two ground tiles and erase a third.", .Params = example } },
			},
			&Automation::TilemapPaint);
	}

}
