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
#include "Engine/Scene/PhysicsJoints.h"
#include "Engine/Scene/PhysicsSystem.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
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
// PhysicsSystemQueries.cpp (queries and bounds), PhysicsSystemCharacters.cpp (MoveCharacter and GetCharacterState), from
// M36 PhysicsSystemSurfaces.cpp (the collider surfaces) and from M37 PhysicsSystemJoints.cpp (joints).
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

	// The asset version of a mesh, or of an M26 terrain's heightfield, a body's shape was built from (§7.5 hot reload: a new
	// version rebuilds the shape; ADR 0037 decision 21: so does an edit or a reimport of a heightfield).
	struct PhysicsAssetInput
	{
		AssetHandle Asset{};
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
		// the transforms and the mesh and heightfield versions baked into it.
		PhysicsBodyPlan Plan{};
		BodyDescription Settings{};
		BodyShapeDescription ShapeDescription{};
		std::vector<PhysicsShapeInput> ShapeInputs{};
		std::vector<PhysicsAssetInput> AssetInputs{};
		// M36: whether the world holds a non-empty list of collider surfaces for the body (SendSurfaces).
		bool HasSurfaces = false;
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
	// M20 (Docs/Decisions/0031-m20-contract.md decision 2): a collider pair, the pair of a scripted gathered collider with the
	// partner's body owner, is keyed by (the collider entity, the partner's body owner, kind): Collider is the collider entity,
	// and Low and High are the collider entity and the partner, the lower UUID first. A body pair's Collider is the null UUID,
	// so body pairs keep their order among themselves.
	struct PhysicsPairKey
	{
		UUID Low{};
		UUID High{};
		PhysicsPairKind Kind = PhysicsPairKind::Collision;
		UUID Collider{};

		[[nodiscard]] bool IsColliderPair() const { return Collider.IsValid(); }

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

	// A sub-shape contact's state, and the pairs it counts in. Jolt reports OnContactRemoved for the contacts of bodies that
	// fall asleep (ContactListener "as soon as a body goes to sleep the contacts between that body and all other bodies will
	// receive an OnContactRemoved callback"), although they still touch: such a contact stays, dormant, until a step that
	// began with one of its bodies awake does not report it again (PhysicsSystem.cpp).
	struct PhysicsSubShapeContact
	{
		// The body pair it counts in.
		PhysicsPairKey Pair{};
		// M20: the collider pairs it counts in (at most one for each side whose collider is a scripted gathered collider),
		// sorted.
		std::vector<PhysicsPairKey> ColliderPairs{};
		// The body owners and the collider entities of its two sides, its BodyA's, then its BodyB's (PhysicsSubShapeKey's
		// order), as its records, or the resolution after a shape was replaced in place, last named them.
		std::array<UUID, 2> Owners{};
		std::array<UUID, 2> Colliders{};
		// The first point, the normal from its BodyA's side towards its BodyB's, and the relative speed of its last touching
		// record (of its Added record while it has not touched): the enter contact of a pair it starts without a record of
		// that step (a collider pair whose collider became a scripted gathered collider, or that a resolution moved it to).
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		float RelativeSpeed = 0.0f;
		bool Dormant = false;
		// M20 (Docs/Decisions/0031-m20-contract.md decision 3): it touched (an Added record that touched, or a Touched record
		// since). A contact that has not touched counts for its pairs but cannot start them.
		bool Touching = true;
	};

	// M37: what decides whether a joint's plan must be composed again (PhysicsSystem.h's M37 paragraph): the fields that
	// change which bodies it joins or whether it is valid. Compared exactly.
	struct PhysicsJointKey
	{
		PhysicsJointType Type = PhysicsJointType::Hinge;
		UUID ConnectedBody{};
		glm::vec3 Anchor = glm::vec3(0.0f);
		glm::vec3 Axis = glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 ConnectedAnchor = glm::vec3(0.0f);
		bool UseLimits = false;
		float LowerLimit = 0.0f;
		float UpperLimit = 0.0f;

		bool operator==(const PhysicsJointKey&) const = default;
	};

	// M37: what a joint in force was made from: a hinge's, slider's or fixed joint's kept frames are formed again when it
	// differs, and a distance joint, which keeps no frames and reads both anchors at each creation, is made again.
	struct PhysicsJointFormation
	{
		PhysicsJointType Type = PhysicsJointType::Hinge;
		UUID Body{};
		UUID Connected{};
		PhysicsBodyOrigin ConnectedOrigin = PhysicsBodyOrigin::RigidBody;
		UUID ConnectedBody{};
		glm::vec3 Anchor = glm::vec3(0.0f);
		glm::vec3 Axis = glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 ConnectedAnchor = glm::vec3(0.0f);
		// The joint's entity and its ancestors up to its body's owner, inclusive.
		std::vector<UUID> Chain{};

		bool operator==(const PhysicsJointFormation&) const = default;
	};

	// M37: one joint entity the session knows (Scene/PhysicsSystemJoints.cpp): its last plan, its live joint and its kept
	// frames.
	struct PhysicsJointEntry
	{
		UUID Entity{};
		// The plan the last joint composition gave it, and the key it read.
		PhysicsJointPlan Plan{};
		PhysicsJointKey PlannedKey{};
		// The live joint (invalid while it is not in force) and the bodies it joins (the connected one invalid for the world).
		JointHandle Handle{};
		BodyHandle Body{};
		BodyHandle Connected{};
		// The settings the live joint has.
		PhysicsJointSettings Settings{};
		// The kept frames (hinge, slider and fixed joints) on the joint's body and on the connected body, relative to their
		// origins, set only by a creation that succeeded; and what they (or a distance joint in force) were formed from.
		bool HasFrames = false;
		PhysicsJointFrame Frame{};
		PhysicsJointFrame ConnectedFrame{};
		PhysicsJointFormation Formation{};
	};

	struct PhysicsPair
	{
		// The sub-shape contacts it counts (its reference count is their number); their state is in State::Contacts.
		std::set<PhysicsSubShapeKey> Contacts;
		// The enter was dispatched (to the sides that are flagged), and the exit not yet. A collider pair's callbacks go to its
		// collider's side only.
		bool Active = false;
		bool EnteredLow = false;
		bool EnteredHigh = false;
		// PreStep destroyed a body of the pair, which lost every contact, or a collider pair's collider stopped being a
		// scripted gathered collider: PostStep ends it with a synthesized exit unless a contact re-established it.
		bool Unconfirmed = false;
		// M20 (decision 3): one of its contacts touched since the pair began. An inactive pair enters at the PostStep that
		// sets it; an inactive one that had set it before had its enter dropped (PhysicsSystem.h) and waits for the flush.
		bool Touched = false;
		uint64_t SinceTick = 0;
		// The collider entities of the first sub-shape pair that touched (§9.4 contact.Collider and OtherCollider), on the Low
		// and High sides.
		UUID LowCollider{};
		UUID HighCollider{};
		// A collider pair's: the owner of its collider's body (PhysicsEvent::SelfBody), as its contacts last named it.
		UUID ColliderBody{};
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
		// The pairs (body pairs and M20's collider pairs), the sub-shape contacts with their state and pairs, the sub-shape
		// contacts of each body handle value and the pairs of each owner, a collider pair under its collider and its partner
		// (indexes kept in step with Pairs).
		std::map<PhysicsPairKey, PhysicsPair> Pairs;
		std::map<PhysicsSubShapeKey, PhysicsSubShapeContact> Contacts;
		std::map<uint32_t, std::set<PhysicsSubShapeKey>> ContactsByBody;
		std::map<UUID, std::set<PhysicsPairKey>> PairsByOwner;
		// M20 (Docs/Decisions/0031-m20-contract.md decision 2): the contacts of each gathered collider entity (a side's
		// collider that is not its body's owner), scripted or not, and the collider pairs of each collider entity.
		std::map<UUID, std::set<PhysicsSubShapeKey>> GatheredContacts;
		std::map<UUID, std::set<PhysicsPairKey>> ColliderPairsByCollider;
		// The bodies whose shape or collider table PreStep replaced in place (ReplaceShape): Jolt keeps reporting a contact
		// that persists under its sub-shape IDs, so PostStep resolves the colliders of their contacts again.
		std::set<uint32_t> ReattributedBodies;
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
		[[nodiscard]] std::vector<PhysicsAssetInput> CaptureAssetInputs(const PhysicsBodyPlan& plan) const;
		[[nodiscard]] bool CreateBody(const PhysicsBodyPlan& plan, std::optional<std::pair<glm::vec3, glm::vec3>> velocities);
		void AssignPlan(PhysicsBodyEntry& entry, const PhysicsBodyPlan& plan);
		void RemoveBody(uint32_t handle, std::set<UUID>& woken);
		[[nodiscard]] bool ReplaceShape(PhysicsBodyEntry& entry, const PhysicsBodyPlan& plan, std::set<UUID>& woken);
		[[nodiscard]] bool CreateCharacter(const PhysicsBodyPlan& plan, std::optional<glm::vec3> velocity);
		void RemoveCharacter(UUID owner, std::set<UUID>& woken);
		void WakeOwners(const std::set<UUID>& owners);
		void CollectPartners(UUID owner, std::set<UUID>& partners) const;

		// Records a new contact `key` in the body pair `pair` (which exists) and in the indexes, and returns its state.
		PhysicsSubShapeContact& AddContact(const PhysicsSubShapeKey& key, const PhysicsPairKey& pair);
		// Removes a contact from its pairs and the indexes; the pairs stay, for PostStep's transitions.
		void EraseContact(const PhysicsSubShapeKey& key);
		// Removes a pair: a body pair with its contacts; a collider pair from its contacts, which stay in their body pair.
		void ErasePair(const PhysicsPairKey& key);
		void PurgeBodyContacts(uint32_t handle, std::set<UUID>& partners);
		// M20: sets the owners and colliders of a contact's sides, moving it between the gathered colliders' contacts and
		// unlinking it from the collider pairs of a side's previous collider.
		void SetContactSides(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const std::array<UUID, 2>& owners,
			const std::array<UUID, 2>& colliders);
		// M20: counts the contact in the collider pair `pair` (which exists), or no longer.
		void LinkColliderPair(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const PhysicsPairKey& pair);
		void UnlinkColliderPair(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const PhysicsPairKey& pair);
		// M20 (decision 2): whether `entity` is a scripted gathered collider: present (IsPresent), its colliders gathered into
		// another owner's body (GatheredColliders), and its Script component naming a script. The system reads the component
		// alone, never the script or a VM.
		[[nodiscard]] bool IsScriptedGatheredCollider(UUID entity) const;

		// Records a diagnostic once per (code, severity, entity, subject) and logs it, unless `alreadyLogged` (the world's own
		// PHYSICS_LIMIT_EXCEEDED warning near a limit, PhysicsWorldLimits; the body limit's refusals after the first).
		void RaiseDiagnostic(PhysicsDiagnostic diagnostic, bool alreadyLogged = false);
		// The diagnostic of a body or character that `error` refused, on the component (and `field`, when given) at fault: a
		// RigidBody plan's `fieldComponent` when given (M44: MassProperties), else the RigidBody.
		void RaiseBodyError(const PhysicsBodyPlan& plan, const Error& error, std::string_view field = {}, std::string_view fieldComponent = {});
		// M44: the diagnostic of a Dynamic body that PhysicsShape::CheckDynamicBody refused with `settings`' mass, degrees of
		// freedom and mass override, on the MassProperties when only its override causes the refusal (GetDynamicBodyFault).
		void RaiseDynamicBodyError(const PhysicsBodyPlan& plan, const PhysicsShape& shape, const BodyDescription& settings, const Error& error);
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
		// The script-facing state of `controller`'s character after its last update (PhysicsSystemCharacters.cpp), its ground
		// collider resolved through ResolveHit.
		[[nodiscard]] PhysicsCharacterState MakeCharacterState(const CharacterController& controller) const;

		// The query helpers (PhysicsSystemQueries.cpp):
		//   - ResolveHit: the collider entity and the body owner of a world hit on collider index `collider` of `body`: the
		//     body's entry (Bodies) and its collider table, or, for a character's inner body, the character's entity for both;
		//     nullopt for a body the system does not know, or one whose owner or collider entity is pending destruction or
		//     disabled (IsPresent).
		//   - FindOwnedBodyHandle: the body `owner` owns, as PhysicsSystem::GetBodyInfo chooses it (its RigidBody, character
		//     or implicit static body, before its implicit sensor body); the invalid handle when it owns none.
		[[nodiscard]] std::optional<PhysicsOverlapHit> ResolveHit(BodyHandle body, uint32_t collider) const;
		[[nodiscard]] BodyHandle FindOwnedBodyHandle(UUID owner) const;

		// --- Collider surfaces (M36, Docs/Decisions/0053-m36-contact-surfaces.md decision 3; PhysicsSystemSurfaces.cpp) ----
		// The entities whose ColliderSurface was added, changed or removed since the last PreStep (the component's signals,
		// which never set ComponentsChanged, so a surface write never recomposes the scene).
		std::set<UUID> DirtySurfaces;

		void OnSurfaceChanged(entt::registry& registry, entt::entity entity);
		// The surfaces of `entry`'s solid colliders whose entity has a ColliderSurface, in collider order: its Friction and
		// Restitution, and its SurfaceVelocity turned from the entity's axes into the body's (the owner's inverse world rotation
		// times the entity's world rotation; the velocity itself for the owner's own colliders). None for a sensor body.
		[[nodiscard]] std::vector<PhysicsColliderSurface> CollectSurfaces(const PhysicsBodyEntry& entry) const;
		// Sends `entry`'s surfaces to the world (PhysicsWorld::SetColliderSurfaces) when it has some or had some.
		void SendSurfaces(PhysicsBodyEntry& entry);
		// PreStep, after its rebuilds: sends the surfaces of every body that holds a collider of a DirtySurfaces entity, in
		// body handle order, and empties the set.
		void RefreshSurfaces();
		// The created solid body (a RigidBody's or an implicit static body, never a sensor's) that holds the colliders of
		// `entity`, which is present (IsPresent): its RigidBody's or implicit static body when the entity owns one holding its
		// own colliders, else the body they were gathered into; null when the entity has no solid collider in a created body.
		[[nodiscard]] const PhysicsBodyEntry* FindColliderBody(UUID entity) const;
		// --- Joints (M37, Scene/PhysicsSystemJoints.cpp; PhysicsSystem.h's M37 paragraph) -------------------------------
		// The last Rebuild's body composition (the joints' input), the joint entities by UUID (so every walk is in UUID
		// order), what changed since the joints were last synchronized, and the last PostStep's breaks.
		PhysicsComposition Composition{};
		std::map<UUID, PhysicsJointEntry> Joints;
		// A joint component changed, or a body was created or removed: SynchronizeJoints has work.
		bool JointsChanged = false;
		// The bodies were composed again or a joint component was added or removed: the joint plan is composed again.
		bool JointPlansStale = false;
		// The joint entities whose components changed (their plan is composed again when their key changed).
		std::set<UUID> ChangedJoints;
		std::vector<PhysicsJointBreak> LastJointBreaks;

		void OnJointChanged(entt::registry& registry, entt::entity entity);
		void OnJointAddedOrRemoved(entt::registry& registry, entt::entity entity);
		void ConnectJointSignals(entt::registry& registry);
		void DisconnectJointSignals(entt::registry& registry);
		// Brings the joints in step with the scene and the bodies (PhysicsSystem.h's M37 paragraph); nothing unless
		// JointsChanged.
		void SynchronizeJoints();
		// Rebuild's hook: keeps `composition` (the pass's bodies) for the joints' plans and synchronizes every joint over it.
		void SynchronizeJointsAfterRebuild(const PhysicsComposition& composition);
		// Destroys every live joint of the body `handle` (RemoveBody, before the body) and marks the joints for the next
		// synchronization.
		void RemoveBodyJoints(BodyHandle handle);
		// The break pass at the start of PostStep: fills LastJointBreaks.
		void BreakJoints(uint64_t tick);
		// Delivers LastJointBreaks to the listener (the end of PostStep).
		void DispatchJointBreaks();
		// FlushDestroyed's part: the joints of `candidates` that left play (a destroyed entity's entry goes, a disabled one's
		// keeps its frames).
		void RemoveLeavingJoints(const std::set<UUID>& candidates);
		// The joint part of the state hash (AppendStateHash).
		void AppendJointHash(std::vector<std::byte>& bytes) const;
		// Whether any joint has kept frames (the hash's joint part is not empty).
		[[nodiscard]] bool HasKeptJointFrames() const;
		// Destroys every live joint (the session's end, before the bodies); the world's destructor would too.
		void DestroyAllJoints();
	};

}
