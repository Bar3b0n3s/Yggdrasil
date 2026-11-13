#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <string>
#include <string_view>
#include <vector>

// The terrain checks of one scene (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 9): what project.validate
// reports about a scene's Terrains. The codes are appended after PROBE_NOT_BAKED in both of ProjectValidator.cpp's code
// tables (ADR 0028's phase-3 ledger: M26's after M24's at its merge), each with its Tests/Data/Validation/<CODE>/ fixture
// and its row in the game-debugging skill's validation-codes page; the terrain's drawing and its collider behave as each
// code says without reporting it again. Stream T-A.

namespace Engine {

	class AssetManager;
	class Scene;

	// Warning, not fixable: a Terrain has no Heightfield, or its Heightfield does not load as one; it draws nothing, and its
	// TerrainCollider makes no body.
	inline constexpr std::string_view TerrainNoHeightfieldCode = "TERRAIN_NO_HEIGHTFIELD";
	// Warning, not fixable: a Terrain's SplatMap is a Texture that is not imported as a data texture (the Texture importer's
	// Usage Linear and Compression None, TerrainComponent.h): its weights are decoded as sRGB colour or blurred by block
	// compression, and terrain.edit cannot paint it.
	inline constexpr std::string_view TerrainSplatNotRawCode = "TERRAIN_SPLAT_NOT_RAW";

	// One terrain validation finding.
	struct TerrainSceneIssue
	{
		std::string Code{}; // one of the codes above
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{};           // the Terrain's entity
		std::string Component{}; // "Terrain"
		std::string Field{};     // "Heightfield" or "SplatMap"
		std::string Message{};
		std::string Hint{};

		bool operator==(const TerrainSceneIssue&) const = default;
	};

	// The terrain checks over `scene` (effectively enabled entities with a Terrain, canonical order; an entity's
	// TERRAIN_NO_HEIGHTFIELD before its TERRAIN_SPLAT_NOT_RAW):
	//   - TERRAIN_NO_HEIGHTFIELD for a null Heightfield, or one that does not load through `assets` as a Heightfield (a missing
	//     or mistyped asset is also the asset checks' ASSET_MISSING or ASSET_TYPE_MISMATCH).
	//   - TERRAIN_SPLAT_NOT_RAW for a SplatMap that is a registered Texture whose .meta is not the Texture importer's with Usage
	//     Linear and Compression None (a glTF's texture sub-asset included; a .meta without Compression reads as Auto). A null
	//     SplatMap, one that is not a Texture (ASSET_MISSING, ASSET_TYPE_MISMATCH) and a built-in texture (procedural, never
	//     imported) are not checked.
	// Pure function of its inputs otherwise; main thread.
	[[nodiscard]] std::vector<TerrainSceneIssue> FindTerrainSceneIssues(const Scene& scene, AssetManager& assets);

}
