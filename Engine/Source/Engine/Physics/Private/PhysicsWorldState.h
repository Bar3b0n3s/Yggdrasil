#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/ContactBuffer.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsWorld.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cstdint>
#include <vector>

// The Jolt side of PhysicsWorld and PhysicsShape (Architecture §9.1), shared by the Physics module's .cpp files: the world
// (PhysicsWorld.cpp, stream A), its queries (PhysicsWorldQueries.cpp, stream C) and CharacterController (stream C), which
// needs the world's JPH::PhysicsSystem and temporary allocator. Private to the Physics module (§3 rule 3: Jolt types never
// reach a public header).
//
// The M11 contract's initial layout (Docs/Decisions/0014-m11-decisions.md decision 17): private, so it changes without the
// contract owner's review. Stream A owns it; stream C adds what the queries and characters need in the marked section.

namespace Engine {

	// One PhysicsShape: the Jolt shape and the user data of its colliders.
	struct PhysicsShape::State
	{
		JPH::RefConst<JPH::Shape> Shape{};
		// ColliderShapeDescription::UserData of each collider, in description order.
		std::vector<uint32_t> ColliderUserData{};
		// Whether a collider is a triangle mesh (a Dynamic body cannot use the shape, PHYSICS_NONCONVEX_DYNAMIC).
		bool HasMesh = false;
	};

	// One PhysicsWorld. Members are destroyed in reverse order: the system before the allocator it uses.
	struct PhysicsWorld::State
	{
		PhysicsWorldSpecification Specification{};
		ContactBuffer Contacts;
		Scope<JPH::TempAllocatorImpl> TempAllocator;
		// Stream A adds here the three layer filters over Specification.Layers (Physics/Private/), the ContactListener over
		// Contacts, and the per-body data the world keeps (shape references, sensor flags, layers).
		Scope<JPH::PhysicsSystem> System;

		// --- Stream C: queries and characters ---------------------------------------------------------------------------
	};

}
