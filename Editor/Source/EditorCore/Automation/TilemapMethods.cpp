#include "EditorPCH.h"
#include "EditorCore/Automation/TilemapMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <format>
#include <utility>
#include <vector>

// tilemap.paint (Docs/Decisions/0032-m21-contract.md decision 26): the wire format and the registration are the contract's,
// the handler stream T3's; RegisterMethods.cpp calls the registration functions.

namespace Engine {

	namespace {

		// The source file of the Tilemap asset `handle`. Errors at /tilemap: InvalidArgument for a built-in or an asset of
		// another type (a sub-asset, a dependency file); NotFound for a handle the registry does not hold.
		Result<VfsPath> FindTilemapSource(const EditorAssetManager& assets, AssetHandle handle)
		{
			if (assets.GetBuiltins().Find(handle) != nullptr || IsBuiltinAssetHandle(handle))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/tilemap",
					std::format("'{}' is a built-in asset, not a tilemap", assets.GetReferencePath(handle)), "built-in assets cannot change"));
			}
			const AssetType type = assets.GetAssetType(handle);
			const AssetRecord* record = assets.GetRegistry().Find(handle);
			if (type != AssetType::Tilemap && (type != AssetType::None || record != nullptr))
			{
				// A main asset or sub-asset of another type, or a dependency file (registered, without a type of its own).
				const std::string what = type == AssetType::None ? std::string("a dependency file") : std::format("a {}", AssetTypeToString(type));
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/tilemap",
					std::format("'{}' is {}, not a Tilemap", assets.GetReferencePath(handle), what), "pass a .tilemap asset; asset.create {type: \"Tilemap\"} makes one"));
			}
			if (record == nullptr)
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/tilemap", std::format("no asset has the handle {}", handle.ToString())));
			return record->SourcePath;
		}

		// The cells' count and coordinates, which need no tilemap: InvalidArgument at /cells or /cells/<i>/x|y.
		Status CheckPaintCells(const TilemapPaintParams& params)
		{
			if (params.Cells.empty() || params.Cells.size() > MaxTilemapPaintCells)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/cells",
					std::format("tilemap.paint writes 1 to {} cells, not {}", MaxTilemapPaintCells, params.Cells.size()),
					"split a larger edit into several calls"));
			}
			const auto outside = [](int32_t coordinate)
			{
				return coordinate < -MaxTilemapCellCoordinate || coordinate >= MaxTilemapCellCoordinate;
			};
			for (size_t index = 0; index < params.Cells.size(); ++index)
			{
				const TilemapPaintCell& cell = params.Cells[index];
				for (const auto& [axis, coordinate] : { std::pair{ "x", cell.X }, std::pair{ "y", cell.Y } })
				{
					if (outside(coordinate))
					{
						return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/cells/{}/{}", index, axis),
							std::format("cell {}'s {} is {}, outside [{}, {})", index, axis, coordinate, -MaxTilemapCellCoordinate, MaxTilemapCellCoordinate)));
					}
				}
			}
			return {};
		}

	}

	namespace Automation {

		Result<TilemapPaintResult> TilemapPaint(EditorMethodContext& context, const TilemapPaintParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Tilemap, "/tilemap"));
			const EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const VfsPath source, FindTilemapSource(assets, handle));
			ENGINE_TRY(CheckPaintCells(params));

			Result<std::string> text = editor.GetVfs().ReadText(source);
			if (!text)
				return std::unexpected(Utils::ToEditorFileError(std::move(text).error()));
			const TypeRegistry& types = editor.GetTypeRegistry();
			const std::string relative = Utils::ToProjectRelative(source);
			// Strict, as its importer reads it: a member the file does not define would not survive the rewrite.
			TilemapLoadReport report;
			Result<TilemapData> read = TilemapFromText(*text, types, report, true);
			if (!read)
			{
				// The reader's line, column and pointer, in the file it read.
				ErrorLocation location = read.error().GetLocation();
				location.File = relative;
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' cannot be read as a tilemap: {}", relative, read.error().GetMessageText()))
						.WithLocation(std::move(location))
						.WithIssues(read.error().GetIssues())
						.WithHint("project.validate reports what is wrong with the file; fix the file, then paint again"));
			}
			TilemapData tilemap = std::move(*read);

			std::vector<TilemapCellEdit> edits;
			edits.reserve(params.Cells.size());
			for (size_t index = 0; index < params.Cells.size(); ++index)
			{
				const TilemapPaintCell& cell = params.Cells[index];
				if (cell.Tile > tilemap.Palette.size())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/cells/{}/tile", index),
						std::format("tile {} is above the palette's {} entries of '{}' (0 erases, k paints entry k - 1)", cell.Tile, tilemap.Palette.size(), relative),
						"add the entry to Palette with asset.setProperties first"));
				}
				edits.push_back({ .Cell = glm::ivec2(cell.X, cell.Y), .Tile = cell.Tile });
			}
			Result<uint32_t> changed = ApplyTilemapCellEdits(tilemap, edits);
			if (!changed)
				return std::unexpected(Utils::LocateAtParam(changed.error(), "/cells"));

			TilemapPaintResult result;
			result.Tilemap = Utils::MakeAssetSummary(assets, handle);
			result.Changed = *changed;
			result.Chunks = static_cast<uint32_t>(tilemap.Chunks.size());
			if (*changed == 0)
				return result;
			ENGINE_TRY_ASSIGN(const std::string painted, TilemapToText(tilemap, types));
			// One AssetEditCommand: one undo step, written through EditorContext's write path (provenance, the dry-run overlay,
			// the reimport, deferred while a lockstep, recording, replay or test session runs).
			ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command, AssetEditCommand::CreateForWrite(editor, source, AsBytes(painted), "Paint Tilemap"));
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
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
