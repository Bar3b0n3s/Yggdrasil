#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/PhysicsSystem.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The play session's 2D physics (M21, C21.1, R-2D-PHYS; ADR 0028 decision 6; Docs/Decisions/0032-m21-contract.md decisions
// 9 to 13): the bridge between a runtime scene's 2D components (RigidBody2D, BoxCollider2D, CircleCollider2D,
// PolygonCollider2D) and its Physics2DWorld (Physics/Physics2DWorld.h), a second Jolt world beside PhysicsSystem's. It is
// PhysicsSystem's design in the plane: the same composition rules with RigidBody2D in RigidBody's place (gathering into the
// nearest ancestor RigidBody2D's compound, implicit static and sensor bodies, collision groups, canonical body order), the
// same PreStep (rebuilds, pairs across rebuilds, kinematic moves, teleports), Step and PostStep (write-back and events), the
// same FlushDestroyed, the same pair rules (M20's pending enter and collider events) and the same diagnostics, each as
// Scene/PhysicsSystem.h states it, with these differences:
//   - Worlds. An entity belongs to one world: one that holds a 2D physics component and a 3D one (RigidBody, BoxCollider,
//     SphereCollider, CapsuleCollider, MeshCollider, CharacterController) is PHYSICS2D_MIXED_WORLDS (Error): it keeps its
//     3D body, exactly as without its 2D components, and its 2D components make no 2D body, so the 3D world's results stay
//     bit-identical (Breakout's replay). The two worlds never collide with each other.
//   - The plane. A body's pose is its owner's world X and Y and the angle about Z of its world rotation's twist about Z
//     (ToPhysics2DPose). An owner whose world rotation is not a rotation about Z alone (beyond PhysicsUnitRotationTolerance)
//     is PHYSICS2D_OFF_PLANE (Warning): its body takes the twist. PostStep writes back X, Y and the rotation about Z,
//     keeping the entity's world Z (free for draw depth) and replacing a swing by none; so a 2D body never leaves its plane.
//   - The phases (§5.7, ADR 0028's phase-2 execution order): Physics2DPreStep, Physics2DStep and Physics2DPostStep run after
//     the 3D PhysicsPostStep and before DestroyFlush, in Play and Simulate alike. Physics2DPreStep first runs the transform
//     update when the 3D post-step changed a transform, since PreStep reads the cached world matrices. 3D callbacks see an
//     unstepped 2D world and 2D callbacks a stepped and written-back 3D world. DestroyFlush flushes both worlds, the 3D one
//     first; PostStepTransformUpdate propagates both write-backs.
//   - Events. The pair events are PhysicsEvents (Point and Normal in the plane, z = 0), sorted by M20's PhysicsEventOrderKey
//     and dispatched to an IPhysics2DEventListener, whose scripts receive OnCollisionEnter2D, OnCollisionExit2D,
//     OnTriggerEnter2D and OnTriggerExit2D on every slot of the receiving entity (M20's script slots), exactly as the 3D
//     callbacks are delivered.
//   - Queries take and return planar values (vec2, angles in degrees about +Z for poses, radians per second for angular
//     velocities); the layer masks are the 3D world's (PhysicsSettings.Layers, Physics.LayerMask).
//   - The state hash block (AppendStateHash) follows M19's native block, and is empty without a 2D body, so a scene without
//     one keeps its hash and every existing replay its checkpoints.
//   - The world is created at the first 2D body. Create makes no Physics2DWorld (no second Jolt world, temp allocator or
//     pair and manifold caches) while the scene's components make no 2D body; the PreStep that composes the first creates it
//     (Create itself when the scene starts with one), and it lives until the system is destroyed, even when its last body
//     goes. Before it exists the system answers as an empty world: queries hit nothing, GetStats and the hash block are
//     empty, GetGravity returns the specification's gravity (or SetGravity's, which the world takes when it is created),
//     and Step and PostStep do nothing. So a session without 2D bodies, every 3D game's, costs and behaves exactly as before
//     M21: it holds one PhysicsWorld ("PhysicsSystem: destroying the session destroys every body and the world").
//
// Determinism, threading and ownership as PhysicsSystem's. Frozen by the M21 contract (C21.1); stream P2 implements it.

