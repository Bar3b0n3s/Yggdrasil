#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/ContactBuffer.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Physics/Private/PhysicsCollisionGroups.h"
#include "Engine/Physics/Private/PhysicsContactListener.h"
#include "Engine/Physics/Private/PhysicsEdgeRemoval.h"
#include "Engine/Physics/Private/PhysicsLayerFilters.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <cstdint>
#include <map>
#include <vector>

// The Jolt side of PhysicsWorld and PhysicsShape (Architecture §9.1), shared by the Physics module's .cpp files: the world
// (PhysicsWorld.cpp), its queries (PhysicsWorldQueries.cpp), joints (PhysicsWorldJoints.cpp), vehicles
// (PhysicsWorldVehicles.cpp) and CharacterController, which needs the world's
// JPH::PhysicsSystem and temporary allocator. Private to the Physics module (§3 rule 3: Jolt types never reach a public
// header), so it changes without the contract owner's review (Docs/Decisions/0014-m11-decisions.md decision 17).

namespace Engine {

	// One PhysicsShape: the Jolt shape and its colliders.
	struct PhysicsShape::State
	{
		JPH::RefConst<JPH::Shape> Shape{};
		// ColliderShapeDescription::UserData of each collider, in description order. Several make Shape a
		// JPH::StaticCompoundShape whose sub-shape user data they are; one makes Shape that collider's own shape.
		std::vector<uint32_t> ColliderUserData{};
		// Whether a collider is a triangle mesh or (M26) a height field (a Dynamic body cannot use the shape,
		// PHYSICS_NONCONVEX_DYNAMIC; two such bodies never meet, PhysicsCollisionGroups).
		bool HasMesh = false;
	};

	namespace Detail {

		// One live joint of a PhysicsWorld (M37, PhysicsWorldJoints.cpp). Jolt's body 1 is the connected body (the world's
		// fixed body for none) and its body 2 the joint's body, so a hinge's angle and a slider's position are the joint's
		// body's motion relative to what it is joined to.
		struct PhysicsJointRecord
		{
			JPH::Ref<JPH::TwoBodyConstraint> Constraint{};
			PhysicsJointType Type = PhysicsJointType::Hinge;
			JPH::BodyID Body{};
			// Invalid for the world.
			JPH::BodyID ConnectedBody{};
			PhysicsJointSettings Settings{};
		};

		// One live vehicle of a PhysicsWorld (M44, PhysicsWorldVehicles.cpp). The world owns Jolt's reference-counted objects,
		// each embedded so the system's references never delete it. Members are destroyed in reverse order: the constraint
		// first (the world removed it from the system and its step listeners), then the tester it referenced, then the wheel
		// settings its wheels referenced.
		struct PhysicsVehicleRecord
		{
			std::vector<Scope<JPH::WheelSettingsWV>> Wheels{};
			Scope<JPH::VehicleCollisionTester> Tester{};
			Scope<JPH::VehicleConstraint> Constraint{};
			JPH::BodyID Body{};
		};

	}

