#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// The sprite checks of one scene (M21, C21.1; Docs/Decisions/0032-m21-contract.md decision 12): what project.validate
// reports about a scene's SpriteRenderers and SpriteAnimators. The codes are appended after PREFAB_CYCLE in both of
// ProjectValidator.cpp's code tables (ADR 0028's phase-2 ledger), before the PHYSICS2D_* codes, each with its
// Tests/Data/Validation/<CODE>/ fixture and its row in the game-debugging skill; the renderer and the SpriteAnimation phase
// behave as each code says without reporting it again.
//
// Frozen by the M21 contract (C21.1); streams A (the sorting layer and the frame) and B (the clip) implement it.

namespace Engine {

	class AssetManager;
	class Scene;

	// Warning, not fixable: a SpriteRenderer names a SortingLayer that the project's SortingLayers.Layers does not declare;
	// it draws in the first layer.
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
		std::string Component{}; // "SpriteRenderer" or "SpriteAnimator"
		std::string Field{};     // "SortingLayer", "Frame" or "Clip"
		std::string Message{};
		std::string Hint{};

		bool operator==(const SpriteSceneIssue&) const = default;
	};

	// The sprite checks over `scene` (effectively enabled entities, canonical order) with the project's sorting layers
	// `sortingLayers` (SortingLayers.Layers); sheets load through `assets` (null: the frame and clip checks are skipped).
	// Pure function of its inputs otherwise; main thread.
	[[nodiscard]] std::vector<SpriteSceneIssue> FindSpriteSceneIssues(const Scene& scene, AssetManager* assets, std::span<const std::string> sortingLayers);

}
