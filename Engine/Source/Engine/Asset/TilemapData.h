#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Tilemaps (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 23): the .tilemap file (AssetType::Tilemap,
// format version 1), the reflected TilemapData struct behind it, its cooked payload and the loaded asset. A tilemap is data
// only: a grid of cells in chunks, each cell empty or holding an entry of the tilemap's palette, a SpriteSheet frame with a
// Solid flag. Produced by TilemapImporter (.tilemap files), asset.create and tilemap.paint; drawn by the TilemapRenderer
// (Scene/Components/TilemapRendererComponent.h) through the sprite path and collided by the TilemapCollider
// (Scene/Components/TilemapColliderComponent.h, Asset/TilemapCollision.h).
//
// Conventions, which every consumer follows:
//   - Cells. Cell (x, y), integers, x right and y up (the entity's local +X and +Y, unlike texel space), each in
//     [-MaxTilemapCellCoordinate, MaxTilemapCellCoordinate). Its rectangle is [x CellSize.x, (x + 1) CellSize.x] x
//     [y CellSize.y, (y + 1) CellSize.y] in the local XY plane, so cell (0, 0)'s bottom-left corner is the entity's origin,
//     and its centre is GetTilemapCellCenter's.
//   - Chunks. ChunkSize x ChunkSize cells, ChunkSize a power of two from 1 to MaxTilemapChunkSize, so it divides 2^20
//     and the chunk of any cell within MaxTilemapCellCoordinate holds only such cells: chunk (X, Y) holds the cells
//     [X ChunkSize, (X + 1) ChunkSize) x [Y ChunkSize, (Y + 1) ChunkSize) (GetTilemapChunkCoordinate floors), its X and
//     Y each in [-MaxTilemapCellCoordinate / ChunkSize, MaxTilemapCellCoordinate / ChunkSize). Its Tiles hold ChunkSize^2
//     cell values row by row from its bottom row up, each row left to right: Tiles[(y - Y ChunkSize) ChunkSize +
//     (x - X ChunkSize)]. Chunks are listed sorted by (Y, X), each once, and none is empty: a chunk exists while one of its
//     cells is not, so the file and the cooked form are canonical (tilemap.paint adds a chunk at its first tile and
//     removes it with its last).
//   - Cell values. 0 is an empty cell; k in [1, Palette.size()] is Palette[k - 1]. The palette is identified by position:
//     reordering it repaints the map.
//   - Palette entries. A SpriteSheet and the index of one of its frames (a frame index fits in 16 bits,
//     MaxSpriteSheetFrames), drawn as a sprite of that frame (TilemapRendererComponent.h), and Solid, whether the
//     TilemapCollider collides with the cell. A null Sheet is valid and draws nothing (as a missing sheet); a Frame the
//     sheet lacks is TILEMAP_FRAME_OUT_OF_RANGE at validation and draws nothing.
// The sheets' contents are not part of the tilemap: the importer never reads a sheet, so a tilemap imports and loads
// without them, and a sheet edit changes nothing a tilemap cooks (the dependency graph reimports the tilemap as the sheet's
// dependent, to the same bytes).

namespace Engine {

	class AssetManager;
	class TypeRegistry;

	// The .tilemap document header (§6.9).
	inline constexpr std::string_view TilemapFormatName = "Tilemap";
	inline constexpr uint32_t TilemapFormatVersion = 1;

	// The limits ValidateTilemap enforces, so a tilemap external input controls cannot exhaust memory. ChunkSize is a power
	// of two up to MaxTilemapChunkSize (1, 2, 4, 8, 16, 32 or 64), so it divides 2^20 (the file comment).
	inline constexpr uint32_t DefaultTilemapChunkSize = 16;
	inline constexpr uint32_t MaxTilemapChunkSize = 64;
	inline constexpr uint32_t MaxTilemapPaletteEntries = 65535;      // a cell value fits in 16 bits (the cooked form)
	inline constexpr uint64_t MaxTilemapCells = uint64_t{ 1 } << 22; // stored cells: the chunk count times ChunkSize^2
	inline constexpr int32_t MaxTilemapCellCoordinate = 1 << 20;     // a cell's x and y lie in [-2^20, 2^20)
	inline constexpr float MinTilemapCellSize = 0.001f;              // CellSize's components, in local units
	inline constexpr float MaxTilemapCellSize = 1000.0f;

