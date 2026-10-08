#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/CharacterController.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/PhysicsSystem.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The state of a PhysicsSystem (Architecture §9.2 to §9.6), shared by the system's .cpp files: PhysicsSystem.cpp
// (composition, sync, events, exits, body control, observability, and the characters' lifecycle within the step),
// PhysicsSystemQueries.cpp (queries and bounds) and PhysicsSystemCharacters.cpp (MoveCharacter and GetCharacterState).
// Private to Scene, so it changes without the contract owner's review (Docs/Decisions/0014-m11-decisions.md decision 17).
// The entry structs are at namespace scope, not nested: Clang with libstdc++ cannot default-construct a nested struct with
// default member initializers inside its enclosing class (ADR 0012 decision 24).

namespace Engine {

	// Which parts of an entity's local transform a body's shape bakes in.
	enum class PhysicsShapeInputParts : uint8_t
	{
		Scale,         // the owner and its ancestors: their scales make the owner's world scale
		ScaleRotation, // the same below a non-uniformly scaled ancestor, where a rotation changes the world scale too
		Whole          // an entity below the owner, down to a collider: its whole transform places the collider
	};

	// One entity whose local transform a body's shape was built from (§9.2 "scale is baked into the shape"): PreStep compares
	// the parts that matter bit for bit and rebuilds the shape when one changed (PhysicsSystem.h, rebuild triggers).
	struct PhysicsShapeInput
	{
		entt::entity Entity = entt::null;
		PhysicsShapeInputParts Parts = PhysicsShapeInputParts::Whole;
		TransformComponent Local{};
	};

