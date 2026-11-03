#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/Physics2DWorld.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Private/Physics2DComposition.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The state of a Physics2DSystem (Docs/Decisions/0032-m21-contract.md decisions 9 to 11): PhysicsSystemState.h's in the
// plane, without characters and mesh shapes. The pair and contact types (PhysicsPairKey, PhysicsSubShapeKey,
// PhysicsSubShapeContact, PhysicsPair) and the shape inputs (PhysicsShapeInput) are the 3D system's, so both worlds keep
// pairs by the same rules. Private to Scene.

namespace Engine {

	// One created 2D body: what contacts and query hits resolve its handle and sub-shape user data through.
	struct Physics2DBodyEntry
	{
		BodyHandle Handle{};
		UUID Owner{};
		PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody;
		// The collider entities in sub-shape user data order (Physics2DBodyPlan::Colliders).
		std::vector<UUID> Colliders{};
		Ref<const PhysicsShape> Shape{};
		// Dynamic bodies: the owner's local Translation and Rotation that PostStep last wrote (or that the body was created
		// from); when they differ bit for bit, PreStep compares the owner's planar pose with PlacedPose. Static bodies: the
		// owner's local values of their last placement, which PreStep writes back when a Transform is written beyond the
		// physics range.
		glm::vec3 WrittenTranslation = glm::vec3(0.0f);
		glm::quat WrittenRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		// Static bodies: the owner's world matrix the body was created or last teleported at.
		glm::mat4 PlacedMatrix = glm::mat4(1.0f);
		// The planar pose of the owner's Transform (Utils::GetPlanarPose, computed as PreStep computes a target) at the body's
		// last creation, placement, teleport, kinematic move or write-back. PreStep places a body again only when its owner's
		// planar pose differs from it bit for bit, so a write of the world Z alone moves and wakes nothing; it is never
		// compared with the body's own pose, whose angle the round trip through the quaternion may move by an ulp.
		Physics2DPose PlacedPose{};
		// What the body was built from, compared at each rebuild: the plan, the settings (Body2DDescription without its shape,
		// pose and velocities), the shape description and the transforms baked into it.
		Physics2DBodyPlan Plan{};
		Body2DDescription Settings{};
		Body2DShapeDescription ShapeDescription{};
		std::vector<PhysicsShapeInput> ShapeInputs{};
	};

	struct Physics2DSystem::State
	{
		// --- Composition, sync and events -----------------------------------------------------------------------------
		Physics2DSystemSpecification Specification{};
		PhysicsLayerTable Layers{};
		// Created at the first 2D body (Physics2DSystem.h); null before.
		Scope<Physics2DWorld> World;
		// The gravity the world has, or takes when it is created.
		glm::vec2 Gravity = glm::vec2(0.0f, -9.81f);
		// Why creating the world failed, when it did (every body is then refused with it).
		std::optional<Error> WorldError{};
		// The created bodies by BodyHandle value: lookups only (output order comes from the scene's canonical order).
		std::map<uint32_t, Physics2DBodyEntry> Bodies;
		IPhysics2DEventListener* Listener = nullptr; // not owned
		std::vector<PhysicsEvent> LastEvents;
		std::vector<PhysicsDiagnostic> Diagnostics;

		// The body handle value of each (owner, origin), and the body each collider entity was gathered into (when it is not
		// the owner).
		std::map<std::pair<UUID, PhysicsBodyOrigin>, uint32_t> BodiesByOwner;
		std::map<UUID, uint32_t> GatheredColliders;
		// The pairs, contacts and their indexes, as PhysicsSystem::State's.
		std::map<PhysicsPairKey, PhysicsPair> Pairs;
		std::map<PhysicsSubShapeKey, PhysicsSubShapeContact> Contacts;
		std::map<uint32_t, std::set<PhysicsSubShapeKey>> ContactsByBody;
		std::map<UUID, std::set<PhysicsPairKey>> PairsByOwner;
		std::map<UUID, std::set<PhysicsSubShapeKey>> GatheredContacts;
		std::map<UUID, std::set<PhysicsPairKey>> ColliderPairsByCollider;
		std::set<uint32_t> ReattributedBodies;
		std::set<PhysicsSubShapeKey> EligibleDormantContacts;
		std::set<std::tuple<std::string, DiagnosticSeverity, UUID, std::string>> RaisedDiagnostics;
		std::vector<PhysicsDiagnostic> PendingListenerDiagnostics;

