#include "EnginePCH.h"
#include "Engine/Physics/CharacterController.h"

#include "Engine/Core/Assert.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/Private/CharacterContactRecorder.h"
#include "Engine/Physics/Private/PhysicsConversions.h"
#include "Engine/Physics/Private/PhysicsWorldState.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Character/CharacterID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// The character controller (Architecture §9.6) over a JPH::CharacterVirtual.
//
// The inner body. §9.6 gives the character a kinematic inner body (a capsule of the controller's size on its layer, with
// an ID from the canonical body-ID sequence) so that raycasts, overlaps and sensors see it. The controller creates it
// through PhysicsWorld::CreateBody, as the world's next body, rather than through CharacterVirtualSettings::mInnerBodyShape:
// that way it is one of the world's bodies like any other (its user data, layer, motion type and collision group are the
// world's, PhysicsWorld::IsBodyValid and GetUserData answer for it, it counts against the world's limit and in
// PhysicsEngine::GetLiveBodyCount, and the world's GroupFilter applies to it), and its ID is the same canonical one
// mInnerBodyIDOverride would have to name. The controller then does what CharacterVirtual does for an inner body it owns:
// it places the body at the character's padded base after every update and teleport (SetPositionAndRotation without
// activation; the body never sleeps), it keeps the character's own collision from seeing it (the body filter), and it
// removes it with the character. M34 (Docs/Decisions/0049-m34-characters-input.md decision 1): the body's object layer
// has the Character kind (PhysicsLayers.h), which pairs with sensors only, so the solver never resolves a contact between
// it and a Dynamic body: placed rather than swept, it would push that body with infinite mass (World Tree's brief 11:
// shards driven through the floor, and a character launched by the shard it stood on). The character's own collision
// meets other characters' inner bodies as it did before (CharacterObjectLayerFilter), so characters still block each other.
//
// Dynamic bodies (M34, ADR 0049 decision 2). The character pushes Dynamic bodies through CharacterVirtual's own contacts
// alone, with a strength of its Mass times PushAcceleration. A Dynamic body lighter than the character
// (IsLighterThanCharacter) never moves it: it cannot push it or carry it up a surface of its own (the recorder), it gives
// it none of its velocity as its ground (Update), it is never a stair (WalkStairs, Jolt's step without light bodies) and
// it carries at most its own weight from the character (PushGround, which replaces CharacterVirtual's weight impulse).
//
// Contacts. A CharacterContactRecorder records the character's contacts during ExtendedUpdate; Update then appends each as
// a ContactEvent with FromCharacter, BodyA the inner body, ColliderA 0 (the capsule is the character's only collider) and
// SubShapeA CharacterContactSubShape. A sensor touching the inner body is reported by the world's own listener (inner body
// versus sensor, SubShapeA the capsule's empty sub-shape ID, all bits set), which the character's recorder refuses; the
// two sources never share a sub-shape key, and each reports every removal of what it added, so the session's per-pair
// reference count stays exact.
//
// Surfaces (M36, Docs/Decisions/0053-m36-contact-surfaces.md decision 4). The collider a character stands on is Jolt's
// ground body and sub-shape of the update (resolved as a contact's, ResolvePhysicsCollider), and its surface velocity
// (PhysicsBodyRecord::Surfaces) joins the ground's velocity, turned by the body's rotation when it is read: at the start of
// Update for the step's velocity (the ground of the last update, where it is now), at its end for the ground state it
// reports (the new ground). Without a surface the update computes exactly what it did before M36.
//
// The squeeze (M36 W36.2, ADR 0053 decision 12). After the update, the contacts of its last pass (GetActiveContacts) name the
// bodies that push the character: not sensors or discarded, able to push it (CharacterContactSettings::mCanPushCharacter,
// which the recorder turns off for a light Dynamic body), not Static, and moving towards it (the contact point's velocity
// along the contact's normal, which points at the character) faster than MinPushSpeed. Their contacts' distances are those
// of the pass's start, so the controller measures them again where the update left the character: one CheckCollision of the
// character's capsule against those bodies alone. A body whose push over the step (its speed times the step) would end more
// than CharacterSqueezeTolerance inside the padded capsule squeezes it; the update could not keep the character ahead of it.
//
// Range. A character never leaves the range physics places bodies in (IsPlaceablePhysicsPose): an update that would
// carry it beyond MaxPhysicsCoordinate leaves it where it was, with no velocity, and logs a warning once per controller.
// The weight the character puts on what it stands on (its mass times the gravity times the step, PushGround) is capped at
// MaxGroundImpulse, so no mass and gravity within their bounds can overflow a body's velocity.

namespace Engine {

	namespace Utils {

		// The SubShapeA of the character's records: the character side has no Jolt sub-shape. 0 differs from the inner body's
		// own sub-shape ID in the world's records (the capsule is not a compound, so Jolt reports the empty ID, all bits set).
		constexpr uint32_t CharacterContactSubShape = 0;

		// The ground's speed away from the character, in m/s along the up axis, below which a character on walkable ground
		// counts as grounded (Jolt's sample rule): above it the character is leaving the ground, as in the steps after a
		// jump, and keeps its own vertical velocity.
		constexpr float LeavingGroundSpeed = 0.1f;