namespace Engine {

	class Scene;
	class XXH64Hasher;

	// What a 2D system is created with. The play session fills it from its specification.
	struct Physics2DSystemSpecification
	{
		// Required, outliving the system (a documented back-reference): the play session's runtime scene.
		Scene* RuntimeScene = nullptr;
		// The project's Physics2DSettings.Gravity, in metres per second squared along X and Y.
		glm::vec2 Gravity = glm::vec2(0.0f, -9.81f);
		// The project's PhysicsSettings.Layers and Collisions, shared with the 3D world.
		std::vector<std::string> Layers = { std::string(DefaultPhysicsLayerName) };
		std::vector<std::vector<std::string>> Collisions = { { std::string(DefaultPhysicsLayerName), std::string(DefaultPhysicsLayerName) } };
		// Simulation.FixedHz (>= 1): the collision steps of each world step, as the 3D world's.
		uint32_t FixedHz = 60;
		PhysicsWorldLimits Limits{};
	};

	// Receives the 2D world's sorted events and run-time diagnostics (the ScriptEngine, which delivers the 2D callbacks).
	// OnPhysics2DEvent's order and drop rules are IPhysicsEventListener::OnPhysicsEvent's, and so are the calls it may make.
	class IPhysics2DEventListener
	{
	public:
		virtual ~IPhysics2DEventListener() = default;

		virtual void OnPhysics2DEvent(const PhysicsEvent& event) = 0;
		virtual void OnPhysics2DDiagnostic(const PhysicsDiagnostic& /*diagnostic*/) {}
	};

	// A 2D ray hit (Physics2D.Raycast's RaycastHit, physics2d.raycast's hits).
	struct Physics2DRaycastHit
	{
		UUID Entity{}; // the collider entity that was hit
		UUID Body{};   // the owner of the body it belongs to
		glm::vec2 Point = glm::vec2(0.0f);
		glm::vec2 Normal = glm::vec2(0.0f, 1.0f); // unit, against the ray
		float Distance = 0.0f;

		bool operator==(const Physics2DRaycastHit&) const = default;
	};

	struct Physics2DSystemStats
	{
		uint32_t BodyCount = 0;
		uint32_t ActiveBodyCount = 0;
		uint32_t ContactPairCount = 0;
	};

