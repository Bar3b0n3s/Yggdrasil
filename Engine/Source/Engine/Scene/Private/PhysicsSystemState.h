#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/CharacterController.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/PhysicsSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <map>
#include <vector>

// The state of a PhysicsSystem (Architecture §9.2 to §9.6), shared by the system's .cpp files: PhysicsSystem.cpp (stream
// B: composition, sync, events, exits, body control, observability), PhysicsSystemQueries.cpp and
// PhysicsSystemCharacters.cpp (stream C: queries, bounds, characters). Private to Scene.
//
// The M11 contract's initial layout (Docs/Decisions/0014-m11-decisions.md decision 17): private, so it changes without the
// contract owner's review. Stream B owns it; stream C adds what the queries and characters need in the marked section.
// The entry structs are at namespace scope, not nested: Clang with libstdc++ cannot default-construct a nested struct with
// default member initializers inside its enclosing class (ADR 0012 decision 24).

namespace Engine {

	// One created body: what contacts and query hits resolve its handle and sub-shape user data through (§9.2).
	struct PhysicsBodyEntry
	{
		BodyHandle Handle{};
		UUID Owner{};
		PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody;
		// The collider entities in sub-shape user data order (PhysicsBodyPlan::Colliders).
		std::vector<UUID> Colliders{};
		Ref<const PhysicsShape> Shape{};
		// Dynamic bodies: the owner's local Translation and Rotation that PostStep last wrote (or that the body was created
		// from); PreStep teleports the body when they differ bit for bit (§9.3, PhysicsSystem.h).
		glm::vec3 WrittenTranslation = glm::vec3(0.0f);
		glm::quat WrittenRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		// Static bodies: the owner's world matrix the body was created or last teleported at; PreStep teleports the body when
		// it differs bit for bit.
		glm::mat4 PlacedMatrix = glm::mat4(1.0f);
	};

	// One created character (§9.6).
	struct PhysicsCharacterEntry
	{
		UUID Owner{};
		Scope<CharacterController> Controller{};
		// The velocity of the last MoveCharacter, consumed by the next PreStep.
		glm::vec3 DesiredVelocity = glm::vec3(0.0f);
		// The owner's local Translation and Rotation that PostStep last wrote (as for Dynamic bodies).
		glm::vec3 WrittenTranslation = glm::vec3(0.0f);
		glm::quat WrittenRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	};

	struct PhysicsSystem::State
	{
		// --- Stream B -------------------------------------------------------------------------------------------------
		PhysicsSystemSpecification Specification{};
		PhysicsLayerTable Layers{};
		Scope<PhysicsWorld> World;
		// The created bodies by BodyHandle value: lookups only (output order comes from the scene's canonical order, §5.1).
		std::map<uint32_t, PhysicsBodyEntry> Bodies;
		// The shapes of mesh colliders (§9.2), shared by the bodies.
		PhysicsMeshShapeCache MeshShapes;
		IPhysicsEventListener* Listener = nullptr; // not owned
		std::vector<PhysicsEvent> LastEvents;
		std::vector<PhysicsDiagnostic> Diagnostics;

		// --- Stream C: queries and characters ----------------------------------------------------------------------------
		// The created characters by owner UUID.
		std::map<UUID, PhysicsCharacterEntry> Characters;
	};

}