		// The squared length (m/s^2) below which a gravity counts as none (CharacterController::Update).
		constexpr float MinGravityLengthSq = 1.0e-20f;

		// The largest impulse, in N s, that one update's weight may put on the body the character stands on (PushGround:
		// mass * gravity * deltaTime): on the lightest body (1 g, 2 mm across) a larger one could make a velocity whose
		// squared length overflows, which Jolt asserts on. A character of MaxPhysicsMass under the Earth's gravity at 1 Hz
		// stays just below it.
		constexpr double MaxGroundImpulse = 1.0e10;

		// M34 (Docs/Decisions/0049-m34-characters-input.md decision 2): the acceleration whose force on the character's Mass
		// is the most it pushes a Dynamic body with (CharacterVirtual's strength, in N): the force of its weight under the
		// Earth's gravity, whatever the world's gravity, so a character pushes what is lighter than it on any ground whose
		// friction coefficient is at most 1.
		constexpr float PushAcceleration = 9.81f;

		// `velocity` shortened to at most MaxCharacterSpeed (a terminal speed: a fall under a large gravity never builds a
		// velocity that carries the character out of Jolt's bounds). Scaled by its largest component first, so a long but
		// finite vector never overflows; a vector that is not finite stops the character.
		[[nodiscard]] static JPH::Vec3 ClampSpeed(JPH::Vec3Arg velocity)
		{
			const JPH::Vec3 magnitude = velocity.Abs();
			const float largest = std::max(magnitude.GetX(), std::max(magnitude.GetY(), magnitude.GetZ()));
			if (!std::isfinite(largest))
				return JPH::Vec3::sZero();
			if (largest <= MaxCharacterSpeed / 2.0f)
				return velocity; // |velocity| <= sqrt(3) * largest < MaxCharacterSpeed
			const JPH::Vec3 direction = (velocity / largest).Normalized();
			const float length = largest * (velocity / largest).Length();
			return length > MaxCharacterSpeed ? direction * MaxCharacterSpeed : velocity;
		}

		[[nodiscard]] static Status ValidateDescription(const PhysicsWorld& world, const CharacterControllerDescription& description)
		{
			const bool finite = std::isfinite(description.Height) && std::isfinite(description.Radius) && std::isfinite(description.MaxSlopeAngle)
				&& std::isfinite(description.StepHeight) && std::isfinite(description.Mass) && Detail::IsFinite(description.Pose.Position)
				&& Detail::IsFinite(description.Pose.Rotation);
			if (!finite)
				return MakeError(ErrorCode::InvalidArgument, "the character controller's description holds a value that is not finite");
			if (!IsWithinPhysicsRange(description.Pose.Position))
			{
				return MakeError(ErrorCode::InvalidArgument, "the character's position ({}, {}, {}) is out of range: each coordinate must be at most {} m",
					description.Pose.Position.x, description.Pose.Position.y, description.Pose.Position.z, MaxPhysicsCoordinate);
			}
			if (!IsPhysicsUnitRotation(description.Pose.Rotation))
				return MakeError(ErrorCode::InvalidArgument, "the character controller's rotation is not a unit quaternion (length {})",
					glm::length(description.Pose.Rotation));
			if (!(description.Radius > 0.0f) || !(description.Height > 2.0f * description.Radius))
			{
				return std::unexpected(Error(ErrorCode::Validation,
					std::format("{}: the character's capsule needs a radius above 0 and a height above twice its radius (height {} m, radius {} m)",
						PhysicsInvalidShapeCode, description.Height, description.Radius))
						.WithHint("make Height greater than twice Radius"));
			}
			if (description.MaxSlopeAngle < 0.0f || description.MaxSlopeAngle > 90.0f)
				return MakeError(ErrorCode::InvalidArgument, "the character's MaxSlopeAngle {} is outside [0, 90] degrees", description.MaxSlopeAngle);
			if (description.StepHeight < 0.0f)
				return MakeError(ErrorCode::InvalidArgument, "the character's StepHeight {} is negative", description.StepHeight);
			if (!(description.Mass > 0.0f) || description.Mass > MaxPhysicsMass)
				return MakeError(ErrorCode::InvalidArgument, "the character's Mass must be above 0 and at most {} kg (got {})", MaxPhysicsMass, description.Mass);
			const uint32_t layerCount = world.GetLayers().GetLayerCount();
			if (description.Layer >= layerCount)
				return MakeError(ErrorCode::InvalidArgument, "the character's layer {} is outside the world's {} layer(s)", description.Layer, layerCount);
			return {};
		}

		// The character's own collision with object layers (CharacterVirtual's queries): the pair rule of a Moving body on
		// the character's layer, with another character's inner body (the Character kind, which the solver pairs with
		// sensors only) met as the Moving body it is, so characters block each other as they did before M34.
		class CharacterObjectLayerFilter final : public JPH::ObjectLayerFilter
		{
		public:
			// `layers` is a documented back-reference to the world's table, which outlives the update that uses the filter.
			CharacterObjectLayerFilter(const PhysicsLayerTable& layers, uint32_t layer)
				: m_Layers(&layers), m_Layer(MakePhysicsObjectLayer(layer, PhysicsObjectKind::Moving))
			{
			}

