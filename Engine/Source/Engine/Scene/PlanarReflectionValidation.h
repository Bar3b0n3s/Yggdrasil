#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// The planar reflection checks of one scene (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3): what
// project.validate reports about a scene's PlanarReflections. The code is appended to both of ProjectValidator.cpp's code
// tables after M41's ANIMATION_ATTACHMENT_INVALID (M42's ledger), with its Tests/Data/Validation/RENDER_REFLECTION_INVALID/
// fixture and its row in the game-debugging skill's validation-codes page; the renderer draws such a reflection as the code
// says without reporting it again.

namespace Engine {

	class AssetManager;
	class Scene;

	// Warning, not fixable: a PlanarReflection whose entity draws no mesh (no MeshRenderer, or one without a Mesh), so nothing
	// shows its reflection; or whose mesh's bounds reach farther than PlanarReflectionMaxMeshDistance from its plane, so the
	// glass is not on the plane (usually a Normal along the wrong axis: the finding suggests the local axis along which the
	// bounds are thinnest and its Offset); or whose Normal is zero.
	inline constexpr std::string_view RenderReflectionInvalidCode = "RENDER_REFLECTION_INVALID";
	// The planar reflection codes, an array of their own (M42's ledger; M36's and M37's pattern).
	inline constexpr std::array<std::string_view, 1> PlanarReflectionDiagnosticCodes = { RenderReflectionInvalidCode };

	// How far, in world metres (after the entity's scale), the corners of a PlanarReflection's mesh bounds may lie from its
	// plane: a glass's thickness, or the slack of a slightly bent one.
	inline constexpr double PlanarReflectionMaxMeshDistance = 0.05;

	// One planar reflection validation finding.
	struct PlanarReflectionSceneIssue
	{
		std::string Code{}; // RenderReflectionInvalidCode
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{};           // the PlanarReflection's entity
		std::string Component{}; // "PlanarReflection" or "MeshRenderer"
		std::string Field{};     // "Normal" or "Mesh"
		std::string Message{};
		std::string Hint{};

		bool operator==(const PlanarReflectionSceneIssue&) const = default;
	};

	// The planar reflection checks over `scene` (effectively enabled entities with a PlanarReflection, canonical order; at
	// most one finding an entity):
	//   - no MeshRenderer, or one whose Mesh is null (MeshRenderer.Mesh);
	//   - a zero Normal (PlanarReflection.Normal; a file can hold one that no write path accepts);
	//   - a mesh that loads through `assets` as a Mesh whose bounds' corners, at the entity's world matrix, lie farther than
	//     PlanarReflectionMaxMeshDistance from the plane (PlanarReflection.Normal), with a hint naming the local axis along
	//     which the bounds are thinnest in the world and the Offset that puts the plane through their centre on it. A mesh
	//     that does not load is not checked (ASSET_MISSING and ASSET_TYPE_MISMATCH report it); a hidden MeshRenderer (Visible
	//     false) is checked as a shown one.
	// Pure function of its inputs otherwise; main thread.
	[[nodiscard]] std::vector<PlanarReflectionSceneIssue> FindPlanarReflectionIssues(const Scene& scene, AssetManager& assets);

}