	// The asset version of a mesh a body's shape was built from (§7.5 hot reload: a new version rebuilds the shape).
	struct PhysicsMeshInput
	{
		AssetHandle Mesh{};
		uint64_t Version = 0;
	};

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
		// it differs bit for bit. WrittenTranslation and WrittenRotation then hold the owner's local values of that placement,
		// which PreStep writes back when a Transform is written beyond the physics range.
		glm::mat4 PlacedMatrix = glm::mat4(1.0f);
		// What the body was built from, compared at each rebuild to choose between nothing, PhysicsWorld::SetShape and a new
		// body: the plan, the settings (BodyDescription without its shape, pose and velocities), the shape description and
		// the transforms and mesh versions baked into it.
		PhysicsBodyPlan Plan{};
		BodyDescription Settings{};
		BodyShapeDescription ShapeDescription{};
		std::vector<PhysicsShapeInput> ShapeInputs{};
		std::vector<PhysicsMeshInput> MeshInputs{};
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
		// What the controller was created from (its Pose aside), compared at each rebuild, and the component's GravityFactor,
		// read at every update.
		CharacterControllerDescription Description{};
		float GravityFactor = 1.0f;
	};

	// Whether a body pair touches as solids or through a sensor (§9.4: OnCollision* or OnTrigger*).
	enum class PhysicsPairKind : uint8_t
	{
		Collision,
		Trigger
	};

	// An active pair (§9.4), keyed by its two owners, never by body handle, so it survives the rebuild of either body.
	struct PhysicsPairKey
	{
		UUID Low{};
		UUID High{};
		PhysicsPairKind Kind = PhysicsPairKind::Collision;

		auto operator<=>(const PhysicsPairKey&) const = default;
	};

	// One sub-shape contact of a pair as Jolt reports it (§9.4 step 1): the two bodies' handle values and SubShapeID values,
	// the lower handle first, and whether a character's listener reported it.
	struct PhysicsSubShapeKey
	{
		uint32_t BodyA = 0;
		uint32_t SubShapeA = 0;
		uint32_t BodyB = 0;
		uint32_t SubShapeB = 0;
		bool FromCharacter = false;

		auto operator<=>(const PhysicsSubShapeKey&) const = default;
	};

	// A sub-shape contact's state. Jolt reports OnContactRemoved for the contacts of bodies that fall asleep (ContactListener
	// "as soon as a body goes to sleep the contacts between that body and all other bodies will receive an OnContactRemoved
	// callback"), although they still touch: such a contact stays, dormant, until a step that began with one of its bodies
	// awake does not report it again (PhysicsSystem.cpp).
	struct PhysicsSubShapeContact
	{
		bool Dormant = false;
	};

	struct PhysicsPair
	{
		// The sub-shape contacts that touch (the pair's reference count is their number).
		std::map<PhysicsSubShapeKey, PhysicsSubShapeContact> Contacts;
		// The enter was dispatched (to the sides that are flagged), and the exit not yet.
		bool Active = false;
		bool EnteredLow = false;
		bool EnteredHigh = false;
		// PreStep destroyed a body of the pair, which lost every contact; PostStep ends it unless a contact re-established it.
		bool Unconfirmed = false;
		uint64_t SinceTick = 0;
		// The collider entities of the first sub-shape pair that touched (§9.4 contact.Collider and OtherCollider).
		UUID LowCollider{};
		UUID HighCollider{};
	};

	struct PhysicsSystem::State
	{
		// --- Composition, sync and events -----------------------------------------------------------------------------
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

		// The body handle value of each (owner, origin), the body each collider entity was gathered into (when it is not the
		// owner), and the owner of each character's inner body.
		std::map<std::pair<UUID, PhysicsBodyOrigin>, uint32_t> BodiesByOwner;
		std::map<UUID, uint32_t> GatheredColliders;
		std::map<uint32_t, UUID> InnerBodies;
		// The pairs, the pair of each sub-shape contact, the sub-shape contacts of each body handle value and the pairs of
		// each owner (indexes kept in step with Pairs).
		std::map<PhysicsPairKey, PhysicsPair> Pairs;
		std::map<PhysicsSubShapeKey, PhysicsPairKey> ContactPairs;
		std::map<uint32_t, std::set<PhysicsSubShapeKey>> ContactsByBody;
		std::map<UUID, std::set<PhysicsPairKey>> PairsByOwner;
		// Dormant contacts a body of which was awake when the coming step began (PreStep end).
		std::set<PhysicsSubShapeKey> EligibleDormantContacts;
		// Every diagnostic raised, by (code, severity, entity, subject), and those raised since the listener last heard them.
		std::set<std::tuple<std::string, DiagnosticSeverity, UUID, std::string>> RaisedDiagnostics;
		std::vector<PhysicsDiagnostic> PendingListenerDiagnostics;

		// What PreStep rebuilds from (EnTT signals and the scene's structure, PhysicsSystem.h): a physics component (or
		// MeshRenderer, or DisabledTag) changed somewhere, and on which entities; the scene revision and the hierarchy (the
		// canonical order with each entity's parent) at the last look.
		bool ComponentsChanged = false;
		std::set<UUID> ChangedEntities;
		uint64_t SeenRevision = 0;
		std::vector<std::pair<UUID, UUID>> SeenHierarchy;
		// Entities that became pending destruction or effectively disabled since the last flush (FlushDestroyed visits them).
		std::set<UUID> FlushCandidates;
		// max(1, ceil(60 / FixedHz)) (§9.3).
		uint32_t CollisionSteps = 1;
		// Whether callbacks are running (the step hooks assert it is clear).
		bool Dispatching = false;

		// Scene/PhysicsSystem.cpp.
		void ConnectSignals();
		void DisconnectSignals();
		void OnComponentChanged(entt::registry& registry, entt::entity entity);
		void OnEntityLeavingPlay(entt::registry& registry, entt::entity entity);

		[[nodiscard]] bool IsPresent(UUID entity) const;
		[[nodiscard]] bool HasHierarchyChanged();
		void RecordHierarchy();
		void Rebuild(bool hierarchyChanged, std::set<UUID>& woken);
		void RefreshShapes(const std::set<UUID>& owners, std::set<UUID>& woken);
		[[nodiscard]] std::set<UUID> FindShapeChanges(bool hierarchyChanged) const;
		[[nodiscard]] std::vector<PhysicsShapeInput> CaptureShapeInputs(const PhysicsBodyPlan& plan) const;
		[[nodiscard]] std::vector<PhysicsMeshInput> CaptureMeshInputs(const PhysicsBodyPlan& plan) const;
		[[nodiscard]] bool CreateBody(const PhysicsBodyPlan& plan, std::optional<std::pair<glm::vec3, glm::vec3>> velocities);
		void AssignPlan(PhysicsBodyEntry& entry, const PhysicsBodyPlan& plan);
		void RemoveBody(uint32_t handle, std::set<UUID>& woken);
		[[nodiscard]] bool ReplaceShape(PhysicsBodyEntry& entry, const PhysicsBodyPlan& plan, std::set<UUID>& woken);
		[[nodiscard]] bool CreateCharacter(const PhysicsBodyPlan& plan, std::optional<glm::vec3> velocity);
		void RemoveCharacter(UUID owner, std::set<UUID>& woken);
		void WakeOwners(const std::set<UUID>& owners);
		void CollectPartners(UUID owner, std::set<UUID>& partners) const;

		void AddContact(const PhysicsSubShapeKey& key, const PhysicsPairKey& pair);
		void EraseContact(const PhysicsSubShapeKey& key);
		void ErasePair(const PhysicsPairKey& key);
		void PurgeBodyContacts(uint32_t handle, std::set<UUID>& partners);

		// Records a diagnostic once per (code, severity, entity, subject) and logs it, unless `alreadyLogged` (the world's own
		// PHYSICS_LIMIT_EXCEEDED warning near a limit, PhysicsWorldLimits; the body limit's refusals after the first).
		void RaiseDiagnostic(PhysicsDiagnostic diagnostic, bool alreadyLogged = false);
		// The diagnostic of a body or character that `error` refused, on the component (and `field`, when given) at fault.
		void RaiseBodyError(const PhysicsBodyPlan& plan, const Error& error, std::string_view field = {});
		void DeliverPendingDiagnostics();

		[[nodiscard]] const PhysicsBodyEntry* FindOwnedBody(UUID owner, PhysicsBodyOrigin origin) const;
		[[nodiscard]] PhysicsBodyEntry* FindOwnedBody(UUID owner, PhysicsBodyOrigin origin);
		// The entry of the body `handle` that an index (BodiesByOwner, GatheredColliders) names; verified to exist.
		[[nodiscard]] const PhysicsBodyEntry* FindBody(uint32_t handle) const;
		[[nodiscard]] PhysicsBodyEntry* FindBody(uint32_t handle);
		[[nodiscard]] Result<PhysicsBodyEntry*> FindRigidBody(UUID entity);
		[[nodiscard]] Result<const PhysicsBodyEntry*> FindRigidBody(UUID entity) const;

		// --- Characters, queries and bounds --------------------------------------------------------------------------
		// The created characters by owner UUID. Their lifecycle (CreateCharacter, RemoveCharacter, the teleports and updates
		// of PreStep step 5 and the write-back of PostStep) is in PhysicsSystem.cpp, because the step phases and the rebuilds
		// own it; InnerBodies maps each character's inner body to its owner.
		std::map<UUID, PhysicsCharacterEntry> Characters;

		// The owner of the character whose inner body is `body` (InnerBodies); the invalid UUID when `body` is no character's.
		[[nodiscard]] UUID FindCharacterOwner(BodyHandle body) const;

		// The query helpers (PhysicsSystemQueries.cpp):
		//   - ResolveHit: the collider entity and the body owner of a world hit on collider index `collider` of `body`: the
		//     body's entry (Bodies) and its collider table, or, for a character's inner body, the character's entity for both;
		//     nullopt for a body the system does not know, or one whose owner or collider entity is pending destruction or
		//     disabled (IsPresent).
		//   - FindOwnedBodyHandle: the body `owner` owns, as PhysicsSystem::GetBodyInfo chooses it (its RigidBody, character
		//     or implicit static body, before its implicit sensor body); the invalid handle when it owns none.
		[[nodiscard]] std::optional<PhysicsOverlapHit> ResolveHit(BodyHandle body, uint32_t collider) const;
		[[nodiscard]] BodyHandle FindOwnedBodyHandle(UUID owner) const;
	};

}
