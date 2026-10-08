#include "EnginePCH.h"
#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/Private/PhysicsConversions.h"
#include "Engine/Physics/Private/PhysicsEdgeRemoval.h"
#include "Engine/Physics/Private/PhysicsEngineState.h"
#include "Engine/Physics/Private/PhysicsMassProperties.h"
#include "Engine/Physics/Private/PhysicsWorldState.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>

#include <limits>

// The world's queries are in PhysicsWorldQueries.cpp.

namespace Engine {

	namespace {

		// A body's identity and motion type, read under its lock.
		struct LiveBody
		{
			JPH::BodyID ID{};
			JPH::EMotionType MotionType = JPH::EMotionType::Static;
		};

	}

	namespace Utils {

		// The engine's DOF flags are Jolt's EAllowedDOFs bit for bit.
		static_assert(std::to_underlying(PhysicsDofs::TranslationX) == std::to_underlying(JPH::EAllowedDOFs::TranslationX));
		static_assert(std::to_underlying(PhysicsDofs::RotationZ) == std::to_underlying(JPH::EAllowedDOFs::RotationZ));
		static_assert(std::to_underlying(PhysicsDofs::All) == std::to_underlying(JPH::EAllowedDOFs::All));

		// Which motion types a call accepts (RequireBody).
		constexpr uint8_t StaticBodies = 1u << 0;
		constexpr uint8_t KinematicBodies = 1u << 1;
		constexpr uint8_t DynamicBodies = 1u << 2;
		constexpr uint8_t MovingBodies = KinematicBodies | DynamicBodies;
		constexpr uint8_t AnyBodies = StaticBodies | MovingBodies;

		// The fixed mass properties of every Kinematic body (BodyDescription::Mass): 1 kg with the inertia of a solid unit
		// sphere (2/5 m r^2). Jolt never uses a kinematic body's mass in contacts, and its shape's (none for a MeshShape) is
		// never asked for.
		constexpr float KinematicMass = 1.0f;
		constexpr float KinematicInertia = 0.4f;

		[[nodiscard]] static uint8_t GetMotionTypeBit(JPH::EMotionType type)
		{
			switch (type)
			{
				case JPH::EMotionType::Static:    return StaticBodies;
				case JPH::EMotionType::Kinematic: return KinematicBodies;
				case JPH::EMotionType::Dynamic:   return DynamicBodies;
			}

			ENGINE_CORE_ASSERT(false, "Unknown EMotionType {}", std::to_underlying(type));
			return 0;
		}

		[[nodiscard]] static PhysicsMotionType ToPhysicsMotionType(JPH::EMotionType type)
		{
			switch (type)
			{
				case JPH::EMotionType::Static:    return PhysicsMotionType::Static;
				case JPH::EMotionType::Kinematic: return PhysicsMotionType::Kinematic;
				case JPH::EMotionType::Dynamic:   return PhysicsMotionType::Dynamic;
			}

			ENGINE_CORE_ASSERT(false, "Unknown EMotionType {}", std::to_underlying(type));
			return PhysicsMotionType::Static;
		}

		[[nodiscard]] static JPH::EMotionType ToJoltMotionType(PhysicsMotionType type)
		{
			switch (type)
			{
				case PhysicsMotionType::Static:    return JPH::EMotionType::Static;
				case PhysicsMotionType::Kinematic: return JPH::EMotionType::Kinematic;
				case PhysicsMotionType::Dynamic:   return JPH::EMotionType::Dynamic;
			}

			ENGINE_CORE_ASSERT(false, "Unknown PhysicsMotionType {}", std::to_underlying(type));
			return JPH::EMotionType::Static;
		}

		[[nodiscard]] static bool IsKnownMotionType(PhysicsMotionType type)
		{
			return type == PhysicsMotionType::Static || type == PhysicsMotionType::Kinematic || type == PhysicsMotionType::Dynamic;
		}

		[[nodiscard]] static bool IsKnownMotionQuality(PhysicsMotionQuality quality)
		{
			return quality == PhysicsMotionQuality::Discrete || quality == PhysicsMotionQuality::LinearCast;
		}

		// `body`'s identity and motion type when it names a live body of `system`, under the body's lock.
		[[nodiscard]] static std::optional<LiveBody> FindLiveBody(const JPH::PhysicsSystem& system, BodyHandle body)
		{
			const std::optional<JPH::BodyID> id = Detail::ToJoltBodyID(body);
			if (!id.has_value())
				return std::nullopt;
			const JPH::BodyLockRead lock(system.GetBodyLockInterface(), *id);
			if (!lock.Succeeded())
				return std::nullopt;
			return LiveBody{ .ID = *id, .MotionType = lock.GetBody().GetMotionType() };
		}

		// The live body `body` names when its motion type is one of `allowed`. Anything else is a programmer error of the
		// caller, which validates its input first (PhysicsWorld.h): asserted, and nullopt, so the call is ignored.
		[[nodiscard]] static std::optional<LiveBody> RequireBody(const JPH::PhysicsSystem& system, BodyHandle body, uint8_t allowed, std::string_view operation)
		{
			const std::optional<LiveBody> live = FindLiveBody(system, body);
			ENGINE_CORE_ASSERT(live.has_value(), "PhysicsWorld::{}: 0x{:08x} names no live body of this world", operation, body.GetValue());
			if (!live.has_value())
				return std::nullopt;
			const bool isAllowed = (GetMotionTypeBit(live->MotionType) & allowed) != 0;
			ENGINE_CORE_ASSERT(isAllowed, "PhysicsWorld::{} does not apply to a {} body", operation,
				PhysicsMotionTypeToString(ToPhysicsMotionType(live->MotionType)));
			if (!isAllowed)
				return std::nullopt;
			return live;
		}