	// Registry struct "TilemapTile": one palette entry.
	struct TilemapTile
	{
		TypedAssetHandle<AssetType::SpriteSheet> Sheet{}; // null draws nothing
		uint32_t Frame = 0;                               // an index of the sheet's frames, below MaxSpriteSheetFrames
		bool Solid = true;                                // the TilemapCollider collides with the cells holding it

		bool operator==(const TilemapTile&) const = default;
	};

	// Registry struct "TilemapChunk": ChunkSize x ChunkSize cells (see the file comment).
	struct TilemapChunk
	{
		int32_t X = 0; // chunk coordinates: the chunk's first cell is (X ChunkSize, Y ChunkSize)
		int32_t Y = 0;
		std::vector<uint32_t> Tiles{}; // ChunkSize^2 cell values, at most Palette.size() each, not all 0

		bool operator==(const TilemapChunk&) const = default;
	};

	// A tilemap (AssetType::Tilemap), registry struct "Tilemap". Every field is serialized under its member name, in member
	// order, after "Format": "Tilemap" and "Version": 1. Plain data; immutable once loaded (AssetRef<TilemapData>). There is
	// no placeholder tilemap: a consumer whose tilemap is missing or failed reports it and draws or collides nothing.
	struct TilemapData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Tilemap;
		// The cooked payload layout below (CookedHeader::FormatVersion).
		static constexpr uint16_t FormatVersion = 1;

		TilemapData()
			: Asset(StaticType)
		{
		}