			bool ShouldCollide(JPH::ObjectLayer layer) const override
			{
				const auto other = static_cast<PhysicsObjectLayer>(layer);
				const PhysicsObjectLayer seen = GetPhysicsObjectKind(other) == PhysicsObjectKind::Character
					? MakePhysicsObjectLayer(GetPhysicsLayerIndex(other), PhysicsObjectKind::Moving)
					: other;
				return m_Layers->ShouldCollide(m_Layer, seen);
			}
		private:
			const PhysicsLayerTable* m_Layers = nullptr;
			PhysicsObjectLayer m_Layer = 0;
		};

		// M36 W36.2: the speed, in m/s, at which a body's contact point must move towards the character for the body to push it
		// (FindSqueeze): far below any machine's, above the drift of a body at rest.
		constexpr float MinPushSpeed = 1.0e-3f;

		// M36 W36.2: the squeeze's query sees the pushing bodies alone (FindSqueeze).
		class PusherFilter final : public JPH::BodyFilter
		{
		public:
			// `pushers` is a documented back-reference to FindSqueeze's list, which outlives the query.
			explicit PusherFilter(const std::vector<std::pair<JPH::BodyID, float>>& pushers)
				: m_Pushers(&pushers)
			{
			}

			bool ShouldCollide(const JPH::BodyID& body) const override
			{
				return std::any_of(m_Pushers->begin(), m_Pushers->end(), [&body](const std::pair<JPH::BodyID, float>& pusher)
				{
					return pusher.first == body;
				});
			}
		private:
			const std::vector<std::pair<JPH::BodyID, float>>* m_Pushers = nullptr;
		};

		// M36 W36.2: the deepest penetration of each pushing body into the capsule the squeeze's query found (positive inside,
		// negative while apart within the query's reach). The narrow phase reports hits on the main thread in an order of its
		// own; the deepest of each body is the same whatever the order.
		class PenetrationCollector final : public JPH::CollideShapeCollector
		{
		public:
			void AddHit(const JPH::CollideShapeResult& result) override
			{
				m_Hits.emplace_back(result.mBodyID2, result.mPenetrationDepth);
			}

			// The deepest hit of `pushers[index]`'s body, or nullopt when the query met none.
			[[nodiscard]] std::optional<float> GetDepth(size_t index, const std::vector<std::pair<JPH::BodyID, float>>& pushers) const
			{
				std::optional<float> deepest;
				for (const auto& [body, depth] : m_Hits)
				{
					if (body == pushers[index].first && (!deepest.has_value() || depth > *deepest))
						deepest = depth;
				}
				return deepest;
			}
		private:
			std::vector<std::pair<JPH::BodyID, float>> m_Hits;
		};

		// The character's own collision (CharacterVirtual's queries): never its inner body, and no body of its collision
		// group, decided by the inner body's CollisionGroup as the world installed it (its GroupFilter), so the rule is the
		// world's own.
		class CharacterBodyFilter final : public JPH::BodyFilter
		{
		public:
			CharacterBodyFilter(const JPH::BodyID& innerBody, const JPH::CollisionGroup& group)
				: m_InnerBody(innerBody), m_Group(group)
			{
			}

			bool ShouldCollide(const JPH::BodyID& body) const override
			{
				return body != m_InnerBody;
			}

			bool ShouldCollideLocked(const JPH::Body& body) const override
			{
				return m_Group.CanCollide(body.GetCollisionGroup());
			}
		private:
			JPH::BodyID m_InnerBody{};
			JPH::CollisionGroup m_Group{};
		};

	}

	struct CharacterController::State
	{
		// The world the character lives in: a documented back-reference (CharacterController::Create).
		PhysicsWorld* World = nullptr;
		uint64_t UserData = 0;
		uint32_t Layer = 0;
		float StepHeight = 0.0f;
		// CharacterControllerDescription::InheritGroundVelocity: while grounded, Update adds the ground's horizontal velocity.
		bool InheritGroundVelocity = false;
		BodyHandle InnerBody{};
		// The inner body's collision group as the world installed it.
		JPH::CollisionGroup Group{};
		// The capsule, shared by the CharacterVirtual (its Jolt shape) and the inner body.
		Ref<const PhysicsShape> Shape{};
		Scope<CharacterContactRecorder> Contacts;
		// Declared after the recorder it reports to, so it is destroyed first.
		Scope<JPH::CharacterVirtual> Character;
		// Whether Update logged that it kept the character in range (once per controller).
		bool HasWarnedRange = false;
		// M36: the ground of the last update (CharacterGroundState::GroundBody, GroundCollider) and its collider's surface
		// velocity in world space as the update ended, when it has a surface (none on a light body).
		BodyHandle GroundBody{};
		uint32_t GroundCollider = 0;
		std::optional<glm::vec3> GroundSurfaceVelocity{};

		// M36 W36.2: the squeeze of the last update (GetSqueeze).
		CharacterSqueeze Squeeze{};

		// M36: a collider a character stands on (FindGroundCollider).
		struct GroundColliderInfo
		{
			uint32_t Collider = 0;
			// Its surface velocity in world space, when it has a surface.
			std::optional<glm::vec3> SurfaceVelocity{};
		};