		// The length of `vector` in double precision, so a long float vector's length never overflows; infinity for a vector
		// that is not finite.
		[[nodiscard]] static double GetLength(JPH::Vec3Arg vector)
		{
			const double x = vector.GetX();
			const double y = vector.GetY();
			const double z = vector.GetZ();
			const double length = std::sqrt(x * x + y * y + z * z);
			return std::isfinite(length) ? length : std::numeric_limits<double>::infinity();
		}

		// `vector` scaled by `factor` (0 <= factor <= 1) in double precision.
		[[nodiscard]] static JPH::Vec3 Scale(JPH::Vec3Arg vector, double factor)
		{
			return JPH::Vec3(static_cast<float>(vector.GetX() * factor), static_cast<float>(vector.GetY() * factor), static_cast<float>(vector.GetZ() * factor));
		}

		// `velocity` shortened, when it is longer than `maximum`, to just below it: Jolt asserts that a velocity it is given is
		// at most the body's maximum, and its own clamping can round to a hair above it. A velocity that is not finite stops.
		[[nodiscard]] static JPH::Vec3 ClampVelocity(JPH::Vec3Arg velocity, float maximum)
		{
			const double length = GetLength(velocity);
			const double limit = static_cast<double>(maximum) * (1.0 - 1.0e-5);
			if (length <= limit)
				return velocity;
			if (!std::isfinite(length))
				return JPH::Vec3::sZero();
			return Scale(velocity, limit / length);
		}

		// The largest velocity change, in m/s or rad/s, that the forces and torques accumulated on a body may make in one step:
		// far beyond any body's maximum velocity (the step clamps to it anyway), and small enough that Jolt's squared velocity
		// lengths stay finite whatever the body's mass and inertia (Jolt asserts on an infinite one). The body functions accept
		// forces and lever arms whose product with a tiny body's inverse inertia would overflow otherwise.
		constexpr double MaxVelocityChangePerStep = 1.0e9;

		// The largest maximum velocity (m/s and rad/s) and gravity factor magnitude a body gets: BodyDescription's values are
		// clamped to them (the registry gives them no upper bound), so neither a body's velocity nor its step's gravity can
		// grow until Jolt's squared lengths overflow (with gravity components of at most MaxPhysicsGravity, a second of
		// gravity at the largest factor changes a velocity by less than 2e15 m/s).
		constexpr float MaxBodySpeed = 1.0e6f;
		constexpr float MaxGravityFactor = 1.0e3f;

		// InvalidArgument unless every component of `gravity` is finite and at most MaxPhysicsGravity in magnitude.
		[[nodiscard]] static Status CheckGravity(const glm::vec3& gravity)
		{
			if (!Detail::IsFinite(gravity))
				return MakeError(ErrorCode::InvalidArgument, "the gravity ({}, {}, {}) is not finite", gravity.x, gravity.y, gravity.z);
			if (std::abs(gravity.x) > MaxPhysicsGravity || std::abs(gravity.y) > MaxPhysicsGravity || std::abs(gravity.z) > MaxPhysicsGravity)
			{
				return MakeError(ErrorCode::InvalidArgument, "the gravity ({}, {}, {}) is out of range: each component must be at most {} m/s^2", gravity.x,
					gravity.y, gravity.z, MaxPhysicsGravity);
			}
			return {};
		}

		// Scales down the forces and torques accumulated on each active Dynamic body so that the step's velocity change from
		// them stays within MaxVelocityChangePerStep. Main thread, between steps (no body is locked by anyone else).
		static void BoundAccumulatedForces(JPH::PhysicsSystem& system, float deltaTime)
		{
			JPH::BodyIDVector active;
			system.GetActiveBodies(JPH::EBodyType::RigidBody, active);
			const JPH::BodyLockInterfaceNoLock& locks = system.GetBodyLockInterfaceNoLock();
			for (const JPH::BodyID& id : active)
			{
				const JPH::BodyLockWrite lock(locks, id);
				if (!lock.Succeeded() || !lock.GetBody().IsDynamic())
					continue;
				JPH::Body& body = lock.GetBody();
				const JPH::MotionProperties& motion = *body.GetMotionProperties();
				const JPH::Vec3 force = body.GetAccumulatedForce();
				const double linearChange = GetLength(force) * static_cast<double>(motion.GetInverseMass()) * static_cast<double>(deltaTime);
				if (linearChange > MaxVelocityChangePerStep)
				{
					body.ResetForce();
					body.AddForce(Scale(force, MaxVelocityChangePerStep / linearChange));
				}
				const JPH::Vec3 torque = body.GetAccumulatedTorque();
				const double angularChange = GetLength(motion.MultiplyWorldSpaceInverseInertiaByVector(body.GetRotation(), torque)) * static_cast<double>(deltaTime);
				if (angularChange > MaxVelocityChangePerStep)
				{
					body.ResetTorque();
					if (std::isfinite(angularChange))
						body.AddTorque(Scale(torque, MaxVelocityChangePerStep / angularChange));
				}
			}
		}

		// The motion quality a body gets: LinearCast needs a shape with an inner radius (Jolt asserts on a cast of a shape
		// that has none), so such a shape moves Discrete.
		[[nodiscard]] static JPH::EMotionQuality GetMotionQuality(bool wantsLinearCast, const JPH::Shape& shape)
		{
			return wantsLinearCast && shape.GetInnerRadius() > 0.0f ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
		}