	// One PhysicsWorld. Members are destroyed in reverse order: the system (and the bodies still in it) first, then the
	// allocator, the listener and the filters it uses.
	struct PhysicsWorld::State
	{
		PhysicsWorldSpecification Specification{};
		ContactBuffer Contacts;
		// Jolt's three layer filters over Specification.Layers (§9.2 "Layers").
		Detail::PhysicsBroadPhaseLayerMap BroadPhaseLayers{};
		Detail::PhysicsObjectVsBroadPhaseLayerFilter ObjectVsBroadPhaseLayerFilter{ Specification.Layers };
		Detail::PhysicsObjectLayerPairFilter ObjectLayerPairFilter{ Specification.Layers };
		// BodyDescription::CollisionGroup and the mesh-pair rule.
		Detail::PhysicsCollisionGroups CollisionGroups;
		// The bodies CreateBody made, by Jolt body index (grown on demand, at most Specification.Limits.MaxBodies): every body
		// of the world, character inner bodies included.
		std::vector<Detail::PhysicsBodyRecord> Bodies{};
		Detail::PhysicsStepContacts StepContacts;
		Detail::PhysicsContactListener ContactListener{ Contacts, Bodies, StepContacts };
		// The last step's distinct body pairs and sub-shape contacts in contact (GetStats).
		uint32_t LastStepBodyPairs = 0;
		uint32_t LastStepContacts = 0;
		// PHYSICS_LIMIT_EXCEEDED warnings already logged (once per world and limit).
		bool HasWarnedBodies = false;
		bool HasWarnedBodyPairs = false;
		bool HasWarnedContactConstraints = false;
		// --- Joints (M37, PhysicsWorldJoints.cpp) ---------------------------------------------------------------------
		// The live joints by JointHandle value (ordered, so every walk over them is in creation order), the next handle
		// value, the body pairs they keep apart (read by the body-pair hook and the filter during a step; the system and its
		// hook are destroyed first) and the duration of the last step's collision steps (their forces' divisor).
		std::map<uint32_t, Detail::PhysicsJointRecord> Joints{};
		uint32_t NextJoint = 1;
		Detail::PhysicsJointedPairs JointedPairs{};
		Detail::PhysicsJointedPairFilter JointedPairFilter{ JointedPairs };
		float CollisionStepDelta = 0.0f;
		// --- Vehicles (M44, PhysicsWorldVehicles.cpp) ------------------------------------------------------------------
		// The live vehicles by VehicleHandle value (ordered, so every walk over them is in creation order) and the next handle
		// value. Destroyed with the state, after the world's destructor removed them from the system.
		std::map<uint32_t, Detail::PhysicsVehicleRecord> Vehicles{};
		uint32_t NextVehicle = 1;
		// A TempAllocatorImpl of Specification.Limits.TempAllocatorBytes (§9.1, 16 MB by default) that falls back to the heap
		// when a step needs more, so no content can exhaust it (JPH::TempAllocatorImpl aborts the process when it is full).
		Scope<JPH::TempAllocator> TempAllocator;
		Scope<JPH::PhysicsSystem> System;

		// --- Queries and character contacts --------------------------------------------------------------------------
		// The collider index (ColliderShapeDescription::UserData) of the sub-shape `subShape` (a JPH::SubShapeID value) of a
		// body whose shape is `shape` (§9.2 "Collider identity in compounds", PhysicsShape.h): Detail::ResolveColliderIndex
		// (the rule the contact listener uses too) with the shape's first collider as the single collider. Used by the
		// queries (PhysicsWorldQueries.cpp) and by the character's contact records (CharacterController.cpp); defined in
		// PhysicsWorldQueries.cpp. Any thread: reads only the immutable shape.
		[[nodiscard]] static uint32_t ResolveColliderUserData(const PhysicsShape& shape, uint32_t subShape);

		// --- Joints (M37; defined in PhysicsWorldJoints.cpp) ------------------------------------------------------------
		// Destroys every joint of `body` (DestroyBody calls it before it removes the body), waking the bodies they joined.
		void DestroyJointsOf(const JPH::BodyID& body);
		// Tells every joint of `body` that its centre of mass moved by `deltaCenterOfMass` in body space (SetShape).
		void NotifyJointsShapeChanged(const JPH::BodyID& body, JPH::Vec3Arg deltaCenterOfMass);
		// Removes every joint from the system without waking anything (the world's destructor, before its bodies).
		void DestroyAllJoints();

		// --- Vehicles (M44; defined in PhysicsWorldVehicles.cpp) ---------------------------------------------------------
		// Removes a vehicle's step listener and constraint from the system (its record is erased by the caller).
		void RemoveVehicle(Detail::PhysicsVehicleRecord& record);
		// Destroys the vehicle of `body` (DestroyBody calls it before the body's joints and the body), waking nothing.
		void DestroyVehiclesOf(const JPH::BodyID& body);
		// Removes every vehicle from the system without waking anything (the world's destructor, before its joints).
		void DestroyAllVehicles();
	};

}
