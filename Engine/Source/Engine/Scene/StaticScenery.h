#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// The static scenery checks of one scene (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3): what project.validate
// reports about a scene's Static components. The code is appended to both of ProjectValidator.cpp's code tables after M42's
// RENDER_REFLECTION_INVALID, the first of phase 8's codes (ADR 0060's ledger), with its
// Tests/Data/Validation/SCENE_STATIC_MOVES/ fixture and its row in the game-debugging skill's validation-codes page.

namespace Engine {

	class Scene;

	// Warning, not fixable: an effectively static entity (HierarchyStaticTag, StaticComponent.h) that a component moves every
	// tick: a Dynamic or Kinematic RigidBody, a CharacterController, a BoneAttachment or an Animator whose RootMotion is Apply.
	// Moving it is correct, but it rebuilds its static cell each tick it moves and renders at its current pose, without
	// interpolation; a moving part belongs outside the static subtree, or under a Static with Scope NotStatic.
	inline constexpr std::string_view SceneStaticMovesCode = "SCENE_STATIC_MOVES";
	// The static scenery codes, an array of their own (phase 8's ledger; M36's, M37's and M42's pattern).
	inline constexpr std::array<std::string_view, 1> StaticSceneryDiagnosticCodes = { SceneStaticMovesCode };

	// One static scenery validation finding.
	struct StaticSceneryIssue
	{
		std::string Code{}; // SceneStaticMovesCode
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{};           // the static entity
		std::string Component{}; // the component that moves it: "RigidBody", "CharacterController", "BoneAttachment" or "Animator"
		std::string Field{};     // "Type" for a RigidBody, "RootMotion" for an Animator, empty otherwise
		std::string Message{};
		std::string Hint{};

		bool operator==(const StaticSceneryIssue&) const = default;
	};

	// The static scenery checks over `scene`: every effectively static entity, enabled or not (an enabled one moves at once),
	// in canonical order, with at most one finding an entity, naming the first of the moving components in the order above
	// (M44 appends VehicleWheel to the list at its merge). Pure function of the scene; main thread.
	[[nodiscard]] std::vector<StaticSceneryIssue> FindStaticSceneryIssues(const Scene& scene);

}