		// Where the inner body goes: the character's base raised by its padding, as CharacterVirtual places an inner body.
		[[nodiscard]] PhysicsPose GetInnerBodyPose() const
		{
			const JPH::RVec3 position = Character->GetPosition() + Character->GetCharacterPadding() * Character->GetUp();
			return PhysicsPose{ .Position = Detail::ToGlm(position), .Rotation = Detail::ToGlm(Character->GetRotation()) };
		}

		// Moves the inner body to the character. The character stays where the world places bodies (Update, SetPose), so the
		// pose is always placeable; one that is not would leave the inner body where it is rather than reach the world.
		void PlaceInnerBody()
		{
			const PhysicsPose pose = GetInnerBodyPose();
			ENGINE_CORE_ASSERT(IsPlaceablePhysicsPose(pose), "CharacterController: the inner body's pose ({}, {}, {}) is out of range", pose.Position.x,
				pose.Position.y, pose.Position.z);
			if (IsPlaceablePhysicsPose(pose))
				World->SetPose(InnerBody, pose, false);
		}

		// Whether `body` is a Dynamic body lighter than the character (see the file comment).
		[[nodiscard]] bool IsLightBody(const JPH::BodyID& body) const
		{
			return IsLighterThanCharacter(World->m_State->System->GetBodyLockInterfaceNoLock(), body, Character->GetMass());
		}

		// M36: the collider behind `subShape` of `body` (ColliderShapeDescription::UserData) and its surface velocity turned
		// by the body's rotation now, when it has a surface; nothing for an invalid body or one that no longer exists. Main
		// thread, between steps.
		[[nodiscard]] GroundColliderInfo FindGroundCollider(const JPH::BodyID& body, const JPH::SubShapeID& subShape) const
		{
			if (body.IsInvalid())
				return {};
			const std::vector<Detail::PhysicsBodyRecord>& records = World->m_State->Bodies;
			const JPH::BodyLockRead lock(World->m_State->System->GetBodyLockInterfaceNoLock(), body);
			if (!lock.SucceededAndIsInBroadPhase())
				return {};
			const JPH::Body& ground = lock.GetBody();
			GroundColliderInfo found{ .Collider = Detail::ResolvePhysicsCollider(records, ground, subShape) };
			const Detail::PhysicsBodyRecord* record = Detail::FindPhysicsBodyRecord(records, body);
			const PhysicsColliderSurface* surface = record != nullptr ? Detail::FindColliderSurface(*record, found.Collider) : nullptr;
			if (surface != nullptr)
				found.SurfaceVelocity = Detail::ToGlm(ground.GetRotation() * Detail::ToJolt(surface->SurfaceVelocity));
			return found;
		}

		// Whether `contact` of the last update is a stair against a movement at `velocity`: CharacterVirtual::CanWalkStairs's
		// test (collided and kept, the character pushing into it, a surface too steep to walk), and not a light body.
		[[nodiscard]] bool IsStair(const JPH::CharacterContact& contact, JPH::Vec3Arg velocity) const
		{
			const bool steep = contact.mHadCollision && !contact.mWasDiscarded && contact.mSurfaceNormal.Dot(velocity - contact.mLinearVelocity) < 0.0f
				&& Character->IsSlopeTooSteep(contact.mSurfaceNormal);
			return steep && !(contact.mMotionTypeB == JPH::EMotionType::Dynamic && IsLightBody(contact.mBodyB));
		}

		// The stair step of CharacterVirtual::ExtendedUpdate, which Update runs with its own stairs off: when the update
		// moved the character less far along `desired` than it asked and a stair (IsStair) holds it back, WalkStairs up to
		// StepHeight towards the stair that opposes the movement most. A light body is never a stair, so the character
		// pushes it instead of climbing it.
		void WalkStairs(float deltaTime, JPH::Vec3Arg desired, JPH::RVec3Arg previousPosition, const JPH::CharacterVirtual::ExtendedUpdateSettings& settings,
			const JPH::BroadPhaseLayerFilter& broadPhaseFilter, const JPH::ObjectLayerFilter& objectLayerFilter, const JPH::BodyFilter& bodyFilter,
			const JPH::ShapeFilter& shapeFilter)
		{
			JPH::CharacterVirtual& character = *Character;
			const JPH::Vec3 up = character.GetUp();
			if (!(StepHeight > 0.0f) || !character.IsSupported())
				return;
			const JPH::Vec3 horizontal = desired - desired.Dot(up) * up;
			const JPH::Vec3 desiredStep = horizontal * deltaTime;
			const float desiredLength = desiredStep.Length();
			if (horizontal.IsNearZero(1.0e-6f) || !(desiredLength > 0.0f))
				return;
			const JPH::Vec3 forward = desiredStep / desiredLength;
			JPH::Vec3 achieved = JPH::Vec3(character.GetPosition() - previousPosition);
			achieved -= achieved.Dot(up) * up;
			const float achievedLength = std::max(0.0f, achieved.Dot(forward));
			if (achievedLength + 1.0e-4f >= desiredLength)
				return;

			const JPH::CharacterVirtual::ContactList& contacts = character.GetActiveContacts();
			const bool blocked = std::any_of(contacts.begin(), contacts.end(), [this, &horizontal](const JPH::CharacterContact& contact)
			{
				return IsStair(contact, horizontal);
			});
			if (!blocked)
				return;
			JPH::Vec3 direction = forward;
			float bestCosine = settings.mWalkStairsCosAngleForwardContact;
			for (const JPH::CharacterContact& contact : contacts)
			{
				if (!IsStair(contact, desired))
					continue;
				// The stair's normal made horizontal and turned to point along the movement.
				JPH::Vec3 towards = contact.mSurfaceNormal.Dot(up) * up - contact.mSurfaceNormal;
				const float length = towards.Length();
				if (!(length > 1.0e-6f))
					continue;
				towards /= length;
				const float cosine = towards.Dot(forward);
				if (cosine > bestCosine)
				{
					direction = towards;
					bestCosine = cosine;
				}
			}
			const JPH::Vec3 stepForward = direction * std::max(settings.mWalkStairsMinStepForward, desiredLength - achievedLength);
			const JPH::Vec3 stepForwardTest = direction * settings.mWalkStairsStepForwardTest;
			static_cast<void>(character.WalkStairs(deltaTime, up * StepHeight, stepForward, stepForwardTest, settings.mWalkStairsStepDownExtra,
				broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, *World->m_State->TempAllocator));
		}

