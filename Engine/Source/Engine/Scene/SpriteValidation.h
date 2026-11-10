#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The sprite checks of one scene (M21, C21.1; Docs/Decisions/0032-m21-contract.md decision 12): what project.validate
// reports about a scene's SpriteRenderers and SpriteAnimators, and from C21.2 its TilemapRenderers' sorting layers. The codes
// are appended after PREFAB_CYCLE in both of ProjectValidator.cpp's code tables (ADR 0028's phase-2 ledger), before the
// PHYSICS2D_* codes, each with its Tests/Data/Validation/<CODE>/ fixture and its row in the game-debugging skill; the
// renderer and the SpriteAnimation phase behave as each code says without reporting it again.
//
// Frozen by the M21 contract (C21.1); streams A (the sorting layer and the frame) and B (the clip) implement it. C21.2
// (decision 30, stream T1) extends the sorting-layer check to TilemapRenderer. M21's milestone review adds
// SPRITE_FRAME_OUTSIDE_TEXTURE, a check of sprite sheet and tilemap files rather than of a scene, after M21's last code.

namespace Engine {

	class AssetManager;
	class Scene;
	struct SpriteSheetData;

	// Warning, not fixable: a SpriteRenderer, or a TilemapRenderer (C21.2, stream T1), names a SortingLayer that the
	// project's SortingLayers.Layers does not declare; it draws in the first layer.
	inline constexpr std::string_view SpriteUnknownSortingLayerCode = "SPRITE_UNKNOWN_SORTING_LAYER";
	// Warning, not fixable: a SpriteRenderer's Frame is not an index of its sheet's frames (a sheet that loads); it draws
	// nothing.
	inline constexpr std::string_view SpriteFrameOutOfRangeCode = "SPRITE_FRAME_OUT_OF_RANGE";
	// Warning, not fixable: a SpriteAnimator's Clip is not empty and names no clip of its SpriteRenderer's sheet (a sheet
	// that loads, or no sheet); it plays nothing.
	inline constexpr std::string_view SpriteUnknownClipCode = "SPRITE_UNKNOWN_CLIP";

	// One sprite validation finding.
	struct SpriteSceneIssue
	{
		std::string Code{}; // one of the codes above
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{};
		std::string Component{}; // "SpriteRenderer", "SpriteAnimator" or "TilemapRenderer"
		std::string Field{};     // "SortingLayer", "Frame" or "Clip"
		std::string Message{};
		std::string Hint{};

		bool operator==(const SpriteSceneIssue&) const = default;
	};

	// The sprite checks over `scene` (effectively enabled entities, canonical order; an entity's SpriteRenderer, then its
	// SpriteAnimator, then its TilemapRenderer) with the project's sorting layers `sortingLayers` (SortingLayers.Layers);
	// sheets load through `assets` (null: the frame and clip checks are skipped). Pure function of its inputs otherwise;
	// main thread.
	[[nodiscard]] std::vector<SpriteSceneIssue> FindSpriteSceneIssues(const Scene& scene, AssetManager* assets, std::span<const std::string> sortingLayers);

	// Warning, not fixable (M21's milestone review, closing Backlog §6's frame past its texture): a sprite sheet's frame whose
	// texel rectangle extends past its texture's size, or a tilemap's palette entry that names such a frame; the sprite or
	// the tiles sample the texture's edge texels there (clamped). project.validate reports it on the sheet's file (Field
	// "Frames[<i>].Rect") and on each tilemap file whose palette names the frame (Field "Palette[<i>].Frame"), once per frame
	// or entry. Appended after TILEMAP_FRAME_OUT_OF_RANGE, the end of M21's block, in both of ProjectValidator.cpp's code
	// tables, with its Tests/Data/Validation/SPRITE_FRAME_OUTSIDE_TEXTURE/ fixture and its row in the game-debugging skill's
	// validation-codes page.
	inline constexpr std::string_view SpriteFrameOutsideTextureCode = "SPRITE_FRAME_OUTSIDE_TEXTURE";

	// One frame of a sprite sheet that extends past its texture.
	struct SpriteFrameTextureIssue
	{
		uint32_t Frame = 0;  // the frame's index in the sheet
		uint64_t Right = 0;  // X + Width: one past the frame's last texel column
		uint64_t Bottom = 0; // Y + Height: one past its last texel row
		uint32_t TextureWidth = 0;
		uint32_t TextureHeight = 0;

		bool operator==(const SpriteFrameTextureIssue&) const = default;
	};

	// The frames of `sheet` whose Rect extends past its texture (X + Width above the texture's width, or Y + Height above its
	// height), in frame order. The texture's size is its imported data's recorded width and height (TextureData::Width and
	// Height), loaded through `assets` as the renderer loads it: the editor's asset manager holds every imported texture's
	// data since its import (a refresh imports every asset), so nothing is decoded again. A null Texture, or one that is
	// missing, fails to load or is not a Texture, has no size to check: the validator's asset codes report those. Main thread
	// (the manager).
	[[nodiscard]] std::vector<SpriteFrameTextureIssue> FindSpriteFramesOutsideTexture(const SpriteSheetData& sheet, AssetManager& assets);

}