		glm::vec2 CellSize = glm::vec2(1.0f);         // local units, each component in [MinTilemapCellSize, MaxTilemapCellSize]
		uint32_t ChunkSize = DefaultTilemapChunkSize; // a power of two from 1 to MaxTilemapChunkSize
		std::vector<TilemapTile> Palette{};           // at most MaxTilemapPaletteEntries
		std::vector<TilemapChunk> Chunks{};           // sorted by (Y, X), unique, none empty
	};

	// Registers the structs "TilemapTile", "TilemapChunk" and "Tilemap" (TilemapData) with the field ranges above and the
	// rules across fields (ValidateTilemap, ChunkSize's power of two among them) as Validate hooks, and Generate hooks that
	// keep the registry suite's random objects valid. RegisterAssetTypes calls it (stream T1), so tilemaps read and write in
	// the editor and in exported games alike.
	void RegisterTilemapTypes(TypeRegistry& registry);

	// Checks every rule of the file comment and the field comments: CellSize's components finite and in [MinTilemapCellSize,
	// MaxTilemapCellSize]; ChunkSize a power of two from 1 to MaxTilemapChunkSize; at most MaxTilemapPaletteEntries palette
	// entries, each Frame below MaxSpriteSheetFrames; the chunks sorted by (Y, X) without duplicates, each holding exactly
	// ChunkSize^2 values of at most the palette's size, not all 0, its X and Y each in [-MaxTilemapCellCoordinate /
	// ChunkSize, MaxTilemapCellCoordinate / ChunkSize) (compared as they are, before any product with ChunkSize, so a fuzzed
	// coordinate never overflows; every cell of such a chunk lies within MaxTilemapCellCoordinate); at most MaxTilemapCells
	// stored cells. Sheets and their frame counts are not checked (the file comment). Pure. Errors: Validation with one
	// issue per violation, each located by its JSON pointer into the .tilemap document ("/CellSize", "/ChunkSize",
	// "/Palette/3/Frame", "/Chunks/2/Tiles/17", "/Chunks/4" for an unsorted, duplicate, empty or out-of-range chunk).
	[[nodiscard]] Status ValidateTilemap(const TilemapData& tilemap);

	// The assets `tilemap` references, without nulls, sorted and unique: its palette's sheets. Its importer's
	// Import.Dependencies (ADR 0028's phase-2 ledger: every new importer fills them, so M19's pruned export keeps the sheets)
	// and its edges in the AssetDependencyGraph.
	[[nodiscard]] std::vector<AssetHandle> GetTilemapDependencies(const TilemapData& tilemap);

	// What a tilemap load reports besides its result: the file's version and its warnings (unknown members) in document order.
	struct TilemapLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics{};
	};

	// The canonical .tilemap document ("Format", "Version", then the registry fields in member order, every field written,
	// §6): CellSize, ChunkSize, Palette and Chunks, each chunk's Tiles one JSON array of ChunkSize^2 integers. Errors:
	// Validation for a value that cannot be written (a non-finite cell size), located. Requires a frozen registry on which
	// RegisterTilemapTypes ran (asserted; InvalidState without asserts).
	[[nodiscard]] Result<Json> TilemapToJson(const TilemapData& tilemap, const TypeRegistry& registry);

	// The canonical text of TilemapToJson: Pretty for .tilemap files.
	[[nodiscard]] Result<std::string> TilemapToText(const TilemapData& tilemap, const TypeRegistry& registry, JsonStyle style = JsonStyle::Pretty);

	// Reads a .tilemap document strictly through the registry (StructInfo::FromJson of "Tilemap": missing keys keep their
	// defaults, unknown keys warn into `report`, or fail with `strictUnknowns`, every value validated), then ValidateTilemap.
	// Errors: Validation (located, every bad field an issue) for a wrong "Format", a version below 1, a wrong JSON type, an
	// out-of-range value or a ValidateTilemap violation; UnsupportedVersion naming both versions for a newer file.
	[[nodiscard]] Result<TilemapData> TilemapFromJson(const Json& document, const TypeRegistry& registry, TilemapLoadReport& report,
		bool strictUnknowns = false);

	// JsonReader::Parse, then TilemapFromJson. Errors: Parse (line and column), and as TilemapFromJson.
	[[nodiscard]] Result<TilemapData> TilemapFromText(std::string_view text, const TypeRegistry& registry, TilemapLoadReport& report,
		bool strictUnknowns = false);

	// The cooked tilemap payload (FormatVersion 1), little-endian:
	//     f32 CellSizeX; f32 CellSizeY; uint32 ChunkSize; uint32 PaletteCount; uint32 ChunkCount;
	//     PaletteCount x { uint64 Sheet (the handle's value; 0 for none); uint32 Frame; uint8 Solid (0 or 1); }
	//     ChunkCount x { int32 X; int32 Y; ChunkSize^2 x uint16 Tile; }
	// Palette and chunks in the tilemap's order, each chunk's cells in its Tiles order. Asserts ValidateTilemap. Pure; identical
	// tilemaps give identical bytes in every configuration (no padding, no pointers, floats as their bits).
	[[nodiscard]] Buffer SerializeTilemapPayload(const TilemapData& tilemap);

	// Reads a payload written by SerializeTilemapPayload, checking every count against its limit and against the bytes left
	// before allocating, then ValidateTilemap. Never asserts on data (fuzzed: "TilemapData: 1,000 seeded mutations of a tilemap
	// file and of its cooked form never crash"). Errors: Parse for truncation, a count above its limit, a Solid byte other
	// than 0 or 1, or trailing bytes; Validation from ValidateTilemap.
	[[nodiscard]] Result<TilemapData> DeserializeTilemapPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `tilemap`. Asserts ValidateTilemap.
	[[nodiscard]] Buffer CookTilemap(const TilemapData& tilemap, uint32_t importerVersion);

	// The tilemap of a cooked artifact (both asset managers' loader for AssetType::Tilemap, RegisterBuiltinLoaders, stream T1).
	// Errors: as ReadCookedArtifact and DeserializeTilemapPayload.
	[[nodiscard]] Result<AssetRef<TilemapData>> LoadCookedTilemap(std::span<const std::byte> cooked);

	// --- Cells ----------------------------------------------------------------------------------------------------------------

	// The chunk holding cell `cell` for chunks of `chunkSize` (>= 1, asserted) cells: each coordinate divided by chunkSize,
	// rounded towards negative infinity (cell (-1, 16) is in chunk (-1, 1) at 16). Pure.
	[[nodiscard]] glm::ivec2 GetTilemapChunkCoordinate(glm::ivec2 cell, uint32_t chunkSize);

	// The centre of cell `cell` in the tilemap's local XY plane: ((x + 0.5) CellSize.x, (y + 0.5) CellSize.y), where a
	// TilemapRenderer draws the cell's tile with its frame's pivot. Pure; the products are IEEE-exact for the cell sizes and
	// coordinates of pixel-art maps (dyadic values).
	[[nodiscard]] glm::vec2 GetTilemapCellCenter(const TilemapData& tilemap, glm::ivec2 cell);

	// The value of cell `cell`: 0 when it is empty or lies in no chunk. Logarithmic in the chunk count.
	[[nodiscard]] uint32_t GetTilemapCell(const TilemapData& tilemap, glm::ivec2 cell);

	// One cell to write: tilemap.paint's {x, y, tile}.
	struct TilemapCellEdit
	{
		glm::ivec2 Cell = glm::ivec2(0);
		uint32_t Tile = 0; // the new cell value: 0 erases, k paints Palette[k - 1]

		bool operator==(const TilemapCellEdit&) const = default;
	};

	// Writes `edits` in their order into `tilemap` (valid, asserted), a later edit of a cell winning: creates the chunk of a
	// cell painted where none is (inserted at its sorted place, the rest of it empty), and removes every chunk the edits
	// leave empty, so the result is canonical and valid: ChunkSize divides 2^20, so the chunk of any cell within
	// MaxTilemapCellCoordinate is one ValidateTilemap accepts, the edge cells (-2^20, -2^20) and (2^20 - 1, 2^20 - 1)
	// included. Returns the number of cells whose value changed (a cell edited twice back to its value counts none).
	// Atomic: on error `tilemap` is unchanged. Errors: InvalidArgument naming the edit's index for a cell outside
	// MaxTilemapCellCoordinate or a Tile above the palette's size; Validation when the result would store more than
	// MaxTilemapCells cells.
	[[nodiscard]] Result<uint32_t> ApplyTilemapCellEdits(TilemapData& tilemap, std::span<const TilemapCellEdit> edits);

	// --- Validation (project.validate) ----------------------------------------------------------------------------------------

	// Warning, not fixable: a palette entry of a Tilemap names a frame index its sprite sheet does not have; the cells holding
	// it draw nothing (TilemapRendererComponent.h). Appended after M21's C21.1 codes in both of ProjectValidator.cpp's tables
	// (ADR 0028's phase-2 ledger), with its Tests/Data/Validation/TILEMAP_FRAME_OUT_OF_RANGE/ fixture and its row in the
	// game-debugging skill's validation-codes page (stream T1).
	inline constexpr std::string_view TilemapFrameOutOfRangeCode = "TILEMAP_FRAME_OUT_OF_RANGE";

	// One palette entry whose frame its sheet lacks.
	struct TilemapPaletteIssue
	{
		uint32_t Entry = 0;      // the index in Palette (the cells holding it have the value Entry + 1)
		AssetHandle Sheet{};     // the entry's sheet
		uint32_t Frame = 0;      // the entry's frame
		uint32_t FrameCount = 0; // the sheet's frame count

		bool operator==(const TilemapPaletteIssue&) const = default;
	};

	// The palette entries of `tilemap` whose Frame is not an index of their sheet's frames, in palette order, each sheet loaded
	// through `assets` (Load, so a sheet that is missing or fails to load is skipped: the validator's ASSET_MISSING and
	// ASSET_IMPORT_FAILED report it, and a null Sheet is no issue). Main thread (the manager).
	[[nodiscard]] std::vector<TilemapPaletteIssue> FindTilemapPaletteIssues(const TilemapData& tilemap, AssetManager& assets);

}