		// The weight the character puts on the Dynamic body it stands on, as CharacterVirtual::Update puts it (its mass times
		// the gravity's part along the ground's normal, over the step, at the ground's contact point), which Update turns off
		// in Jolt: at most the body's own weight for a light body, and capped at MaxGroundImpulse.
		void PushGround(float deltaTime, const glm::vec3& gravity)
		{
			const JPH::CharacterVirtual& character = *Character;
			const JPH::BodyID ground = character.GetGroundBodyID();
			const JPH::Vec3 joltGravity = Detail::ToJolt(gravity);
			const float normalDotGravity = character.GetGroundNormal().Dot(joltGravity);
			if (ground.IsInvalid() || !(normalDotGravity < 0.0f))
				return;
			double mass = character.GetMass();
			{
				const JPH::BodyLockRead lock(World->m_State->System->GetBodyLockInterfaceNoLock(), ground);
				if (!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsDynamic())
					return;
				if (IsLighterThanCharacter(lock.GetBody(), character.GetMass()))
					mass = 1.0 / static_cast<double>(lock.GetBody().GetMotionProperties()->GetInverseMass());
			}
			// |impulse| = mass * |n . g| * deltaTime, along the gravity.
			const double gravityLength = glm::length(glm::dvec3(gravity));
			const double magnitude = std::min(mass * -static_cast<double>(normalDotGravity) * static_cast<double>(deltaTime), Utils::MaxGroundImpulse);
			const JPH::Vec3 impulse = joltGravity * static_cast<float>(magnitude / gravityLength);
			World->m_State->System->GetBodyInterfaceNoLock().AddImpulse(ground, impulse, character.GetGroundPosition());
		}

		// M36 W36.2: the squeeze of the update that just ran (see the file comment).
		[[nodiscard]] CharacterSqueeze FindSqueeze(float deltaTime, const JPH::BroadPhaseLayerFilter& broadPhaseFilter,
			const JPH::ObjectLayerFilter& objectLayerFilter) const
		{
			const JPH::CharacterVirtual& character = *Character;
			// The bodies that push the character this step, with the distance each push covers over it (the largest of the
			// body's contacts).
			std::vector<std::pair<JPH::BodyID, float>> pushers;
			for (const JPH::CharacterContact& contact : character.GetActiveContacts())
			{
				// mCanPushCharacter is the recorder's answer only for a contact the update collided with, so a light body is
				// asked here too.
				if (contact.mIsSensorB || contact.mWasDiscarded || !contact.mCanPushCharacter || contact.mBodyB.IsInvalid()
					|| contact.mMotionTypeB == JPH::EMotionType::Static || (contact.mMotionTypeB == JPH::EMotionType::Dynamic && IsLightBody(contact.mBodyB)))
				{
					continue;
				}
				const float speed = contact.mLinearVelocity.Dot(contact.mContactNormal);
				if (!(speed > Utils::MinPushSpeed))
					continue;
				const float push = speed * deltaTime;
				const auto found = std::find_if(pushers.begin(), pushers.end(), [&contact](const std::pair<JPH::BodyID, float>& pusher)
				{
					return pusher.first == contact.mBodyB;
				});
				if (found == pushers.end())
					pushers.emplace_back(contact.mBodyB, push);
				else
					found->second = std::max(found->second, push);
			}
			if (pushers.empty())
				return {};

			// Where the update left the character: each pusher's deepest penetration of the capsule (negative while apart),
			// within the farthest push.
			float farthest = 0.0f;
			for (const auto& [body, push] : pushers)
				farthest = std::max(farthest, push);
			Utils::PusherFilter bodyFilter(pushers);
			Utils::PenetrationCollector collector;
			character.CheckCollision(character.GetPosition(), character.GetRotation(), JPH::Vec3::sZero(), farthest, character.GetShape(),
				character.GetPosition(), collector, broadPhaseFilter, objectLayerFilter, bodyFilter, JPH::ShapeFilter());

			CharacterSqueeze squeeze;
			for (size_t index = 0; index < pushers.size(); ++index)
			{
				const std::optional<float> depth = collector.GetDepth(index, pushers);
				// The distance CharacterVirtual keeps (beyond its padding), less the push still to come.
				if (!depth.has_value() || !(*depth + character.GetCharacterPadding() + pushers[index].second > CharacterSqueezeTolerance))
					continue;
				const BodyHandle body = Detail::ToBodyHandle(pushers[index].first);
				if (!squeeze.IsSqueezed || body.GetValue() < squeeze.Body.GetValue())
					squeeze = CharacterSqueeze{ .IsSqueezed = true, .Body = body };
			}
			return squeeze;
		}

