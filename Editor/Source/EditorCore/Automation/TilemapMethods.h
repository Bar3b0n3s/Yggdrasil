#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

// tilemap.paint (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 26): the edit of a Tilemap asset's cells,
// the one action of the editor's tile brush (EditorCore/Viewport/TilemapPainting.h), so automation paints exactly as the
// editor does (Roadmap rule 5). Conventions as in MethodRegistry.h and AssetMethods.h: "tilemap" is an AssetReference (§7.1),
// the result names it as an AssetSummary, and the edit is one AssetEditCommand executed through EditorContext::Execute: one
// undo step labelled "[agent] Paint Tilemap", the result's undoIndex (0 in a dry run or a batch). Asset edits write through
// (ADR 0010): the .tilemap file is rewritten in its canonical form and reimported; a play session in lockstep (a recording or
// replay included) or a test run sees the change only after it ends, as asset.setProperties' (§7.5 race rule 4). It edits
// an asset, never an open scene, so it takes no scene target (the scene-target inventory's None).
//
// Frozen by the M21 contract (C21.2): the wire format below. The types and the method are written here; stream T3 implements
// the handler and calls the registration functions from RegisterMethods.cpp, together with its C++ round trip, its Python
// test, its scene-target inventory entry, the MCP schema test's proxied tool and the regenerated references and catalogue.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// The most cells one tilemap.paint writes.
	inline constexpr uint32_t MaxTilemapPaintCells = 65536;

	// Registry struct "TilemapPaintCell": one cell to write (TilemapCellEdit, Asset/TilemapData.h).
	struct TilemapPaintCell
	{
		int32_t X = 0;     // the cell's column, x right
		int32_t Y = 0;     // its row, y up
		uint32_t Tile = 0; // the new cell value: 0 erases, k paints the palette's entry k - 1
	};

	// tilemap.paint {tilemap, cells}.
	struct TilemapPaintParams
	{
		std::string Tilemap{};                 // required: a Tilemap asset (AssetReference)
		std::vector<TilemapPaintCell> Cells{}; // required: 1 to MaxTilemapPaintCells cells, written in order (a later one of a cell wins)
	};

	struct TilemapPaintResult
	{
		AssetSummary Tilemap{};
		uint32_t Changed = 0;   // the cells whose value changed (ApplyTilemapCellEdits)
		uint32_t Chunks = 0;    // the tilemap's chunk count after the edit
		uint32_t UndoIndex = 0; // the edit's undo step; 0 in a dry run or a batch, and when nothing changed (nothing is written)
	};

	namespace Automation {

		// tilemap.paint: reads the .tilemap source (TilemapFromText), applies the cells (ApplyTilemapCellEdits) and writes the
		// canonical text as one AssetEditCommand; a call that changes no cell writes nothing and returns undoIndex 0. Errors:
		// asset.setProperties' for the reference (NotFound, InvalidArgument for a built-in or a sub-asset) at /tilemap and
		// InvalidArgument there for an asset that is not a Tilemap; InvalidArgument at /cells for no cell or more than
		// MaxTilemapPaintCells, at /cells/<i>/x or /cells/<i>/y for a cell outside MaxTilemapCellCoordinate and at
		// /cells/<i>/tile for a value above the palette's size (its hint: add the entry to Palette with asset.setProperties);
		// Validation at /cells when the result would hold more than MaxTilemapCells cells; ImportFailed naming the file for a
		// source that does not read (the hint names project.validate); the AssetEditCommand's errors. Nothing is written on
		// error.
		[[nodiscard]] Result<TilemapPaintResult> TilemapPaint(EditorMethodContext& context, const TilemapPaintParams& params);

	}

	// Registers the structs above.
	void RegisterTilemapMethodTypes(TypeRegistry& registry);

	// Registers tilemap.paint: mutating, SupportsDryRun, AllowedInBatch, an MCP tool; not available in the Runtime.
	void RegisterTilemapMethods(MethodRegistry& methods);

}