		// What PreStep rebuilds from: a 2D physics component (or a 3D one on an entity holding a 2D one, or DisabledTag)
		// changed, and on which entities; the scene revision and the hierarchy at the last look.
		bool ComponentsChanged = false;
		std::set<UUID> ChangedEntities;
		uint64_t SeenRevision = 0;
		std::vector<std::pair<UUID, UUID>> SeenHierarchy;
		// Entities that became pending destruction or effectively disabled since the last flush.
		std::set<UUID> FlushCandidates;
		// max(1, ceil(60 / FixedHz)).
		uint32_t CollisionSteps = 1;
		// The compositions Rebuild ran (Physics2DSystemStats::Compositions).
		uint64_t Compositions = 0;
		// Whether callbacks are running (the step hooks assert it is clear).
		bool Dispatching = false;

		void ConnectSignals();
		void DisconnectSignals();
		void On2DComponentChanged(entt::registry& registry, entt::entity entity);
		void On3DComponentChanged(entt::registry& registry, entt::entity entity);
		void OnEntityLeavingPlay(entt::registry& registry, entt::entity entity);

		// Whether the scene holds any 2D physics component (EnTT storage sizes): without one and without a world, PreStep has
		// nothing to compose.
		[[nodiscard]] bool SceneHolds2DComponents() const;
		[[nodiscard]] bool IsPresent(UUID entity) const;
		[[nodiscard]] bool HasHierarchyChanged();
		void RecordHierarchy();
		// The world, created on first use with the current gravity, or why it could not be created.
		[[nodiscard]] Result<Physics2DWorld*> EnsureWorld();
		void Rebuild(bool hierarchyChanged, std::set<UUID>& woken);
		void RefreshShapes(const std::set<UUID>& owners, std::set<UUID>& woken);
		[[nodiscard]] std::set<UUID> FindShapeChanges(bool hierarchyChanged) const;
		[[nodiscard]] std::vector<PhysicsShapeInput> CaptureShapeInputs(const Physics2DBodyPlan& plan) const;
		[[nodiscard]] Result<Ref<const PhysicsShape>> BuildShape(const Physics2DBodyPlan& plan, Body2DShapeDescription& description);
		[[nodiscard]] bool CreateBody(const Physics2DBodyPlan& plan, std::optional<std::pair<glm::vec2, float>> velocities);
		void AssignPlan(Physics2DBodyEntry& entry, const Physics2DBodyPlan& plan);
		void RemoveBody(uint32_t handle, std::set<UUID>& woken);
		[[nodiscard]] bool ReplaceShape(Physics2DBodyEntry& entry, const Physics2DBodyPlan& plan, std::set<UUID>& woken);
		void WakeOwners(const std::set<UUID>& owners);
		void CollectPartners(UUID owner, std::set<UUID>& partners) const;

		PhysicsSubShapeContact& AddContact(const PhysicsSubShapeKey& key, const PhysicsPairKey& pair);
		void EraseContact(const PhysicsSubShapeKey& key);
		void ErasePair(const PhysicsPairKey& key);
		void PurgeBodyContacts(uint32_t handle, std::set<UUID>& partners);
		void SetContactSides(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const std::array<UUID, 2>& owners,
			const std::array<UUID, 2>& colliders);
		void LinkColliderPair(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const PhysicsPairKey& pair);
		void UnlinkColliderPair(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const PhysicsPairKey& pair);
		[[nodiscard]] bool IsScriptedGatheredCollider(UUID entity) const;

		void RaiseDiagnostic(PhysicsDiagnostic diagnostic, bool alreadyLogged = false);
		void RaiseBodyError(const Physics2DBodyPlan& plan, const Error& error, std::string_view field = {});
		void DeliverPendingDiagnostics();

		[[nodiscard]] const Physics2DBodyEntry* FindOwnedBody(UUID owner, PhysicsBodyOrigin origin) const;
		[[nodiscard]] Physics2DBodyEntry* FindOwnedBody(UUID owner, PhysicsBodyOrigin origin);
		[[nodiscard]] const Physics2DBodyEntry* FindBody(uint32_t handle) const;
		[[nodiscard]] Physics2DBodyEntry* FindBody(uint32_t handle);
		[[nodiscard]] Result<Physics2DBodyEntry*> FindRigidBody(UUID entity);
		[[nodiscard]] Result<const Physics2DBodyEntry*> FindRigidBody(UUID entity) const;

		// The collider entity and body owner of a world hit (nullopt for a body the system does not know, or one whose owner
		// or collider entity is pending destruction or disabled), and the body `owner` owns (its RigidBody2D or implicit
		// static body before its implicit sensor body).
		[[nodiscard]] std::optional<PhysicsOverlapHit> ResolveHit(BodyHandle body, uint32_t collider) const;
		[[nodiscard]] BodyHandle FindOwnedBodyHandle(UUID owner) const;
	};

}
