#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"

#include <array>
#include <string_view>
#include <vector>

// The 2D physics checks of one scene (M21, C21.1; Docs/Decisions/0032-m21-contract.md decision 12): what project.validate
// reports for a scene's 2D bodies and colliders, and what the play session's Physics2DSystem reports at run time from the same
// composition, so edit-time and run-time diagnostics agree (as Scene/PhysicsValidation.h for the 3D world). The 2D world
// shares the 3D world's codes where the rule is the same (PHYSICS_MIXED_TRIGGER, PHYSICS_DYNAMIC_TRIGGER,
// PHYSICS_INVALID_SHAPE, PHYSICS_UNKNOWN_LAYER, PHYSICS_NONUNIFORM_SCALE, PHYSICS_DYNAMIC_UNDER_MOVING_PARENT,
// PHYSICS_LIMIT_EXCEEDED, with the 2D component named in the diagnostic), and adds the two PHYSICS2D_* codes below, appended
// after PREFAB_CYCLE in both of ProjectValidator.cpp's code tables (ADR 0028's phase-2 ledger), each with its
// Tests/Data/Validation/<CODE>/ fixture and its row in the game-debugging skill.
//
// Frozen by the M21 contract (C21.1); stream P2 implements it.

namespace Engine {

	class AssetManager;
	class Scene;

	// Error, not fixable: an entity holds a 2D physics component (RigidBody2D, BoxCollider2D, CircleCollider2D,
	// PolygonCollider2D, and from C21.2 TilemapCollider) and a 3D one (RigidBody, BoxCollider, SphereCollider,
	// CapsuleCollider, MeshCollider, CharacterController). An entity belongs to one world: it keeps its 3D body and its 2D
	// components make no 2D body.
	inline constexpr std::string_view Physics2DMixedWorldsCode = "PHYSICS2D_MIXED_WORLDS";
	// Warning, not fixable: a 2D body's owner has a world rotation that is not a rotation about Z alone (a swing about X or Y
	// beyond PhysicsUnitRotationTolerance). The body takes the rotation's twist about Z, and its write-back drops the swing.
	inline constexpr std::string_view Physics2DOffPlaneCode = "PHYSICS2D_OFF_PLANE";

	// The 2D codes, in the validator's order.
	inline constexpr std::array<std::string_view, 2> Physics2DDiagnosticCodes = { Physics2DMixedWorldsCode, Physics2DOffPlaneCode };

	// Every 2D physics diagnostic of `scene` (an edit scene or a scratch copy of a scene file), sorted by (entity UUID, code,
	// subject): the 2D composition's (the shared PHYSICS_* codes and PHYSICS2D_OFF_PLANE), PHYSICS_INVALID_SHAPE for every 2D
	// body whose shape MakeBody2DShape or PhysicsShape::Create refuses, and PHYSICS2D_MIXED_WORLDS for every entity that
	// holds both kinds of physics components. Building shapes needs the process's PhysicsEngine (skipped and logged once at
	// Warn without it, as ValidateScenePhysics). Pure function of its inputs otherwise; main thread. M21 (C21.2,
	// Docs/Decisions/0032-m21-contract.md decision 25, stream T2): a TilemapCollider's tilemap loads through `assets` as the
	// play session's Physics2DSystem loads it (Physics2DSystem.h's tilemap paragraph), so its PHYSICS_INVALID_SHAPE cases agree;
	// null `assets` makes every tilemap collider PHYSICS_INVALID_SHAPE, as ValidateScenePhysics does for mesh colliders.
	// project.validate passes the editor's asset manager (stream T2). The default exists only so that the contract tree's
	// callers compile unchanged: stream T2 removes it when it passes the editor's manager from ProjectValidator.cpp, so a
	// caller can never forget the argument and see every tilemap collider reported (ValidateScenePhysics' signature).
	[[nodiscard]] std::vector<PhysicsDiagnostic> ValidateScenePhysics2D(const Scene& scene, const PhysicsLayerTable& layers, AssetManager* assets = nullptr);

}