		// Wakes the non-static bodies whose bounds overlap `bounds`: a body that slept resting on geometry that moved, changed
		// shape or disappeared would otherwise float where it was (Jolt wakes no neighbour itself).
		static void WakeBodiesIn(JPH::PhysicsSystem& system, const JPH::AABox& bounds)
		{
			if (!bounds.IsValid())
				return;
			JPH::AABox widened = bounds;
			widened.ExpandBy(JPH::Vec3::sReplicate(JPH::cDefaultConvexRadius));
			system.GetBodyInterface().ActivateBodiesInAABox(widened, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
		}

		// Logs PHYSICS_LIMIT_EXCEEDED as a warning the first time `count` reaches LimitWarningFraction of `limit` (§9.1
		// "approaching a limit"), once per world and limit.
		static void WarnWhenNearLimit(bool& hasWarned, uint32_t count, uint32_t limit, std::string_view what, std::string_view setting,
			std::string_view consequence)
		{
			if (hasWarned || static_cast<float>(count) < PhysicsWorldLimits::LimitWarningFraction * static_cast<float>(limit))
				return;
			hasWarned = true;
			ENGINE_CORE_WARN("{}: the physics world reached {} {} of its limit of {} ({}); at the limit {}", PhysicsLimitExceededCode, count, what, limit, setting,
				consequence);
		}

		[[nodiscard]] static std::string DescribeUpdateErrors(JPH::EPhysicsUpdateError errors)
		{
			std::string text;
			const auto append = [&text](std::string_view part)
			{
				text += text.empty() ? "" : ", ";
				text += part;
			};
			if ((errors & JPH::EPhysicsUpdateError::BodyPairCacheFull) != JPH::EPhysicsUpdateError::None)
				append("the body pair cache is full (PhysicsWorldLimits::MaxBodyPairs)");
			if ((errors & JPH::EPhysicsUpdateError::ManifoldCacheFull) != JPH::EPhysicsUpdateError::None)
				append("the contact manifold cache is full (PhysicsWorldLimits::MaxContactConstraints)");
			if ((errors & JPH::EPhysicsUpdateError::ContactConstraintsFull) != JPH::EPhysicsUpdateError::None)
				append("the contact constraint buffer is full (PhysicsWorldLimits::MaxContactConstraints)");
			return text;
		}

	}

	PhysicsWorld::PhysicsWorld(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PhysicsWorld::~PhysicsWorld()
	{
		if (m_State->System == nullptr)
			return;

		// Every body still in the world goes with it.
		JPH::BodyIDVector bodies;
		m_State->System->GetBodies(bodies);
		if (!bodies.empty())
		{
			JPH::BodyInterface& bodyInterface = m_State->System->GetBodyInterface();
			bodyInterface.RemoveBodies(bodies.data(), static_cast<int>(bodies.size()));
			bodyInterface.DestroyBodies(bodies.data(), static_cast<int>(bodies.size()));
		}
		m_State->Bodies.clear();
		Detail::UnregisterPhysicsWorld(*m_State->System);
	}

	Result<Scope<PhysicsWorld>> PhysicsWorld::Create(const PhysicsWorldSpecification& specification)
	{
		if (!PhysicsEngine::IsInitialized())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "a physics world needs the physics engine, which is not initialized")
					.WithHint("ProcessContext's Physics step initializes it"));
		}
		ENGINE_CORE_ASSERT(Detail::IsPhysicsMainThread(), "PhysicsWorld::Create called off the main thread");
		ENGINE_TRY(Utils::CheckGravity(specification.Gravity));

		const PhysicsWorldLimits& limits = specification.Limits;
		if (limits.MaxBodies == 0 || limits.MaxBodies > JPH::PhysicsSystem::cMaxBodiesLimit)
			return MakeError(ErrorCode::InvalidArgument, "MaxBodies must be 1 to {} (got {})", JPH::PhysicsSystem::cMaxBodiesLimit, limits.MaxBodies);
		if (limits.MaxBodyPairs == 0)
			return MakeError(ErrorCode::InvalidArgument, "MaxBodyPairs must be at least 1");
		if (limits.MaxContactConstraints == 0)
			return MakeError(ErrorCode::InvalidArgument, "MaxContactConstraints must be at least 1");
		if (limits.TempAllocatorBytes == 0 || limits.TempAllocatorBytes > std::numeric_limits<JPH::uint>::max())
			return MakeError(ErrorCode::InvalidArgument, "TempAllocatorBytes must be 1 to {} (got {})", std::numeric_limits<JPH::uint>::max(), limits.TempAllocatorBytes);