	class Physics2DSystem
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class Physics2DSystem;
		};

		// Use Create.
		explicit Physics2DSystem(ConstructionKey key);
		// Removes every body (no events), then destroys the world.
		~Physics2DSystem();

		Physics2DSystem(const Physics2DSystem&) = delete;
		Physics2DSystem& operator=(const Physics2DSystem&) = delete;

		// The 2D physics of `specification.RuntimeScene`: the layer table, then, when the scene's current components make a 2D
		// body, the Physics2DWorld and every 2D body, created in canonical order (see the file comment: a scene without one
		// gets no world yet). Create checks everything Physics2DWorld::Create would (the PhysicsEngine initialized, the
		// gravity, the limits) and the layers whether or not it creates the world, so a session that cannot have a 2D world
		// fails to start as before, and the world's later creation cannot fail. Errors: as PhysicsSystem::Create's.
		[[nodiscard]] static Result<Scope<Physics2DSystem>> Create(const Physics2DSystemSpecification& specification);

		// --- The step (Physics2DPreStep, Physics2DStep, Physics2DPostStep; see the file comment) -----------------------------
		void PreStep(const SimStep& step);
		void Step(const SimStep& step);
		void PostStep(const SimStep& step);
		void FlushDestroyed(uint64_t tick);

		void SetEventListener(IPhysics2DEventListener* listener);
		[[nodiscard]] std::span<const PhysicsEvent> GetLastEvents() const;

		// --- Bodies (§11.5 RigidBody2D; `entity` owns a RigidBody2D body) --------------------------------------------------
		// Errors: PhysicsSystem's body errors ("entity <id> has no 2D physics body").
		[[nodiscard]] Status AddForce(UUID entity, const glm::vec2& force);
		[[nodiscard]] Status AddForceAtPosition(UUID entity, const glm::vec2& force, const glm::vec2& position);
		[[nodiscard]] Status AddTorque(UUID entity, float torque);
		[[nodiscard]] Status AddImpulse(UUID entity, const glm::vec2& impulse);
		[[nodiscard]] Status AddAngularImpulse(UUID entity, float angularImpulse);
		[[nodiscard]] Result<glm::vec2> GetLinearVelocity(UUID entity) const;
		[[nodiscard]] Status SetLinearVelocity(UUID entity, const glm::vec2& velocity);
		[[nodiscard]] Result<float> GetAngularVelocity(UUID entity) const;
		[[nodiscard]] Status SetAngularVelocity(UUID entity, float velocity);
		// Kinematic bodies: the target in the plane, `angle` in degrees about +Z.
		[[nodiscard]] Status MoveKinematic(UUID entity, const glm::vec2& position, float angle);
		// Any 2D body: places it at once (PhysicsSystem::Teleport's rules), the angle in degrees about +Z kept when nullopt.
		[[nodiscard]] Status Teleport(UUID entity, const glm::vec2& position, std::optional<float> angle);
		[[nodiscard]] Result<bool> IsSleeping(UUID entity) const;
		[[nodiscard]] Status WakeUp(UUID entity);

		// --- Queries (between steps; PhysicsSystem's rules and sorting) ------------------------------------------------------
		[[nodiscard]] Result<std::optional<Physics2DRaycastHit>> Raycast(const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
			PhysicsLayerMask layers = AllPhysicsLayers) const;
		[[nodiscard]] Result<std::vector<Physics2DRaycastHit>> RaycastAll(const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
			PhysicsLayerMask layers = AllPhysicsLayers) const;
		[[nodiscard]] Result<std::vector<PhysicsOverlapHit>> OverlapCircle(const glm::vec2& center, float radius,
			PhysicsLayerMask layers = AllPhysicsLayers) const;
		// `angle` in degrees about +Z.
		[[nodiscard]] Result<std::vector<PhysicsOverlapHit>> OverlapBox(const glm::vec2& center, const glm::vec2& halfExtents, float angle,
			PhysicsLayerMask layers = AllPhysicsLayers) const;
		[[nodiscard]] std::optional<Aabb> GetBodyBounds(UUID entity) const;
		[[nodiscard]] std::optional<Aabb> GetColliderBounds(UUID entity) const;
		[[nodiscard]] const PhysicsLayerTable& GetLayers() const;

		[[nodiscard]] glm::vec2 GetGravity() const;
		[[nodiscard]] Status SetGravity(const glm::vec2& gravity);

		// --- Observability --------------------------------------------------------------------------------------------------
		// Every diagnostic raised (the PHYSICS_* codes it shares with the 3D world, and PHYSICS2D_MIXED_WORLDS and
		// PHYSICS2D_OFF_PLANE, Scene/Physics2DValidation.h), as PhysicsSystem::GetDiagnostics.
		[[nodiscard]] std::span<const PhysicsDiagnostic> GetDiagnostics() const;
		[[nodiscard]] Physics2DSystemStats GetStats() const;
		// Appends the 2D block to the session's state hash: for every created 2D body in canonical order of its owner (an
		// owner's solid body before its sensor body), the owner's UUID, its position (x, y), angle, linear velocity (x, y) and
		// angular velocity as the little-endian bit patterns of their floats, and its sleeping flag. Without a 2D body it
		// appends nothing.
		void AppendStateHash(XXH64Hasher& hasher) const;
	private:
		// The world, the body table, the pairs, the events and the diagnostics (Scene/Physics2DSystem.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
