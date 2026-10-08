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
#include <Jolt/Physics/Character/CharacterID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <algorithm>
#include <cmath>
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
// removes it with the character.
//
// Contacts. A CharacterContactRecorder records the character's contacts during ExtendedUpdate; Update then appends each as
// a ContactEvent with FromCharacter, BodyA the inner body, ColliderA 0 (the capsule is the character's only collider) and
// SubShapeA CharacterContactSubShape. A dynamic body touching the inner body is also reported by the world's own listener
// (inner body versus body, SubShapeA the capsule's empty sub-shape ID, all bits set); the two sources never share a
// sub-shape key, and each reports every removal of what it added, so the session's per-pair reference count stays exact.
//
// Range. A character never leaves the range physics places bodies in (IsPlaceablePhysicsPose): an update that would
// carry it beyond MaxPhysicsCoordinate leaves it where it was, with no velocity, and logs a warning once per controller.
// The weight Jolt applies to what the character stands on (its mass times the gravity times the step, CharacterVirtual::
// Update) is capped at MaxGroundImpulse, so no mass and gravity within their bounds can overflow a light body's velocity.

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

		// The largest impulse, in N s, that one update's weight may put on the body the character stands on: Jolt applies
		// mass * gravity * deltaTime there (CharacterVirtual::Update), and on the lightest body (1 g, 2 mm across) a larger
		// one could make a velocity whose squared length overflows, which Jolt asserts on. A character of MaxPhysicsMass
		// under the Earth's gravity at 1 Hz stays just below it.
		constexpr double MaxGroundImpulse = 1.0e10;

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
		BodyHandle InnerBody{};
		// The inner body's collision group as the world installed it.
		JPH::CollisionGroup Group{};
		// The capsule, shared by the CharacterVirtual (its Jolt shape) and the inner body.
		Ref<const PhysicsShape> Shape{};
		CharacterContactRecorder Contacts;
		// Declared after the recorder it reports to, so it is destroyed first.
		Scope<JPH::CharacterVirtual> Character;
		// Whether Update logged that it kept the character in range (once per controller).
		bool HasWarnedRange = false;

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

		// Appends the recorded contacts to the world's buffer (see the file comment).
		void AppendContacts()
		{
			const std::vector<CharacterContactRecord> records = Contacts.Drain();
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

		Scope<CharacterController> controller = CreateScope<CharacterController>(ConstructionKey());
		State& state = *controller->m_State;
		state.World = &world;
		state.UserData = description.UserData;
		state.Layer = description.Layer;
		state.StepHeight = description.StepHeight;
		state.InnerBody = *inner;
		state.Shape = std::move(*shape);
		{
			// The world's own body, just created, always reads; without its group the character would collide with the bodies
			// of its own group.
			const JPH::BodyLockRead lock(world.m_State->System->GetBodyLockInterface(), Detail::ToJoltBodyID(*inner).value_or(JPH::BodyID()));
			ENGINE_CORE_ASSERT(lock.Succeeded(), "CharacterController::Create: the inner body {} the world just created cannot be read", inner->GetValue());
			if (lock.Succeeded())
				state.Group = lock.GetBody().GetCollisionGroup();
		}

		// The character's ID only matters between characters, which never collide here; it is the inner body's, so it is
		// deterministic too.
		settings.mID = JPH::CharacterID(inner->GetValue());
		state.Character = CreateScope<JPH::CharacterVirtual>(&settings, Detail::ToJolt(description.Pose.Position), Detail::ToJoltRotation(rotation),
			description.UserData, world.m_State->System.get());
		state.Character->SetListener(&state.Contacts);
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
		// A gravity so weak that its squared length underflows counts as none: CharacterVirtual divides by its length when it
		// pushes the body the character stands on.
		const glm::vec3 effectiveGravity = glm::dot(gravity, gravity) < Utils::MinGravityLengthSq ? glm::vec3(0.0f) : gravity;

		// The velocity for the step (CharacterController.h, ADR 0014 decision 8): the desired horizontal velocity, plus on
		// the ground its vertical velocity and the desired vertical part (a jump), else the previous vertical velocity plus
		// gravity. The platform the character stands on may have moved since the last update.
		character.UpdateGroundVelocity();
		const JPH::Vec3 up = character.GetUp();
		const JPH::Vec3 desired = Detail::ToJolt(desiredVelocity);
		const JPH::Vec3 desiredVertical = desired.Dot(up) * up;
		const JPH::Vec3 currentVertical = character.GetLinearVelocity().Dot(up) * up;
		const JPH::Vec3 groundVertical = character.GetGroundVelocity().Dot(up) * up;
		const bool grounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround
			&& (currentVertical - groundVertical).Dot(up) < Utils::LeavingGroundSpeed;
		const JPH::Vec3 vertical = grounded ? groundVertical + desiredVertical : currentVertical + Detail::ToJolt(effectiveGravity) * deltaTime;
		character.SetLinearVelocity(Utils::ClampSpeed(desired - desiredVertical + vertical));

		// Walk stairs up to StepHeight, and stick to the floor over steps and slopes down to the same height.
		JPH::CharacterVirtual::ExtendedUpdateSettings update;
		update.mWalkStairsStepUp = up * state.StepHeight;
		update.mStickToFloorStepDown = -up * state.StepHeight;

		JPH::PhysicsSystem& system = *state.World->m_State->System;
		const JPH::ObjectLayer layer = MakePhysicsObjectLayer(state.Layer, PhysicsObjectKind::Moving);
		const JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter = system.GetDefaultBroadPhaseLayerFilter(layer);
		const JPH::DefaultObjectLayerFilter objectLayerFilter = system.GetDefaultLayerFilter(layer);
		const Utils::CharacterBodyFilter bodyFilter(Detail::ToJoltBodyID(state.InnerBody).value_or(JPH::BodyID()), state.Group);
		const JPH::ShapeFilter shapeFilter;
		// The gravity Jolt sees only pushes the ground (see the file comment): shortened so the weight stays within
		// MaxGroundImpulse. It never underflows: the cap allows at least 10 m/s^2 at MaxPhysicsMass over a whole second.
		JPH::Vec3 groundGravity = Detail::ToJolt(effectiveGravity);
		const double weight = static_cast<double>(character.GetMass()) * glm::length(glm::dvec3(effectiveGravity)) * static_cast<double>(deltaTime);
		if (weight > Utils::MaxGroundImpulse)
			groundGravity = groundGravity * static_cast<float>(Utils::MaxGroundImpulse / weight);
		const JPH::RVec3 previousPosition = character.GetPosition();
		character.ExtendedUpdate(deltaTime, groundGravity, update, broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter,
			*state.World->m_State->TempAllocator);

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

		state.PlaceInnerBody();
		state.AppendContacts();
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
		return CharacterGroundState{ .IsGrounded = true,
			.GroundNormal = usable ? Detail::ToGlm(normal.Normalized()) : glm::vec3(0.0f, 1.0f, 0.0f),
			.GroundVelocity = Detail::ToGlm(character.GetGroundVelocity()) };
	}

	BodyHandle CharacterController::GetInnerBody() const
	{
		return m_State->InnerBody;
	}

	uint64_t CharacterController::GetUserData() const
	{
		return m_State->UserData;
	}

}
