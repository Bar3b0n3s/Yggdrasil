#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <string>
#include <string_view>
#include <vector>

// The navigation checks of one scene (M30, Docs/Decisions/0045-m30-navigation.md decision 9): what project.validate reports
// about a scene's navigation mesh. The codes are appended after main's in both of ProjectValidator.cpp's code tables in
// landing order (ADR 0042's phase-5 ledger: NAV_MESH_NOT_BAKED then NAV_MESH_STALE, after every other lander's), each with its
// Tests/Data/Validation/<CODE>/ fixture and its row in the game-debugging skill's validation-codes page.

namespace Engine {

	class AssetManager;
	class PhysicsLayerTable;
	class Scene;

	// Warning, not fixable: the scene's Navigation component has no NavMesh, or its NavMesh does not load; every query finds
	// nothing.
	inline constexpr std::string_view NavMeshNotBakedCode = "NAV_MESH_NOT_BAKED";
	// Warning, not fixable: the scene's static colliders, NavModifiers or Navigation settings changed after its navigation mesh
	// was baked (the bake input's hash differs from the one the mesh stores), or its input can no longer be read.
	inline constexpr std::string_view NavMeshStaleCode = "NAV_MESH_STALE";

	// One navigation validation finding.
	struct NavigationSceneIssue
	{
		std::string Code{}; // NavMeshNotBakedCode or NavMeshStaleCode
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{}; // the Navigation component's entity
		std::string Component = "Navigation";
		std::string Field = "NavMesh";
		std::string Message{};
		std::string Hint{};

		bool operator==(const NavigationSceneIssue&) const = default;
	};

	// The navigation checks over `scene`, on the Navigation component the bake reads (the first, in canonical order, on an
	// effectively enabled entity; none without one): NAV_MESH_NOT_BAKED when its NavMesh is null or does not load as a NavMesh
	// through `assets`; otherwise NAV_MESH_STALE when CollectNavigationBakeInput with `layers` fails (an unknown layer, settings
	// out of range) or its HashNavigationBakeInput differs from the mesh's InputHash. Without `layers` (the project's physics
	// layers give no table, which the validator reports itself) the staleness check is skipped. Loads the mesh and the
	// colliders' meshes and heightfields; main thread.
	[[nodiscard]] std::vector<NavigationSceneIssue> FindNavigationSceneIssues(const Scene& scene, AssetManager& assets, const PhysicsLayerTable* layers);

}