		Scope<PhysicsWorld> world = CreateScope<PhysicsWorld>(ConstructionKey{});
		State& state = *world->m_State;
		state.Specification = specification;
		state.TempAllocator = CreateScope<JPH::TempAllocatorImplWithMallocFallback>(static_cast<JPH::uint>(limits.TempAllocatorBytes));
		state.System = CreateScope<JPH::PhysicsSystem>();
		// 0 body mutexes: Jolt picks a count for the machine, which never changes results.
		state.System->Init(limits.MaxBodies, 0, limits.MaxBodyPairs, limits.MaxContactConstraints, state.BroadPhaseLayers, state.ObjectVsBroadPhaseLayerFilter,
			state.ObjectLayerPairFilter);
		state.System->SetGravity(Detail::ToJolt(specification.Gravity));
		state.System->SetContactListener(&state.ContactListener);
		// §9.2 "Seams": internal edge removal with a tighter face test than Jolt's (Physics/Private/PhysicsEdgeRemoval.h).
		state.System->SetSimCollideBodyVsBody(&Detail::CollideBodiesWithEdgeRemoval);
		Detail::RegisterPhysicsWorld(*state.System);
		return world;
	}

	Result<BodyHandle> PhysicsWorld::CreateBody(const BodyDescription& description)
	{
		State& state = *m_State;
		if (description.Shape == nullptr || description.Shape->m_State->Shape == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "a body needs a shape");
		if (!Utils::IsKnownMotionType(description.MotionType))
			return MakeError(ErrorCode::InvalidArgument, "unknown motion type {}", std::to_underlying(description.MotionType));
		if (!Utils::IsKnownMotionQuality(description.MotionQuality))
			return MakeError(ErrorCode::InvalidArgument, "unknown motion quality {}", std::to_underlying(description.MotionQuality));
		if (!Detail::IsFinite(description.Pose.Position))
			return MakeError(ErrorCode::InvalidArgument, "the position ({}, {}, {}) is not finite", description.Pose.Position.x, description.Pose.Position.y,
				description.Pose.Position.z);
		if (!IsWithinPhysicsRange(description.Pose.Position))
		{
			return MakeError(ErrorCode::InvalidArgument, "the position ({}, {}, {}) is out of range: each coordinate must be at most {} m", description.Pose.Position.x,
				description.Pose.Position.y, description.Pose.Position.z, MaxPhysicsCoordinate);
		}
		if (!IsPhysicsUnitRotation(description.Pose.Rotation))
			return MakeError(ErrorCode::InvalidArgument, "the rotation is not a finite unit quaternion");
		if (description.Layer >= state.Specification.Layers.GetLayerCount())
			return MakeError(ErrorCode::InvalidArgument, "layer {} is not one of the world's {} layers", description.Layer, state.Specification.Layers.GetLayerCount());
		if ((std::to_underlying(description.AllowedDofs) & ~std::to_underlying(PhysicsDofs::All)) != 0)
			return MakeError(ErrorCode::InvalidArgument, "AllowedDofs holds unknown flags 0x{:02x}", std::to_underlying(description.AllowedDofs));

		const std::array<std::pair<std::string_view, float>, 8> scalars = { {
			{ "Mass", description.Mass },
			{ "Friction", description.Friction },
			{ "Restitution", description.Restitution },
			{ "LinearDamping", description.LinearDamping },
			{ "AngularDamping", description.AngularDamping },
			{ "GravityFactor", description.GravityFactor },
			{ "MaxLinearVelocity", description.MaxLinearVelocity },
			{ "MaxAngularVelocity", description.MaxAngularVelocity },
		} };
		for (const auto& [name, value] : scalars)
		{
			if (!Detail::IsFinite(value))
				return MakeError(ErrorCode::InvalidArgument, "{} is not finite", name);
		}
		if (!Detail::IsFinite(description.LinearVelocity) || !Detail::IsFinite(description.AngularVelocity))
			return MakeError(ErrorCode::InvalidArgument, "the starting velocities are not finite");
		if (description.Friction < 0.0f)
			return MakeError(ErrorCode::InvalidArgument, "Friction must be >= 0 (got {})", description.Friction);
		if (description.Restitution < 0.0f || description.Restitution > 1.0f)
			return MakeError(ErrorCode::InvalidArgument, "Restitution must be in [0, 1] (got {})", description.Restitution);
		if (description.LinearDamping < 0.0f || description.AngularDamping < 0.0f)
			return MakeError(ErrorCode::InvalidArgument, "damping must be >= 0 (got {} and {})", description.LinearDamping, description.AngularDamping);
		if (description.MaxLinearVelocity < 0.0f || description.MaxAngularVelocity < 0.0f)
			return MakeError(ErrorCode::InvalidArgument, "the maximum velocities must be >= 0 (got {} and {})", description.MaxLinearVelocity,
				description.MaxAngularVelocity);

		const PhysicsShape::State& shape = *description.Shape->m_State;
		const bool isDynamic = description.MotionType == PhysicsMotionType::Dynamic;
		JPH::MassProperties dynamicMass;
		if (isDynamic)
		{
			if (description.IsSensor)
				return MakeError(ErrorCode::Validation, "{}: a Dynamic body cannot be a trigger; triggers are Kinematic sensors", PhysicsDynamicTriggerCode);
			ENGINE_TRY_ASSIGN(dynamicMass, Detail::GetDynamicMassProperties(*shape.Shape, shape.HasMesh, description.Mass, description.AllowedDofs));
		}

		const uint32_t limit = state.Specification.Limits.MaxBodies;
		if (state.System->GetNumBodies() >= limit)
			return MakeError(ErrorCode::InvalidState, "{}: the physics world already holds its limit of {} bodies", PhysicsLimitExceededCode, limit);

		// §9.2 "Body settings", with the rules that keep Jolt's asserts unreachable (BodyDescription).
		const PhysicsObjectKind kind = description.IsSensor
			? PhysicsObjectKind::Sensor
			: (description.MotionType == PhysicsMotionType::Static ? PhysicsObjectKind::Static : PhysicsObjectKind::Moving);
		JPH::BodyCreationSettings settings;
		settings.SetShape(shape.Shape.GetPtr());
		settings.mPosition = Detail::ToJolt(description.Pose.Position);
		settings.mRotation = Detail::ToJoltRotation(description.Pose.Rotation);
		settings.mMotionType = Utils::ToJoltMotionType(description.MotionType);
		settings.mObjectLayer = MakePhysicsObjectLayer(description.Layer, kind);
		settings.mCollisionGroup = state.CollisionGroups.MakeCollisionGroup(description.CollisionGroup, shape.HasMesh);
		settings.mIsSensor = description.IsSensor;
		settings.mCollideKinematicVsNonDynamic = description.CollideKinematicVsNonDynamic;
		const bool wantsLinearCast = description.MotionQuality == PhysicsMotionQuality::LinearCast;
		settings.mMotionQuality = Utils::GetMotionQuality(wantsLinearCast, *shape.Shape);
		settings.mAllowedDOFs = isDynamic ? static_cast<JPH::EAllowedDOFs>(std::to_underlying(description.AllowedDofs)) : JPH::EAllowedDOFs::All;
		settings.mEnhancedInternalEdgeRemoval = description.EnhancedInternalEdgeRemoval;
		// Sensors are kept active so they see sleeping bodies (§9.2 "Triggers").
		settings.mAllowSleeping = description.IsSensor ? false : description.AllowSleeping;
		settings.mFriction = description.Friction;
		settings.mRestitution = description.Restitution;
		settings.mLinearDamping = description.LinearDamping;
		settings.mAngularDamping = description.AngularDamping;
		const float maxLinearVelocity = std::min(description.MaxLinearVelocity, Utils::MaxBodySpeed);
		const float maxAngularVelocity = std::min(description.MaxAngularVelocity, Utils::MaxBodySpeed);
		settings.mMaxLinearVelocity = maxLinearVelocity;
		settings.mMaxAngularVelocity = maxAngularVelocity;
		settings.mGravityFactor = std::clamp(description.GravityFactor, -Utils::MaxGravityFactor, Utils::MaxGravityFactor);
		settings.mUserData = description.UserData;
		if (isDynamic)
		{
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
			settings.mMassPropertiesOverride = dynamicMass;
		}
		else if (description.MotionType == PhysicsMotionType::Kinematic)
		{
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
			settings.mMassPropertiesOverride.mMass = Utils::KinematicMass;
			settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sScale(Utils::KinematicInertia);
		}

		JPH::BodyInterface& bodyInterface = state.System->GetBodyInterface();
		JPH::Body* body = bodyInterface.CreateBody(settings);
		if (body == nullptr)
			return MakeError(ErrorCode::InvalidState, "{}: the physics world cannot hold another body", PhysicsLimitExceededCode);

		// Starting velocities go in clamped to just below the maxima, as BodyInterface::SetLinearVelocity clamps (Jolt asserts
		// on a longer starting velocity in BodyCreationSettings), before the body joins the broad phase.
		if (description.MotionType != PhysicsMotionType::Static)
		{
			body->SetLinearVelocity(Utils::ClampVelocity(Detail::ToJolt(description.LinearVelocity), maxLinearVelocity));
			body->SetAngularVelocity(Utils::ClampVelocity(Detail::ToJolt(description.AngularVelocity), maxAngularVelocity));
		}
		const JPH::BodyID id = body->GetID();
		const bool isStatic = description.MotionType == PhysicsMotionType::Static;
		bodyInterface.AddBody(id, isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
		// A sleeping body that a new static body now overlaps would rest inside it: the world wakes what the new body
		// touches (Jolt wakes no neighbour itself). Bodies that are not static join awake and find their contacts.
		if (isStatic)
			Utils::WakeBodiesIn(*state.System, bodyInterface.GetTransformedShape(id).GetWorldSpaceBounds());

		const uint32_t slot = id.GetIndex();
		if (slot >= state.Bodies.size())
			state.Bodies.resize(slot + 1);
		state.Bodies[slot] = Detail::PhysicsBodyRecord{ .ID = id,
			.Shape = description.Shape,
			.Mass = isDynamic ? description.Mass : 0.0f,
			.CollisionGroup = description.CollisionGroup,
			.Collider = shape.ColliderUserData.empty() ? 0 : shape.ColliderUserData.front(),
			.IsSensor = description.IsSensor,
			.WantsLinearCast = wantsLinearCast };

		Utils::WarnWhenNearLimit(state.HasWarnedBodies, state.System->GetNumBodies(), limit, "bodies", "PhysicsWorldLimits::MaxBodies",
			"no further body is created");
		return Detail::ToBodyHandle(id);
	}

	void PhysicsWorld::DestroyBody(BodyHandle body)
	{
		State& state = *m_State;
		const std::optional<LiveBody> live = Utils::RequireBody(*state.System, body, Utils::AnyBodies, "DestroyBody");
		if (!live.has_value())
			return;

		JPH::BodyInterface& bodyInterface = state.System->GetBodyInterface();
		const JPH::AABox bounds = bodyInterface.GetTransformedShape(live->ID).GetWorldSpaceBounds();
		bodyInterface.RemoveBody(live->ID);
		bodyInterface.DestroyBody(live->ID);
		const uint32_t slot = live->ID.GetIndex();
		if (slot < state.Bodies.size() && state.Bodies[slot].ID == live->ID)
			state.Bodies[slot] = Detail::PhysicsBodyRecord{};
		Utils::WakeBodiesIn(*state.System, bounds);
	}

	bool PhysicsWorld::IsBodyValid(BodyHandle body) const
	{
		return Utils::FindLiveBody(*m_State->System, body).has_value();
	}

	uint64_t PhysicsWorld::GetUserData(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "GetUserData");
		return live.has_value() ? m_State->System->GetBodyInterface().GetUserData(live->ID) : 0;
	}

	PhysicsMotionType PhysicsWorld::GetMotionType(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "GetMotionType");
		return live.has_value() ? Utils::ToPhysicsMotionType(live->MotionType) : PhysicsMotionType::Static;
	}

	bool PhysicsWorld::IsSensor(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "IsSensor");
		if (!live.has_value())
			return false;
		const JPH::BodyLockRead lock(m_State->System->GetBodyLockInterface(), live->ID);
		return lock.Succeeded() && lock.GetBody().IsSensor();
	}

	uint32_t PhysicsWorld::GetLayer(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "GetLayer");
		if (!live.has_value())
			return 0;
		return GetPhysicsLayerIndex(static_cast<PhysicsObjectLayer>(m_State->System->GetBodyInterface().GetObjectLayer(live->ID)));
	}

	Ref<const PhysicsShape> PhysicsWorld::GetShape(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "GetShape");
		if (!live.has_value())
			return nullptr;
		// Every live body was created by CreateBody (character inner bodies included), so it has a record.
		const uint32_t slot = live->ID.GetIndex();
		const bool hasRecord = slot < m_State->Bodies.size() && m_State->Bodies[slot].ID == live->ID;
		ENGINE_CORE_ASSERT(hasRecord, "PhysicsWorld::GetShape: 0x{:08x} names no body record of this world", body.GetValue());
		return hasRecord ? m_State->Bodies[slot].Shape : nullptr;
	}

	Status PhysicsWorld::SetShape(BodyHandle body, const Ref<const PhysicsShape>& shape)
	{
		State& state = *m_State;
		const std::optional<LiveBody> live = Utils::FindLiveBody(*state.System, body);
		if (!live.has_value())
			return MakeError(ErrorCode::InvalidArgument, "0x{:08x} names no live body of this world", body.GetValue());
		if (shape == nullptr || shape->m_State->Shape == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "SetShape needs a shape");
		// Every live body was created by CreateBody (character inner bodies included), so it has a record.
		const uint32_t slot = live->ID.GetIndex();
		const bool hasRecord = slot < state.Bodies.size() && state.Bodies[slot].ID == live->ID;
		ENGINE_CORE_ASSERT(hasRecord, "PhysicsWorld::SetShape: 0x{:08x} names no body record of this world", body.GetValue());
		if (!hasRecord)
			return MakeError(ErrorCode::InvalidArgument, "0x{:08x} names no body record of this world", body.GetValue());

		const PhysicsShape::State& newShape = *shape->m_State;
		Detail::PhysicsBodyRecord& record = state.Bodies[slot];
		const bool isDynamic = live->MotionType == JPH::EMotionType::Dynamic;
		JPH::BodyInterface& bodyInterface = state.System->GetBodyInterface();
		JPH::MassProperties dynamicMass;
		if (isDynamic)
		{
			PhysicsDofs dofs = PhysicsDofs::None;
			{
				const JPH::BodyLockRead lock(state.System->GetBodyLockInterface(), live->ID);
				dofs = static_cast<PhysicsDofs>(std::to_underlying(lock.GetBody().GetMotionProperties()->GetAllowedDOFs()));
			}
			ENGINE_TRY_ASSIGN(dynamicMass, Detail::GetDynamicMassProperties(*newShape.Shape, newShape.HasMesh, record.Mass, dofs));
		}

		const JPH::AABox oldBounds = bodyInterface.GetTransformedShape(live->ID).GetWorldSpaceBounds();
		// Never the shape's mass properties (a Kinematic body keeps the fixed ones CreateBody gave it, so it may take a
		// mesh); a Dynamic body keeps its Mass with the new shape's inertia scaled to it.
		const JPH::EActivation activation = live->MotionType == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
		bodyInterface.SetShape(live->ID, newShape.Shape.GetPtr(), false, activation);
		if (isDynamic)
		{
			const JPH::BodyLockWrite lock(state.System->GetBodyLockInterface(), live->ID);
			JPH::MotionProperties& motion = *lock.GetBody().GetMotionProperties();
			motion.SetMassProperties(motion.GetAllowedDOFs(), dynamicMass);
		}
		// The group stays; whether the body holds a mesh may have changed (PhysicsCollisionGroups). So may whether the shape
		// can cast.
		bodyInterface.SetCollisionGroup(live->ID, state.CollisionGroups.MakeCollisionGroup(record.CollisionGroup, newShape.HasMesh));
		if (live->MotionType != JPH::EMotionType::Static)
			bodyInterface.SetMotionQuality(live->ID, Utils::GetMotionQuality(record.WantsLinearCast, *newShape.Shape));
		record.Shape = shape;
		record.Collider = newShape.ColliderUserData.empty() ? 0 : newShape.ColliderUserData.front();

		JPH::AABox bounds = oldBounds;
		bounds.Encapsulate(bodyInterface.GetTransformedShape(live->ID).GetWorldSpaceBounds());
		Utils::WakeBodiesIn(*state.System, bounds);
		return {};
	}

	PhysicsPose PhysicsWorld::GetPose(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "GetPose");
		if (!live.has_value())
			return {};
		JPH::RVec3 position;
		JPH::Quat rotation;
		m_State->System->GetBodyInterface().GetPositionAndRotation(live->ID, position, rotation);
		return PhysicsPose{ .Position = Detail::ToGlm(position), .Rotation = Detail::ToGlm(rotation) };
	}

	void PhysicsWorld::SetPose(BodyHandle body, const PhysicsPose& pose, bool activate)
	{
		ENGINE_CORE_ASSERT(IsPlaceablePhysicsPose(pose), "PhysicsWorld::SetPose: the pose must be finite and within MaxPhysicsCoordinate, with a unit rotation");
		if (!IsPlaceablePhysicsPose(pose))
			return;
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "SetPose");
		if (!live.has_value())
			return;

		JPH::BodyInterface& bodyInterface = m_State->System->GetBodyInterface();
		const bool isStatic = live->MotionType == JPH::EMotionType::Static;
		const JPH::AABox oldBounds = isStatic ? bodyInterface.GetTransformedShape(live->ID).GetWorldSpaceBounds() : JPH::AABox();
		bodyInterface.SetPositionAndRotation(live->ID, Detail::ToJolt(pose.Position), Detail::ToJoltRotation(pose.Rotation),
			activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
		// A static body cannot wake; what rested on it, or now touches it, does.
		if (isStatic && activate)
		{
			JPH::AABox bounds = oldBounds;
			bounds.Encapsulate(bodyInterface.GetTransformedShape(live->ID).GetWorldSpaceBounds());
			Utils::WakeBodiesIn(*m_State->System, bounds);
		}
	}

	void PhysicsWorld::MoveKinematic(BodyHandle body, const PhysicsPose& target, float deltaTime)
	{
		const bool isValid = IsPlaceablePhysicsPose(target) && Detail::IsFinite(deltaTime) && deltaTime > 0.0f;
		ENGINE_CORE_ASSERT(isValid,
			"PhysicsWorld::MoveKinematic: the target must be finite and within MaxPhysicsCoordinate, with a unit rotation, and the step positive (got {} s)",
			deltaTime);
		if (!isValid)
			return;
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::KinematicBodies, "MoveKinematic");
		if (!live.has_value())
			return;
		m_State->System->GetBodyInterface().MoveKinematic(live->ID, Detail::ToJolt(target.Position), Detail::ToJoltRotation(target.Rotation), deltaTime);
	}

	void PhysicsWorld::AddForce(BodyHandle body, const glm::vec3& force)
	{
		ENGINE_CORE_ASSERT(Detail::IsFinite(force), "PhysicsWorld::AddForce: the force is not finite");
		if (!Detail::IsFinite(force))
			return;
		if (const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::DynamicBodies, "AddForce"))
			m_State->System->GetBodyInterface().AddForce(live->ID, Detail::ToJolt(force), JPH::EActivation::Activate);
	}

	void PhysicsWorld::AddForceAtPosition(BodyHandle body, const glm::vec3& force, const glm::vec3& position)
	{
		ENGINE_CORE_ASSERT(Detail::IsFinite(force) && Detail::IsFinite(position), "PhysicsWorld::AddForceAtPosition: the force or position is not finite");
		if (!Detail::IsFinite(force) || !Detail::IsFinite(position))
			return;
		if (const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::DynamicBodies, "AddForceAtPosition"))
			m_State->System->GetBodyInterface().AddForce(live->ID, Detail::ToJolt(force), Detail::ToJolt(position), JPH::EActivation::Activate);
	}

	void PhysicsWorld::AddTorque(BodyHandle body, const glm::vec3& torque)
	{
		ENGINE_CORE_ASSERT(Detail::IsFinite(torque), "PhysicsWorld::AddTorque: the torque is not finite");
		if (!Detail::IsFinite(torque))
			return;
		if (const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::DynamicBodies, "AddTorque"))
			m_State->System->GetBodyInterface().AddTorque(live->ID, Detail::ToJolt(torque), JPH::EActivation::Activate);
	}

	void PhysicsWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse)
	{
		ENGINE_CORE_ASSERT(Detail::IsFinite(impulse), "PhysicsWorld::AddImpulse: the impulse is not finite");
		if (!Detail::IsFinite(impulse))
			return;
		if (const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::DynamicBodies, "AddImpulse"))
		{
			// The velocity change applied at once and clamped to the body's maximum, as the step would clamp it: a large
			// impulse on a light body never leaves a velocity whose squared length overflows.
			{
				const JPH::BodyLockWrite lock(m_State->System->GetBodyLockInterface(), live->ID);
				JPH::Body& locked = lock.GetBody();
				const JPH::MotionProperties& motion = *locked.GetMotionProperties();
				const JPH::Vec3 change = Detail::ToJolt(impulse) * motion.GetInverseMass();
				locked.SetLinearVelocity(Utils::ClampVelocity(locked.GetLinearVelocity() + change, motion.GetMaxLinearVelocity()));
			}
			m_State->System->GetBodyInterface().ActivateBody(live->ID);
		}
	}

	void PhysicsWorld::AddAngularImpulse(BodyHandle body, const glm::vec3& angularImpulse)
	{
		ENGINE_CORE_ASSERT(Detail::IsFinite(angularImpulse), "PhysicsWorld::AddAngularImpulse: the impulse is not finite");
		if (!Detail::IsFinite(angularImpulse))
			return;
		if (const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::DynamicBodies, "AddAngularImpulse"))
		{
			// As AddImpulse: the change through the inverse inertia, clamped to the body's maximum.
			{
				const JPH::BodyLockWrite lock(m_State->System->GetBodyLockInterface(), live->ID);
				JPH::Body& locked = lock.GetBody();
				const JPH::MotionProperties& motion = *locked.GetMotionProperties();
				const JPH::Vec3 change = motion.MultiplyWorldSpaceInverseInertiaByVector(locked.GetRotation(), Detail::ToJolt(angularImpulse));
				locked.SetAngularVelocity(Utils::ClampVelocity(locked.GetAngularVelocity() + change, motion.GetMaxAngularVelocity()));
			}
			m_State->System->GetBodyInterface().ActivateBody(live->ID);
		}
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "GetLinearVelocity");
		if (!live.has_value() || live->MotionType == JPH::EMotionType::Static)
			return glm::vec3(0.0f);
		return Detail::ToGlm(m_State->System->GetBodyInterface().GetLinearVelocity(live->ID));
	}

	void PhysicsWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity)
	{
		ENGINE_CORE_ASSERT(Detail::IsFinite(velocity), "PhysicsWorld::SetLinearVelocity: the velocity is not finite");
		if (!Detail::IsFinite(velocity))
			return;
		if (const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::MovingBodies, "SetLinearVelocity"))
		{
			// Jolt's BodyInterface clamps to the body's maximum; setting a velocity always wakes the body.
			JPH::BodyInterface& bodyInterface = m_State->System->GetBodyInterface();
			bodyInterface.SetLinearVelocity(live->ID, Detail::ToJolt(velocity));
			bodyInterface.ActivateBody(live->ID);
		}
	}

	glm::vec3 PhysicsWorld::GetAngularVelocity(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "GetAngularVelocity");
		if (!live.has_value() || live->MotionType == JPH::EMotionType::Static)
			return glm::vec3(0.0f);
		return Detail::ToGlm(m_State->System->GetBodyInterface().GetAngularVelocity(live->ID));
	}

	void PhysicsWorld::SetAngularVelocity(BodyHandle body, const glm::vec3& velocity)
	{
		ENGINE_CORE_ASSERT(Detail::IsFinite(velocity), "PhysicsWorld::SetAngularVelocity: the velocity is not finite");
		if (!Detail::IsFinite(velocity))
			return;
		if (const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::MovingBodies, "SetAngularVelocity"))
		{
			JPH::BodyInterface& bodyInterface = m_State->System->GetBodyInterface();
			bodyInterface.SetAngularVelocity(live->ID, Detail::ToJolt(velocity));
			bodyInterface.ActivateBody(live->ID);
		}
	}

	bool PhysicsWorld::IsSleeping(BodyHandle body) const
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "IsSleeping");
		if (!live.has_value() || live->MotionType == JPH::EMotionType::Static)
			return true;
		return !m_State->System->GetBodyInterface().IsActive(live->ID);
	}

	void PhysicsWorld::WakeUp(BodyHandle body)
	{
		const std::optional<LiveBody> live = Utils::RequireBody(*m_State->System, body, Utils::AnyBodies, "WakeUp");
		if (live.has_value() && live->MotionType != JPH::EMotionType::Static)
			m_State->System->GetBodyInterface().ActivateBody(live->ID);
	}

	Status PhysicsWorld::Step(float deltaTime, uint32_t collisionSteps)
	{
		State& state = *m_State;
		if (!Detail::IsFinite(deltaTime) || deltaTime <= 0.0f)
			return MakeError(ErrorCode::InvalidArgument, "a step needs a finite, positive duration (got {} s)", deltaTime);
		if (collisionSteps == 0 || collisionSteps > static_cast<uint32_t>(std::numeric_limits<int>::max()))
			return MakeError(ErrorCode::InvalidArgument, "a step needs 1 to {} collision steps (got {})", std::numeric_limits<int>::max(), collisionSteps);
		JPH::JobSystem* jobSystem = Detail::GetPhysicsJobSystem();
		if (jobSystem == nullptr)
			return MakeError(ErrorCode::InvalidState, "stepping a physics world needs the physics engine, which is not initialized");
		ENGINE_CORE_ASSERT(Detail::IsPhysicsMainThread(), "PhysicsWorld::Step called off the main thread");

		state.StepContacts.Reset();
		Utils::BoundAccumulatedForces(*state.System, deltaTime);
		const JPH::EPhysicsUpdateError errors = state.System->Update(deltaTime, static_cast<int>(collisionSteps), state.TempAllocator.get(), jobSystem);
		const auto [bodyPairs, contacts] = state.StepContacts.CountBodyPairsAndContacts();
		state.LastStepBodyPairs = bodyPairs;
		state.LastStepContacts = contacts;

		const PhysicsWorldLimits& limits = state.Specification.Limits;
		Utils::WarnWhenNearLimit(state.HasWarnedBodyPairs, bodyPairs, limits.MaxBodyPairs, "body pairs in contact", "PhysicsWorldLimits::MaxBodyPairs",
			"contacts are dropped");
		Utils::WarnWhenNearLimit(state.HasWarnedContactConstraints, contacts, limits.MaxContactConstraints, "contact constraints",
			"PhysicsWorldLimits::MaxContactConstraints", "contacts are dropped");
		if (errors != JPH::EPhysicsUpdateError::None)
		{
			return MakeError(ErrorCode::InvalidState, "{}: {}; contacts beyond the limit were dropped this step", PhysicsLimitExceededCode,
				Utils::DescribeUpdateErrors(errors));
		}
		return {};
	}

	glm::vec3 PhysicsWorld::GetGravity() const
	{
		return Detail::ToGlm(m_State->System->GetGravity());
	}

	Status PhysicsWorld::SetGravity(const glm::vec3& gravity)
	{
		ENGINE_TRY(Utils::CheckGravity(gravity));
		m_State->System->SetGravity(Detail::ToJolt(gravity));
		m_State->Specification.Gravity = gravity;
		return {};
	}

	std::vector<ContactEvent> PhysicsWorld::DrainContactEvents()
	{
		return m_State->Contacts.Drain();
	}

	ContactBuffer& PhysicsWorld::GetContactBuffer()
	{
		return m_State->Contacts;
	}

	const PhysicsLayerTable& PhysicsWorld::GetLayers() const
	{
		return m_State->Specification.Layers;
	}

	const PhysicsWorldLimits& PhysicsWorld::GetLimits() const
	{
		return m_State->Specification.Limits;
	}

	PhysicsWorldStats PhysicsWorld::GetStats() const
	{
		const JPH::PhysicsSystem& system = *m_State->System;
		return PhysicsWorldStats{ .BodyCount = system.GetNumBodies(),
			.ActiveBodyCount = system.GetNumActiveBodies(JPH::EBodyType::RigidBody),
			.BodyPairCount = m_State->LastStepBodyPairs,
			.ContactConstraintCount = m_State->LastStepContacts };
	}

}