		// Appends the recorded contacts to the world's buffer (see the file comment).
		void AppendContacts()
		{
			const std::vector<CharacterContactRecord> records = Contacts->Drain();
			if (records.empty())
				return;
			ContactBuffer& buffer = World->GetContactBuffer();
			for (const CharacterContactRecord& record : records)
			{
				ContactEvent event{ .Kind = record.Kind,
					.BodyA = InnerBody,
					.BodyB = BodyHandle(record.Body.GetIndexAndSequenceNumber()),
					.SubShapeA = Utils::CharacterContactSubShape,
					.SubShapeB = record.SubShape.GetValue(),
					.FromCharacter = true };
				if (record.Kind == ContactEventKind::Added)
				{
					if (World->IsBodyValid(event.BodyB))
					{
						if (const Ref<const PhysicsShape> shape = World->GetShape(event.BodyB))
							event.ColliderB = PhysicsWorld::State::ResolveColliderUserData(*shape, event.SubShapeB);
					}
					event.Point = Detail::ToGlm(record.Point);
					event.Normal = Detail::ToGlm(record.Normal);
					event.RelativeNormalSpeed = record.RelativeNormalSpeed;
				}
				buffer.Append(event);
			}
		}
	};

	CharacterController::CharacterController(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	CharacterController::~CharacterController()
	{
		// The CharacterVirtual owns no inner body (see the file comment): the controller removes it.
		State& state = *m_State;
		if (state.World != nullptr && state.InnerBody.IsValid() && state.World->IsBodyValid(state.InnerBody))
			state.World->DestroyBody(state.InnerBody);
	}

	Result<Scope<CharacterController>> CharacterController::Create(PhysicsWorld& world, const CharacterControllerDescription& description)
	{
		ENGINE_TRY(Utils::ValidateDescription(world, description));

		// The capsule, its base at the body origin (§9.6: the entity's origin is the character's base).
		const ColliderShapeDescription capsule{ .Geometry = CapsuleShapeGeometry{ .HalfHeight = 0.5f * (description.Height - 2.0f * description.Radius),
													.Radius = description.Radius },
			.Position = glm::vec3(0.0f, 0.5f * description.Height, 0.0f) };
		Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { capsule } });
		if (!shape)
			return std::unexpected(std::move(shape).error().WithContext("while creating a character controller"));

		const glm::quat rotation = glm::normalize(description.Pose.Rotation);
		JPH::CharacterVirtualSettings settings;
		settings.SetEmbedded();
		settings.mShape = (*shape)->m_State->Shape;
		settings.mUp = JPH::Vec3::sAxisY();
		settings.mMaxSlopeAngle = JPH::DegreesToRadians(description.MaxSlopeAngle);
		settings.mMass = description.Mass;
		settings.mMaxStrength = description.Mass * Utils::PushAcceleration;
		// Contacts below the centre of the lower hemisphere support the character (the capsule's base is at the origin).
		settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -description.Radius);

		// The inner body, the world's next body (see the file comment). It never sleeps, so sensors keep seeing it.
		const BodyDescription innerBody{ .Shape = *shape,
			.MotionType = PhysicsMotionType::Kinematic,
			.Pose = { .Position = description.Pose.Position + glm::vec3(0.0f, settings.mCharacterPadding, 0.0f), .Rotation = rotation },
			.Layer = description.Layer,
			.AllowSleeping = false,
			.UserData = description.UserData,
			.CollisionGroup = description.CollisionGroup };
		Result<BodyHandle> inner = world.CreateBody(innerBody);
		if (!inner)
			return std::unexpected(std::move(inner).error().WithContext("while creating a character controller's inner body"));
		// The Character kind (see the file comment), set before any step can pair the new body with a Dynamic one.
		const JPH::BodyID innerID = Detail::ToJoltBodyID(*inner).value_or(JPH::BodyID());
		world.m_State->System->GetBodyInterface().SetObjectLayer(innerID, MakePhysicsObjectLayer(description.Layer, PhysicsObjectKind::Character));

		Scope<CharacterController> controller = CreateScope<CharacterController>(ConstructionKey());
		State& state = *controller->m_State;
		state.World = &world;
		state.UserData = description.UserData;
		state.Layer = description.Layer;
		state.StepHeight = description.StepHeight;
		state.InheritGroundVelocity = description.InheritGroundVelocity;
		state.InnerBody = *inner;
		state.Shape = std::move(*shape);
		{
			// The world's own body, just created, always reads; without its group the character would collide with the bodies
			// of its own group.
			const JPH::BodyLockRead lock(world.m_State->System->GetBodyLockInterface(), innerID);
			ENGINE_CORE_ASSERT(lock.Succeeded(), "CharacterController::Create: the inner body {} the world just created cannot be read", inner->GetValue());
			if (lock.Succeeded())
				state.Group = lock.GetBody().GetCollisionGroup();
		}

		// The character's ID only matters between characters, which never collide here; it is the inner body's, so it is
		// deterministic too.
		settings.mID = JPH::CharacterID(inner->GetValue());
		state.Contacts = CreateScope<CharacterContactRecorder>(world.m_State->System->GetBodyLockInterfaceNoLock());
		state.Character = CreateScope<JPH::CharacterVirtual>(&settings, Detail::ToJolt(description.Pose.Position), Detail::ToJoltRotation(rotation),
			description.UserData, world.m_State->System.get());
		state.Character->SetListener(state.Contacts.get());
		return controller;
	}

	void CharacterController::Update(float deltaTime, const glm::vec3& desiredVelocity, const glm::vec3& gravity)
	{
		const bool valid = std::isfinite(deltaTime) && deltaTime > 0.0f && Detail::IsFinite(desiredVelocity) && Detail::IsFinite(gravity);
		ENGINE_CORE_ASSERT(valid, "CharacterController::Update needs a finite delta time > 0 and finite vectors");
		if (!valid)
			return;

		State& state = *m_State;
		JPH::CharacterVirtual& character = *state.Character;
		// A gravity so weak that its squared length underflows counts as none: PushGround divides by its length.
		const glm::vec3 effectiveGravity = glm::dot(gravity, gravity) < Utils::MinGravityLengthSq ? glm::vec3(0.0f) : gravity;

		// The velocity for the step (CharacterController.h, ADR 0014 decision 8): the desired horizontal velocity, plus on
		// the ground its vertical velocity and the desired vertical part (a jump), else the previous vertical velocity plus
		// gravity; with InheritGroundVelocity, on the ground also the ground's horizontal velocity (ADR 0030 decision 36), so
		// a platform moving at v carries the character v * t. The platform the character stands on may have moved since the
		// last update, so its velocity is read first; a light body's is none (M34, see the file comment), so a shard the
		// character stands on never launches it.
		// M36 (ADR 0053 decision 4): the surface velocity of the collider the character stands on joins the ground's velocity,
		// its vertical part as every ground's, and its horizontal part carries a grounded character whatever
		// InheritGroundVelocity says (a belt). A light body gives none, as it gives no velocity of its own.
		character.UpdateGroundVelocity();
		const JPH::Vec3 up = character.GetUp();
		const JPH::Vec3 desired = Detail::ToJolt(desiredVelocity);
		const JPH::Vec3 desiredVertical = desired.Dot(up) * up;
		const JPH::Vec3 currentVertical = character.GetLinearVelocity().Dot(up) * up;
		const bool lightGround = state.IsLightBody(character.GetGroundBodyID());
		const std::optional<glm::vec3> groundSurface =
			lightGround ? std::nullopt : state.FindGroundCollider(character.GetGroundBodyID(), character.GetGroundSubShapeID()).SurfaceVelocity;
		JPH::Vec3 groundVelocity = lightGround ? JPH::Vec3::sZero() : character.GetGroundVelocity();
		if (groundSurface.has_value())
			groundVelocity += Detail::ToJolt(*groundSurface);
		const JPH::Vec3 groundVertical = groundVelocity.Dot(up) * up;
		const bool grounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround
			&& (currentVertical - groundVertical).Dot(up) < Utils::LeavingGroundSpeed;
		const JPH::Vec3 vertical = grounded ? groundVertical + desiredVertical : currentVertical + Detail::ToJolt(effectiveGravity) * deltaTime;
		JPH::Vec3 carried = JPH::Vec3::sZero();
		if (grounded && state.InheritGroundVelocity)
		{
			carried = groundVelocity - groundVertical;
		}
		else if (grounded && groundSurface.has_value())
		{
			const JPH::Vec3 surface = Detail::ToJolt(*groundSurface);
			carried = surface - surface.Dot(up) * up;
		}
		const JPH::Vec3 stepVelocity = Utils::ClampSpeed(desired - desiredVertical + carried + vertical);
		character.SetLinearVelocity(stepVelocity);

		// Stick to the floor over steps and slopes down to StepHeight; the stairs up to it are the controller's own step
		// (State::WalkStairs), which leaves light bodies out.
		JPH::CharacterVirtual::ExtendedUpdateSettings update;
		update.mWalkStairsStepUp = JPH::Vec3::sZero();
		update.mStickToFloorStepDown = -up * state.StepHeight;

		JPH::PhysicsSystem& system = *state.World->m_State->System;
		const JPH::ObjectLayer layer = MakePhysicsObjectLayer(state.Layer, PhysicsObjectKind::Moving);
		const JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter = system.GetDefaultBroadPhaseLayerFilter(layer);
		const Utils::CharacterObjectLayerFilter objectLayerFilter(state.World->GetLayers(), state.Layer);
		const Utils::CharacterBodyFilter bodyFilter(Detail::ToJoltBodyID(state.InnerBody).value_or(JPH::BodyID()), state.Group);
		const JPH::ShapeFilter shapeFilter;
		const JPH::RVec3 previousPosition = character.GetPosition();
		// No gravity reaches Jolt, whose only use of it is the weight on the ground: PushGround puts that weight below.
		character.ExtendedUpdate(deltaTime, JPH::Vec3::sZero(), update, broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter,
			*state.World->m_State->TempAllocator);
		state.WalkStairs(deltaTime, stepVelocity, previousPosition, update, broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter);

		// Never beyond the range the world places bodies in (see the file comment).
		if (!IsWithinPhysicsRange(Detail::ToGlm(character.GetPosition())))
		{
			if (!state.HasWarnedRange)
			{
				state.HasWarnedRange = true;
				ENGINE_CORE_WARN("CharacterController: the character of user data {:#x} stops at ({}, {}, {}): it would leave the {} m range physics handles",
					state.UserData, previousPosition.GetX(), previousPosition.GetY(), previousPosition.GetZ(), MaxPhysicsCoordinate);
			}
			character.SetPosition(previousPosition);
			character.SetLinearVelocity(JPH::Vec3::sZero());
		}

		state.PushGround(deltaTime, effectiveGravity);
		state.PlaceInnerBody();
		state.AppendContacts();

		// M36 W36.2: the squeeze, measured where the update left the character (see the file comment).
		state.Squeeze = state.FindSqueeze(deltaTime, broadPhaseFilter, objectLayerFilter);

		// M36: the ground this update found, for GetGroundState (see the file comment).
		state.GroundBody = BodyHandle();
		state.GroundCollider = 0;
		state.GroundSurfaceVelocity.reset();
		if (character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround && !character.GetGroundBodyID().IsInvalid())
		{
			const State::GroundColliderInfo ground = state.FindGroundCollider(character.GetGroundBodyID(), character.GetGroundSubShapeID());
			state.GroundBody = Detail::ToBodyHandle(character.GetGroundBodyID());
			state.GroundCollider = ground.Collider;
			if (!state.IsLightBody(character.GetGroundBodyID()))
				state.GroundSurfaceVelocity = ground.SurfaceVelocity;
		}
	}

	PhysicsPose CharacterController::GetPose() const
	{
		const JPH::CharacterVirtual& character = *m_State->Character;
		return PhysicsPose{ .Position = Detail::ToGlm(character.GetPosition()), .Rotation = Detail::ToGlm(character.GetRotation()) };
	}

	void CharacterController::SetPose(const PhysicsPose& pose)
	{
		const bool valid = IsPlaceablePhysicsPose(pose);
		ENGINE_CORE_ASSERT(valid, "CharacterController::SetPose needs a position within MaxPhysicsCoordinate and a unit rotation");
		if (!valid)
			return;

		State& state = *m_State;
		state.Character->SetPosition(Detail::ToJolt(pose.Position));
		state.Character->SetRotation(Detail::ToJoltRotation(glm::normalize(pose.Rotation)));
		state.PlaceInnerBody();
	}

	glm::vec3 CharacterController::GetVelocity() const
	{
		return Detail::ToGlm(m_State->Character->GetLinearVelocity());
	}

	void CharacterController::SetVelocity(const glm::vec3& velocity)
	{
		const bool valid = Detail::IsFinite(velocity);
		ENGINE_CORE_ASSERT(valid, "CharacterController::SetVelocity needs a finite velocity");
		if (!valid)
			return;
		m_State->Character->SetLinearVelocity(Detail::ToJolt(velocity));
	}

	CharacterGroundState CharacterController::GetGroundState() const
	{
		const JPH::CharacterVirtual& character = *m_State->Character;
		if (character.GetGroundState() != JPH::CharacterBase::EGroundState::OnGround)
			return {};
		const JPH::Vec3 normal = character.GetGroundNormal();
		const bool usable = Detail::IsFinite(Detail::ToGlm(normal)) && normal.LengthSq() > 0.25f;
		glm::vec3 groundVelocity = Detail::ToGlm(character.GetGroundVelocity());
		if (m_State->GroundSurfaceVelocity.has_value())
			groundVelocity += *m_State->GroundSurfaceVelocity;
		return CharacterGroundState{ .IsGrounded = true,
			.GroundNormal = usable ? Detail::ToGlm(normal.Normalized()) : glm::vec3(0.0f, 1.0f, 0.0f),
			.GroundVelocity = groundVelocity,
			.GroundBody = m_State->GroundBody,
			.GroundCollider = m_State->GroundCollider };
	}

	CharacterSqueeze CharacterController::GetSqueeze() const
	{
		return m_State->Squeeze;
	}

	BodyHandle CharacterController::GetInnerBody() const
	{
		return m_State->InnerBody;
	}

	uint64_t CharacterController::GetUserData() const
	{
		return m_State->UserData;
	}

	void CharacterController::SetInheritGroundVelocity(bool inherit)
	{
		m_State->InheritGroundVelocity = inherit;
	}

}
