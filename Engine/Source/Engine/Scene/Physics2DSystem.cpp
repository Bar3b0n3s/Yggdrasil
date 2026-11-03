#include "EnginePCH.h"
#include "Engine/Scene/Physics2DSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/PolygonCollider2DComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/Physics2DSystemState.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <numbers>
#include <tuple>
#include <utility>
#include <variant>

// The play session's 2D physics (Physics2DSystem.h): PhysicsSystem.cpp's design in the plane, rule for rule. Rebuilds compose
// the 2D bodies again (Scene/Private/Physics2DComposition.h) when an EnTT signal or the hierarchy says they may have changed,
// and compare each body's shape inputs bit for bit every PreStep; pairs are keyed by owners and counted by sub-shape
// contacts, with M20's pending enter, dormant contacts and collider pairs; callbacks run with Dispatching set; every pose
// handed to the world passes the physics range first. What differs:
//   - The world is created at the first 2D body (EnsureWorld); before it the system answers as an empty world, and a scene
//     that holds no 2D component and has no world makes PreStep return at once whatever changed (an entity enabled or
//     disabled), so a 3D game pays nothing per tick.
//   - Poses are planar: a body takes its owner's world X and Y and the twist about Z of its world rotation; a write-back keeps
//     the owner's world Z and writes a rotation about Z alone. A body is placed again only when its owner's planar pose
//     differs bit for bit from the one its Transform had at the body's last creation, placement, kinematic move or
//     write-back (Physics2DBodyEntry::PlacedPose, both computed from the entity by GetPlanarPose), so a Transform written to
//     the same planar pose (its world Z, say, for draw depth) moves and wakes nothing, whatever the body's motion type.
//   - Contacts and query hits are planar as Physics2DWorld reports them (points at z = 0, normals in the plane): the system
//     passes them on unchanged.
//   - The signals of the 3D physics components count only on entities that hold a 2D one (PHYSICS2D_MIXED_WORLDS).

namespace Engine {

	namespace {

		// The largest force, impulse, torque or velocity component the body functions accept, as the 3D system's.
		constexpr float MaxVector2DMagnitude = 1.0e12f;

		// One pair event before dispatch.
		struct Pending2DPairEvent
		{
			PhysicsPairKey Key{};
			PhysicsEventType Type = PhysicsEventType::CollisionEnter;
			bool Synthesized = false;
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
			float RelativeSpeed = 0.0f;
			UUID LowCollider{};
			UUID HighCollider{};
			UUID ColliderBody{};
		};

		// A pair as PostStep first found it, before the step's records changed it.
		struct Pair2DBefore
		{
			size_t Contacts = 0;
			bool Touched = false;
		};

		// The first contact of a pair entering this step, oriented from the pair's Low side to its High one; the smallest by a
		// total order on the record's content, so it never depends on the order of the step's records.
		struct First2DContact
		{
			UUID LowCollider{};
			UUID HighCollider{};
			uint32_t LowSubShape = 0;
			uint32_t HighSubShape = 0;
			std::array<uint32_t, 7> Bits{};
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
			float RelativeSpeed = 0.0f;

			[[nodiscard]] bool operator<(const First2DContact& other) const
			{
				return std::tie(LowCollider, HighCollider, LowSubShape, HighSubShape, Bits)
					< std::tie(other.LowCollider, other.HighCollider, other.LowSubShape, other.HighSubShape, other.Bits);
			}
		};

		// A pair FlushDestroyed closes.
		struct Closing2DPair
		{
			PhysicsPairKey Key{};
			bool ToLow = false;
			bool ToHigh = false;
			UUID LowCollider{};
			UUID HighCollider{};
			UUID ColliderBody{};
		};

		struct Resolved2DBody
		{
			UUID Owner{};
			UUID Collider{};
		};

	}

	namespace Utils {

		template<typename Component, auto Handler, typename Instance>
		static void Connect2DSignals(entt::registry& registry, Instance& instance)
		{
			registry.on_construct<Component>().template connect<Handler>(instance);
			registry.on_update<Component>().template connect<Handler>(instance);
			registry.on_destroy<Component>().template connect<Handler>(instance);
		}

		template<typename Component, typename Instance>
		static void Disconnect2DSignals(entt::registry& registry, Instance& instance)
		{
			registry.on_construct<Component>().disconnect(&instance);
			registry.on_update<Component>().disconnect(&instance);
			registry.on_destroy<Component>().disconnect(&instance);
		}

		static bool Same2DBits(float a, float b)
		{
			return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
		}

		static bool Same2DBits(const glm::vec2& a, const glm::vec2& b)
		{
			return Same2DBits(a.x, b.x) && Same2DBits(a.y, b.y);
		}

		static bool Same2DBits(const glm::vec3& a, const glm::vec3& b)
		{
			return Same2DBits(a.x, b.x) && Same2DBits(a.y, b.y) && Same2DBits(a.z, b.z);
		}

		static bool Same2DBits(const glm::quat& a, const glm::quat& b)
		{
			return Same2DBits(a.x, b.x) && Same2DBits(a.y, b.y) && Same2DBits(a.z, b.z) && Same2DBits(a.w, b.w);
		}

		static bool Same2DBits(const glm::mat4& a, const glm::mat4& b)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!Same2DBits(a[column][row], b[column][row]))
						return false;
				}
			}
			return true;
		}

		static bool Same2DBits(const Physics2DPose& a, const Physics2DPose& b)
		{
			return Same2DBits(a.Position, b.Position) && Same2DBits(a.Angle, b.Angle);
		}

		static std::unexpected<Error> MakeNo2DBodyError(UUID entity)
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("entity {} has no 2D physics body", entity))
					.WithHint("2D bodies exist for enabled entities with a RigidBody2D whose 2D colliders the 2D composition accepts"));
		}

		static std::unexpected<Error> Make2DMotionTypeError(UUID entity, PhysicsMotionType type, std::string_view operation, std::string_view needed)
		{
			return std::unexpected(Error(ErrorCode::InvalidState,
				std::format("the 2D body of entity {} is {}, and {} needs a {} body", entity, PhysicsMotionTypeToString(type), operation, needed)));
		}

		static std::unexpected<Error> Make2DVelocityError(UUID entity, PhysicsMotionType type, std::string_view operation)
		{
			std::unexpected<Error> error = Make2DMotionTypeError(entity, type, operation, "Dynamic");
			if (type == PhysicsMotionType::Kinematic)
				return std::unexpected(std::move(error).error().WithHint("Kinematic bodies follow their Transform: move them with MoveKinematic"));
			return error;
		}

		// InvalidArgument unless both components are finite and at most `limit` in magnitude.
		static Status Check2DVector(const glm::vec2& value, float limit, std::string_view what)
		{
			if (!std::isfinite(value.x) || !std::isfinite(value.y))
				return MakeError(ErrorCode::InvalidArgument, "the {} ({}, {}) is not finite", what, value.x, value.y);
			if (std::abs(value.x) > limit || std::abs(value.y) > limit)
				return MakeError(ErrorCode::InvalidArgument, "the {} ({}, {}) is out of range: each component must be at most {}", what, value.x, value.y, limit);
			return {};
		}

		// InvalidArgument unless `value` is finite and at most `limit` in magnitude.
		static Status Check2DScalar(float value, float limit, std::string_view what)
		{
			if (!std::isfinite(value))
				return MakeError(ErrorCode::InvalidArgument, "the {} {} is not finite", what, value);
			if (std::abs(value) > limit)
				return MakeError(ErrorCode::InvalidArgument, "the {} {} is out of range: it must be at most {}", what, value, limit);
			return {};
		}

		// Degrees about +Z to radians, and back (an IEEE multiplication, identical in every configuration).
		static float RadiansFromDegrees(float degrees)
		{
			return degrees * (std::numbers::pi_v<float> / 180.0f);
		}

		// The entity's world matrix: WorldTransformComponent, which the step hooks find current (the play session updates the
		// transforms before Physics2DPreStep and before the system is created), else computed from the parent chain.
		static glm::mat4 Get2DWorldMatrix(ConstEntity entity)
		{
			if (const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>())
				return world->Matrix;
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		// A body origin's planar pose from its entity: the world X and Y and the twist about Z of the world rotation.
		static Physics2DPose GetPlanarPose(ConstEntity entity, const glm::mat4& world)
		{
			return ToPhysics2DPose(PhysicsPose{ .Position = glm::vec3(world[3]), .Rotation = TransformSystem::GetWorldRotation(entity) });
		}

		// The planar pose of the entity's current Transform, bit for bit what GetPlanarPose gives from the world matrix the
		// next transform update caches (ComputeWorldMatrix makes the same products in the same order, TransformSystem.cpp):
		// the PlacedPose a write-back or a teleport records before that update runs.
		static Physics2DPose GetCurrentPlanarPose(ConstEntity entity)
		{
			return GetPlanarPose(entity, TransformSystem::ComputeWorldMatrix(entity));
		}

		// Whether the 2D world places a body at `pose` (its 3D pose IsPlaceablePhysicsPose).
		static bool IsPlaceable2DPose(const Physics2DPose& pose)
		{
			return std::isfinite(pose.Angle) && IsPlaceablePhysicsPose(ToPhysicsPose(pose));
		}

		static void Restore2DLocalPose(Entity entity, const glm::vec3& translation, const glm::quat& rotation)
		{
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			transform.Translation = translation;
			transform.Rotation = rotation;
		}

		// Writes `pose` as the entity's world X and Y and a world rotation about Z alone, keeping its world Z, through the
		// parent's current world inverse, directly (a runtime write); returns the local Translation and Rotation it wrote.
		static std::pair<glm::vec3, glm::quat> Write2DWorldPose(Entity entity, const Physics2DPose& pose)
		{
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			const glm::quat rotation = ToPhysicsPose(pose).Rotation;
			const Entity parent = entity.GetParent();
			if (parent.IsValid())
			{
				const glm::mat4 parentWorld = TransformSystem::ComputeWorldMatrix(parent);
				const float worldZ = (parentWorld * glm::vec4(transform.Translation, 1.0f)).z;
				transform.Translation = glm::vec3(glm::affineInverse(parentWorld) * glm::vec4(pose.Position, worldZ, 1.0f));
				transform.Rotation = glm::normalize(glm::conjugate(TransformSystem::GetWorldRotation(parent)) * rotation);
			}
			else
			{
				transform.Translation = glm::vec3(pose.Position, transform.Translation.z);
				transform.Rotation = rotation;
			}
			return { transform.Translation, transform.Rotation };
		}

		// The body settings of `plan`: Body2DDescription without its shape, pose and velocities.
		static Body2DDescription MakeBody2DSettings(const Physics2DBodyPlan& plan, ConstEntity owner)
		{
			Body2DDescription settings;
			settings.MotionType = plan.MotionType;
			settings.Layer = plan.Layer;
			settings.IsSensor = plan.IsSensor;
			// Sensors detect kinematic platforms, as in the 3D world.
			settings.CollideKinematicVsNonDynamic = plan.IsSensor;
			settings.UserData = plan.Owner.GetValue();
			settings.CollisionGroup = plan.CollisionGroup.GetValue();
			if (plan.Origin == PhysicsBodyOrigin::RigidBody)
			{
				const RigidBody2DComponent& body = owner.GetComponent<RigidBody2DComponent>();
				settings.MotionQuality = ToPhysicsMotionQuality(body.MotionQuality);
				settings.FixedRotation = body.FixedRotation;
				settings.Mass = body.Mass;
				settings.Friction = body.Friction;
				settings.Restitution = body.Restitution;
				settings.LinearDamping = body.LinearDamping;
				settings.AngularDamping = body.AngularDamping;
				settings.GravityFactor = body.GravityFactor;
				settings.MaxLinearVelocity = body.MaxLinearVelocity;
				settings.MaxAngularVelocity = body.MaxAngularVelocity;
				settings.AllowSleeping = body.AllowSleeping;
				settings.EnhancedInternalEdgeRemoval = body.EnhancedInternalEdgeRemoval;
			}
			return settings;
		}

		static bool SameBody2DSettings(const Body2DDescription& a, const Body2DDescription& b)
		{
			return std::tie(a.MotionType, a.Layer, a.IsSensor, a.CollideKinematicVsNonDynamic, a.MotionQuality, a.FixedRotation, a.Mass, a.Friction,
					   a.Restitution, a.LinearDamping, a.AngularDamping, a.GravityFactor, a.MaxLinearVelocity, a.MaxAngularVelocity,
					   a.EnhancedInternalEdgeRemoval, a.AllowSleeping, a.UserData, a.CollisionGroup)
				== std::tie(b.MotionType, b.Layer, b.IsSensor, b.CollideKinematicVsNonDynamic, b.MotionQuality, b.FixedRotation, b.Mass, b.Friction,
					b.Restitution, b.LinearDamping, b.AngularDamping, b.GravityFactor, b.MaxLinearVelocity, b.MaxAngularVelocity,
					b.EnhancedInternalEdgeRemoval, b.AllowSleeping, b.UserData, b.CollisionGroup);
		}

		static bool SameGeometry2D(const Shape2DGeometry& a, const Shape2DGeometry& b)
		{
			if (a.index() != b.index())
				return false;
			if (const BoxShape2D* box = std::get_if<BoxShape2D>(&a))
				return Same2DBits(box->HalfExtents, std::get<BoxShape2D>(b).HalfExtents);
			if (const CircleShape2D* circle = std::get_if<CircleShape2D>(&a))
				return Same2DBits(circle->Radius, std::get<CircleShape2D>(b).Radius);
			const std::vector<glm::vec2>& points = std::get<PolygonShape2D>(a).Points;
			const std::vector<glm::vec2>& others = std::get<PolygonShape2D>(b).Points;
			return std::equal(points.begin(), points.end(), others.begin(), others.end(), [](const glm::vec2& left, const glm::vec2& right)
			{
				return Same2DBits(left, right);
			});
		}

		static bool SameShape2DDescription(const Body2DShapeDescription& a, const Body2DShapeDescription& b)
		{
			return std::equal(a.Colliders.begin(), a.Colliders.end(), b.Colliders.begin(), b.Colliders.end(),
				[](const Collider2DShapeDescription& left, const Collider2DShapeDescription& right)
			{
				return left.UserData == right.UserData && Same2DBits(left.Scale, right.Scale) && Same2DBits(left.Position, right.Position)
					&& Same2DBits(left.Angle, right.Angle) && SameGeometry2D(left.Geometry, right.Geometry);
			});
		}

		static bool Same2DColliders(const std::vector<Physics2DColliderPlan>& a, const std::vector<Physics2DColliderPlan>& b)
		{
			return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const Physics2DColliderPlan& left, const Physics2DColliderPlan& right)
			{
				return left.Entity == right.Entity && left.Type == right.Type && left.IsTrigger == right.IsTrigger;
			});
		}

		// `message` without a leading "<PHYSICS CODE>: ".
		static std::string_view Strip2DCode(std::string_view message, std::string_view code)
		{
			if (!code.empty() && message.starts_with(code) && message.size() > code.size() + 1)
				return message.substr(code.size() + 2);
			return message;
		}

		static PhysicsSubShapeKey Make2DSubShapeKey(const ContactEvent& record)
		{
			if (record.BodyA.GetValue() <= record.BodyB.GetValue())
				return { record.BodyA.GetValue(), record.SubShapeA, record.BodyB.GetValue(), record.SubShapeB, record.FromCharacter };
			return { record.BodyB.GetValue(), record.SubShapeB, record.BodyA.GetValue(), record.SubShapeA, record.FromCharacter };
		}

		static PhysicsEventType Get2DEnterType(PhysicsPairKind kind)
		{
			return kind == PhysicsPairKind::Trigger ? PhysicsEventType::TriggerEnter : PhysicsEventType::CollisionEnter;
		}

		static PhysicsEventType Get2DExitType(PhysicsPairKind kind)
		{
			return kind == PhysicsPairKind::Trigger ? PhysicsEventType::TriggerExit : PhysicsEventType::CollisionExit;
		}

		static bool Is2DEnter(PhysicsEventType type)
		{
			return type == PhysicsEventType::CollisionEnter || type == PhysicsEventType::TriggerEnter;
		}

		static std::optional<size_t> Find2DGatheredSide(const PhysicsSubShapeContact& contact, UUID collider)
		{
			for (size_t side = 0; side < 2; ++side)
			{
				if (contact.Colliders[side] == collider && contact.Owners[side] != collider)
					return side;
			}
			return std::nullopt;
		}

		static PhysicsPairKey Make2DColliderPairKey(UUID collider, UUID partner, PhysicsPairKind kind)
		{
			const bool colliderIsLow = collider < partner;
			return PhysicsPairKey{ .Low = colliderIsLow ? collider : partner, .High = colliderIsLow ? partner : collider, .Kind = kind, .Collider = collider };
		}

		static size_t Get2DLowSide(const PhysicsPairKey& key, const PhysicsSubShapeContact& contact)
		{
			if (!key.IsColliderPair())
				return contact.Owners[0] == key.Low ? 0 : 1;
			const size_t colliderSide = Find2DGatheredSide(contact, key.Collider).value_or(0);
			return key.Low == key.Collider ? colliderSide : 1 - colliderSide;
		}

		static First2DContact Make2DFirstContact(const PhysicsSubShapeKey& key, const PhysicsSubShapeContact& contact, size_t lowSide)
		{
			const std::array<uint32_t, 2> subShapes = { key.SubShapeA, key.SubShapeB };
			First2DContact candidate;
			candidate.LowCollider = contact.Colliders[lowSide];
			candidate.HighCollider = contact.Colliders[1 - lowSide];
			candidate.LowSubShape = subShapes[lowSide];
			candidate.HighSubShape = subShapes[1 - lowSide];
			candidate.Point = contact.Point;
			candidate.Normal = lowSide == 0 ? contact.Normal : -contact.Normal;
			candidate.RelativeSpeed = contact.RelativeSpeed;
			candidate.Bits = { std::bit_cast<uint32_t>(candidate.Point.x), std::bit_cast<uint32_t>(candidate.Point.y), std::bit_cast<uint32_t>(candidate.Point.z),
				std::bit_cast<uint32_t>(candidate.Normal.x), std::bit_cast<uint32_t>(candidate.Normal.y), std::bit_cast<uint32_t>(candidate.Normal.z),
				std::bit_cast<uint32_t>(candidate.RelativeSpeed) };
			return candidate;
		}

		static PhysicsEventOrderKey Get2DPairOrderKey(const PhysicsPairKey& key, PhysicsEventType type, UUID colliderBody)
		{
			if (!key.IsColliderPair())
				return PhysicsEventOrderKey{ .Low = key.Low, .High = key.High, .Type = type, .Collider = UUID() };
			const UUID partner = key.Low == key.Collider ? key.High : key.Low;
			const bool bodyIsLow = colliderBody < partner;
			return PhysicsEventOrderKey{ .Low = bodyIsLow ? colliderBody : partner, .High = bodyIsLow ? partner : colliderBody, .Type = type, .Collider = key.Collider };
		}

		static void Append2DFloat(std::vector<std::byte>& bytes, float value)
		{
			const auto bits = std::bit_cast<uint32_t>(value);
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffu));
		}

		// The body origins of one owner, in the order its bodies are listed, hashed and reported: the solid body (a
		// RigidBody2D's or an implicit static one) before the implicit sensor body.
		constexpr std::array<PhysicsBodyOrigin, 3> Body2DOrigins = { PhysicsBodyOrigin::RigidBody, PhysicsBodyOrigin::ImplicitStatic,
			PhysicsBodyOrigin::ImplicitSensor };

	}

	// --- State: signals and structure ---------------------------------------------------------------------------------

	void Physics2DSystem::State::ConnectSignals()
	{
		entt::registry& registry = Specification.RuntimeScene->GetRegistry();
		Utils::Connect2DSignals<RigidBody2DComponent, &State::On2DComponentChanged>(registry, *this);
		Utils::Connect2DSignals<BoxCollider2DComponent, &State::On2DComponentChanged>(registry, *this);
		Utils::Connect2DSignals<CircleCollider2DComponent, &State::On2DComponentChanged>(registry, *this);
		Utils::Connect2DSignals<PolygonCollider2DComponent, &State::On2DComponentChanged>(registry, *this);
		// A 3D physics component added to or removed from an entity that holds a 2D one makes or ends PHYSICS2D_MIXED_WORLDS.
		Utils::Connect2DSignals<RigidBodyComponent, &State::On3DComponentChanged>(registry, *this);
		Utils::Connect2DSignals<BoxColliderComponent, &State::On3DComponentChanged>(registry, *this);
		Utils::Connect2DSignals<SphereColliderComponent, &State::On3DComponentChanged>(registry, *this);
		Utils::Connect2DSignals<CapsuleColliderComponent, &State::On3DComponentChanged>(registry, *this);
		Utils::Connect2DSignals<MeshColliderComponent, &State::On3DComponentChanged>(registry, *this);
		Utils::Connect2DSignals<CharacterControllerComponent, &State::On3DComponentChanged>(registry, *this);
		registry.on_construct<DisabledTag>().connect<&State::On2DComponentChanged>(*this);
		registry.on_destroy<DisabledTag>().connect<&State::On2DComponentChanged>(*this);
		registry.on_construct<PendingDestroyTag>().connect<&State::OnEntityLeavingPlay>(*this);
		registry.on_construct<HierarchyDisabledTag>().connect<&State::OnEntityLeavingPlay>(*this);
	}

	void Physics2DSystem::State::DisconnectSignals()
	{
		entt::registry& registry = Specification.RuntimeScene->GetRegistry();
		Utils::Disconnect2DSignals<RigidBody2DComponent>(registry, *this);
		Utils::Disconnect2DSignals<BoxCollider2DComponent>(registry, *this);
		Utils::Disconnect2DSignals<CircleCollider2DComponent>(registry, *this);
		Utils::Disconnect2DSignals<PolygonCollider2DComponent>(registry, *this);
		Utils::Disconnect2DSignals<RigidBodyComponent>(registry, *this);
		Utils::Disconnect2DSignals<BoxColliderComponent>(registry, *this);
		Utils::Disconnect2DSignals<SphereColliderComponent>(registry, *this);
		Utils::Disconnect2DSignals<CapsuleColliderComponent>(registry, *this);
		Utils::Disconnect2DSignals<MeshColliderComponent>(registry, *this);
		Utils::Disconnect2DSignals<CharacterControllerComponent>(registry, *this);
		registry.on_construct<DisabledTag>().disconnect(this);
		registry.on_destroy<DisabledTag>().disconnect(this);
		registry.on_construct<PendingDestroyTag>().disconnect(this);
		registry.on_construct<HierarchyDisabledTag>().disconnect(this);
	}

	void Physics2DSystem::State::On2DComponentChanged(entt::registry& registry, entt::entity entity)
	{
		ComponentsChanged = true;
		// An entity being destroyed may have lost its IDComponent already; the hierarchy change covers it.
		if (const IDComponent* id = registry.try_get<IDComponent>(entity))
			ChangedEntities.insert(id->ID);
	}

	void Physics2DSystem::State::On3DComponentChanged(entt::registry& registry, entt::entity entity)
	{
		if (!registry.any_of<RigidBody2DComponent, BoxCollider2DComponent, CircleCollider2DComponent, PolygonCollider2DComponent>(entity))
			return;
		On2DComponentChanged(registry, entity);
	}

	void Physics2DSystem::State::OnEntityLeavingPlay(entt::registry& registry, entt::entity entity)
	{
		if (const IDComponent* id = registry.try_get<IDComponent>(entity))
			FlushCandidates.insert(id->ID);
	}

	bool Physics2DSystem::State::SceneHolds2DComponents() const
	{
		const entt::registry& registry = Specification.RuntimeScene->GetRegistry();
		const auto holds = [&registry]<typename Component>()
		{
			const auto* storage = registry.storage<Component>();
			return storage != nullptr && !storage->empty();
		};
		return holds.template operator()<RigidBody2DComponent>() || holds.template operator()<BoxCollider2DComponent>()
			|| holds.template operator()<CircleCollider2DComponent>() || holds.template operator()<PolygonCollider2DComponent>();
	}

	bool Physics2DSystem::State::IsPresent(UUID entity) const
	{
		const ConstEntity found = std::as_const(*Specification.RuntimeScene).FindEntityByID(entity);
		return found.IsValid() && !found.HasComponent<HierarchyDisabledTag>();
	}

	bool Physics2DSystem::State::HasHierarchyChanged()
	{
		const Scene& scene = *Specification.RuntimeScene;
		if (scene.GetRevision() == SeenRevision)
			return false;
		const std::vector<std::pair<UUID, UUID>> previous = std::move(SeenHierarchy);
		RecordHierarchy();
		return SeenHierarchy != previous;
	}

	void Physics2DSystem::State::RecordHierarchy()
	{
		const Scene& scene = *Specification.RuntimeScene;
		SeenRevision = scene.GetRevision();
		SeenHierarchy.clear();
		const std::span<const UUID> order = scene.GetCanonicalOrder();
		SeenHierarchy.reserve(order.size());
		for (const UUID id : order)
			SeenHierarchy.emplace_back(id, scene.FindEntityByID(id).GetComponent<RelationshipComponent>().Parent);
	}

	Result<Physics2DWorld*> Physics2DSystem::State::EnsureWorld()
	{
		if (World != nullptr)
			return World.get();
		if (WorldError.has_value())
			return std::unexpected(*WorldError);
		Physics2DWorldSpecification specification;
		specification.Gravity = Gravity;
		specification.Layers = Layers;
		specification.Limits = Specification.Limits;
		Result<Scope<Physics2DWorld>> world = Physics2DWorld::Create(specification);
		if (!world)
		{
			ENGINE_CORE_ERROR("The 2D physics world cannot be created: {}", world.error());
			WorldError = world.error();
			return std::unexpected(std::move(world).error().WithContext("while creating the 2D physics world"));
		}
		World = std::move(*world);
		return World.get();
	}

	// --- State: diagnostics -------------------------------------------------------------------------------------------

	void Physics2DSystem::State::RaiseDiagnostic(PhysicsDiagnostic diagnostic, bool alreadyLogged)
	{
		if (!RaisedDiagnostics.emplace(diagnostic.Code, diagnostic.Severity, diagnostic.Entity, diagnostic.Subject).second)
			return;

		if (!alreadyLogged)
		{
			if (diagnostic.Severity == DiagnosticSeverity::Error)
				ENGINE_CORE_ERROR("{}: {}", diagnostic.Code, diagnostic.Message);
			else
				ENGINE_CORE_WARN("{}: {}", diagnostic.Code, diagnostic.Message);
		}

		const auto position = std::upper_bound(Diagnostics.begin(), Diagnostics.end(), diagnostic, [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return std::tie(a.Entity, a.Code, a.Subject, a.Severity) < std::tie(b.Entity, b.Code, b.Subject, b.Severity);
		});
		Diagnostics.insert(position, diagnostic);
		PendingListenerDiagnostics.push_back(std::move(diagnostic));
	}

	void Physics2DSystem::State::RaiseBodyError(const Physics2DBodyPlan& plan, const Error& error, std::string_view field)
	{
		const Scene& scene = *Specification.RuntimeScene;
		std::string_view code = GetPhysicsDiagnosticCode(error);
		const std::string_view reason = Utils::Strip2DCode(error.GetMessageText(), code);
		// A refusal without a physics code (a pose beyond the physics range, the world's creation) is reported as the body's
		// invalid shape, so the body is still refused with a diagnostic, never an assert.
		const bool carriesCode = !code.empty();
		if (!carriesCode)
			code = PhysicsInvalidShapeCode;

		UUID entity = plan.Owner;
		std::string component;
		if (plan.Origin == PhysicsBodyOrigin::RigidBody && (code != PhysicsInvalidShapeCode || !carriesCode || !field.empty()))
			component = "RigidBody2D";
		if (component.empty() && code == PhysicsInvalidShapeCode)
		{
			if (error.GetLocation().Entity.IsValid())
				entity = error.GetLocation().Entity;
			else if (const std::optional<uint32_t> index = Utils::ParseColliderIndex(reason); index.has_value() && *index < plan.Colliders.size())
				entity = plan.Colliders[*index].Entity;
			const auto collider = std::ranges::find_if(plan.Colliders, [entity](const Physics2DColliderPlan& candidate)
			{
				return candidate.Entity == entity;
			});
			if (collider != plan.Colliders.end())
				component = std::string(GetCollider2DComponentName(collider->Type));
		}

		const ConstEntity owner = scene.FindEntityByID(plan.Owner);
		const std::string ownerName = owner.IsValid() ? owner.GetName() : std::string();
		PhysicsDiagnostic diagnostic;
		diagnostic.Code = std::string(code);
		diagnostic.Severity = DiagnosticSeverity::Error;
		diagnostic.Entity = entity;
		diagnostic.Component = std::move(component);
		diagnostic.Field = std::string(field);
		diagnostic.Message = std::format("the 2D body of '{}' ({}) is not created: {}", ownerName, plan.Owner, reason);
		diagnostic.Hint = error.GetHint().empty() ? std::string("fix the component values the message names; the rest of the scene keeps simulating")
												  : error.GetHint();
		if (code == PhysicsLimitExceededCode)
			diagnostic.Subject = "bodies";
		RaiseDiagnostic(std::move(diagnostic));
	}

	void Physics2DSystem::State::DeliverPendingDiagnostics()
	{
		if (PendingListenerDiagnostics.empty())
			return;
		std::vector<PhysicsDiagnostic> pending = std::move(PendingListenerDiagnostics);
		PendingListenerDiagnostics.clear();
		if (Listener == nullptr)
			return;
		Dispatching = true;
		for (const PhysicsDiagnostic& diagnostic : pending)
			Listener->OnPhysics2DDiagnostic(diagnostic);
		Dispatching = false;
	}

	// --- State: contacts and pairs (PhysicsSystem.cpp's, over the 2D tables) ------------------------------------------

	PhysicsSubShapeContact& Physics2DSystem::State::AddContact(const PhysicsSubShapeKey& key, const PhysicsPairKey& pair)
	{
		PhysicsSubShapeContact& contact = Contacts[key];
		contact.Pair = pair;
		const auto found = Pairs.find(pair);
		ENGINE_CORE_VERIFY(found != Pairs.end(), "Physics2DSystem: a contact is added to a pair that does not exist");
		if (found != Pairs.end())
			found->second.Contacts.insert(key);
		ContactsByBody[key.BodyA].insert(key);
		ContactsByBody[key.BodyB].insert(key);
		return contact;
	}

	void Physics2DSystem::State::EraseContact(const PhysicsSubShapeKey& key)
	{
		const auto found = Contacts.find(key);
		if (found == Contacts.end())
			return;
		PhysicsSubShapeContact& contact = found->second;
		if (const auto pair = Pairs.find(contact.Pair); pair != Pairs.end())
			pair->second.Contacts.erase(key);
		for (const PhysicsPairKey& colliderPair : contact.ColliderPairs)
		{
			if (const auto pair = Pairs.find(colliderPair); pair != Pairs.end())
				pair->second.Contacts.erase(key);
		}
		for (size_t side = 0; side < 2; ++side)
		{
			if (!contact.Colliders[side].IsValid() || contact.Colliders[side] == contact.Owners[side])
				continue;
			if (const auto gathered = GatheredContacts.find(contact.Colliders[side]); gathered != GatheredContacts.end())
			{
				gathered->second.erase(key);
				if (gathered->second.empty())
					GatheredContacts.erase(gathered);
			}
		}
		for (const uint32_t body : { key.BodyA, key.BodyB })
		{
			if (const auto contacts = ContactsByBody.find(body); contacts != ContactsByBody.end())
			{
				contacts->second.erase(key);
				if (contacts->second.empty())
					ContactsByBody.erase(contacts);
			}
		}
		EligibleDormantContacts.erase(key);
		Contacts.erase(found);
	}

	void Physics2DSystem::State::ErasePair(const PhysicsPairKey& key)
	{
		const auto pair = Pairs.find(key);
		if (pair == Pairs.end())
			return;
		const std::vector<PhysicsSubShapeKey> contacts(pair->second.Contacts.begin(), pair->second.Contacts.end());
		for (const PhysicsSubShapeKey& contact : contacts)
		{
			if (!key.IsColliderPair())
			{
				EraseContact(contact);
				continue;
			}
			const auto found = Contacts.find(contact);
			ENGINE_CORE_VERIFY(found != Contacts.end(), "Physics2DSystem: a collider pair counts a contact that does not exist (Pairs and Contacts out of step)");
			if (found != Contacts.end())
				UnlinkColliderPair(contact, found->second, key);
		}
		Pairs.erase(key);
		for (const UUID owner : { key.Low, key.High })
		{
			if (const auto owned = PairsByOwner.find(owner); owned != PairsByOwner.end())
			{
				owned->second.erase(key);
				if (owned->second.empty())
					PairsByOwner.erase(owned);
			}
		}
		if (key.IsColliderPair())
		{
			if (const auto owned = ColliderPairsByCollider.find(key.Collider); owned != ColliderPairsByCollider.end())
			{
				owned->second.erase(key);
				if (owned->second.empty())
					ColliderPairsByCollider.erase(owned);
			}
		}
	}

	void Physics2DSystem::State::PurgeBodyContacts(uint32_t handle, std::set<UUID>& partners)
	{
		const auto found = ContactsByBody.find(handle);
		if (found == ContactsByBody.end())
			return;
		const std::set<PhysicsSubShapeKey> contacts = found->second;
		for (const PhysicsSubShapeKey& contact : contacts)
		{
			const auto state = Contacts.find(contact);
			ENGINE_CORE_VERIFY(state != Contacts.end(), "Physics2DSystem: a body's contact does not exist (ContactsByBody and Contacts out of step)");
			if (state == Contacts.end())
				continue;
			std::vector<PhysicsPairKey> keys = { state->second.Pair };
			keys.insert(keys.end(), state->second.ColliderPairs.begin(), state->second.ColliderPairs.end());
			EraseContact(contact);
			for (const PhysicsPairKey& key : keys)
			{
				const auto pairEntry = Pairs.find(key);
				ENGINE_CORE_VERIFY(pairEntry != Pairs.end(), "Physics2DSystem: a contact names a pair that does not exist (Contacts and Pairs out of step)");
				if (pairEntry == Pairs.end())
					continue;
				PhysicsPair& pair = pairEntry->second;
				if (!key.IsColliderPair())
				{
					partners.insert(key.Low);
					partners.insert(key.High);
				}
				if (pair.Contacts.empty())
				{
					if (pair.Active)
						pair.Unconfirmed = true; // PostStep ends it unless a contact re-establishes it
					else
						ErasePair(key);
				}
			}
		}
	}

	void Physics2DSystem::State::SetContactSides(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const std::array<UUID, 2>& owners,
		const std::array<UUID, 2>& colliders)
	{
		for (size_t side = 0; side < 2; ++side)
		{
			const UUID previous = contact.Colliders[side];
			const bool wasGathered = previous.IsValid() && previous != contact.Owners[side];
			if (!wasGathered || (previous == colliders[side] && owners[side] == contact.Owners[side]))
				continue;
			if (const auto gathered = GatheredContacts.find(previous); gathered != GatheredContacts.end())
			{
				gathered->second.erase(key);
				if (gathered->second.empty())
					GatheredContacts.erase(gathered);
			}
			const std::vector<PhysicsPairKey> linked = contact.ColliderPairs;
			for (const PhysicsPairKey& pair : linked)
			{
				if (pair.Collider == previous)
					UnlinkColliderPair(key, contact, pair);
			}
		}
		contact.Owners = owners;
		contact.Colliders = colliders;
		for (size_t side = 0; side < 2; ++side)
		{
			if (colliders[side].IsValid() && colliders[side] != owners[side])
				GatheredContacts[colliders[side]].insert(key);
		}
	}

	void Physics2DSystem::State::LinkColliderPair(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const PhysicsPairKey& pair)
	{
		const auto found = Pairs.find(pair);
		ENGINE_CORE_VERIFY(found != Pairs.end(), "Physics2DSystem: a contact is linked to a collider pair that does not exist");
		if (found == Pairs.end())
			return;
		found->second.Contacts.insert(key);
		const auto position = std::lower_bound(contact.ColliderPairs.begin(), contact.ColliderPairs.end(), pair);
		if (position == contact.ColliderPairs.end() || *position != pair)
			contact.ColliderPairs.insert(position, pair);
		PairsByOwner[pair.Low].insert(pair);
		PairsByOwner[pair.High].insert(pair);
		ColliderPairsByCollider[pair.Collider].insert(pair);
	}

	void Physics2DSystem::State::UnlinkColliderPair(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const PhysicsPairKey& pair)
	{
		if (const auto found = Pairs.find(pair); found != Pairs.end())
			found->second.Contacts.erase(key);
		std::erase(contact.ColliderPairs, pair);
	}

	bool Physics2DSystem::State::IsScriptedGatheredCollider(UUID entity) const
	{
		if (!GatheredColliders.contains(entity))
			return false;
		const ConstEntity found = std::as_const(*Specification.RuntimeScene).FindEntityByID(entity);
		if (!found.IsValid() || found.HasComponent<HierarchyDisabledTag>())
			return false;
		const ScriptComponent* script = found.TryGetComponent<ScriptComponent>();
		return script != nullptr && std::ranges::any_of(script->Scripts, [](const ScriptSlot& slot)
		{
			return slot.Script.IsValid();
		});
	}

	void Physics2DSystem::State::CollectPartners(UUID owner, std::set<UUID>& partners) const
	{
		const auto owned = PairsByOwner.find(owner);
		if (owned == PairsByOwner.end())
			return;
		for (const PhysicsPairKey& key : owned->second)
		{
			if (!key.IsColliderPair())
				partners.insert(key.Low == owner ? key.High : key.Low);
		}
	}

	void Physics2DSystem::State::WakeOwners(const std::set<UUID>& owners)
	{
		if (World == nullptr)
			return;
		for (const UUID owner : owners)
		{
			for (const PhysicsBodyOrigin origin : Utils::Body2DOrigins)
			{
				const Physics2DBodyEntry* entry = FindOwnedBody(owner, origin);
				if (entry != nullptr && entry->Plan.MotionType != PhysicsMotionType::Static)
					World->WakeUp(entry->Handle);
			}
		}
	}

	// --- State: bodies ------------------------------------------------------------------------------------------------

	const Physics2DBodyEntry* Physics2DSystem::State::FindOwnedBody(UUID owner, PhysicsBodyOrigin origin) const
	{
		const auto found = BodiesByOwner.find({ owner, origin });
		return found != BodiesByOwner.end() ? FindBody(found->second) : nullptr;
	}

	Physics2DBodyEntry* Physics2DSystem::State::FindOwnedBody(UUID owner, PhysicsBodyOrigin origin)
	{
		const auto found = BodiesByOwner.find({ owner, origin });
		return found != BodiesByOwner.end() ? FindBody(found->second) : nullptr;
	}

	const Physics2DBodyEntry* Physics2DSystem::State::FindBody(uint32_t handle) const
	{
		const auto found = Bodies.find(handle);
		ENGINE_CORE_VERIFY(found != Bodies.end(), "Physics2DSystem: an index names body 0x{:08x}, which the body table does not hold", handle);
		return found != Bodies.end() ? &found->second : nullptr;
	}

	Physics2DBodyEntry* Physics2DSystem::State::FindBody(uint32_t handle)
	{
		const auto found = Bodies.find(handle);
		ENGINE_CORE_VERIFY(found != Bodies.end(), "Physics2DSystem: an index names body 0x{:08x}, which the body table does not hold", handle);
		return found != Bodies.end() ? &found->second : nullptr;
	}

	Result<Physics2DBodyEntry*> Physics2DSystem::State::FindRigidBody(UUID entity)
	{
		Physics2DBodyEntry* entry = FindOwnedBody(entity, PhysicsBodyOrigin::RigidBody);
		if (entry == nullptr || !IsPresent(entity))
			return Utils::MakeNo2DBodyError(entity);
		return entry;
	}

	Result<const Physics2DBodyEntry*> Physics2DSystem::State::FindRigidBody(UUID entity) const
	{
		const Physics2DBodyEntry* entry = FindOwnedBody(entity, PhysicsBodyOrigin::RigidBody);
		if (entry == nullptr || !IsPresent(entity))
			return Utils::MakeNo2DBodyError(entity);
		return entry;
	}

	std::set<UUID> Physics2DSystem::State::FindShapeChanges(bool hierarchyChanged) const
	{
		const entt::registry& registry = Specification.RuntimeScene->GetRegistry();
		const auto sameParts = [](const PhysicsShapeInput& input, const TransformComponent& local)
		{
			switch (input.Parts)
			{
				case PhysicsShapeInputParts::Scale:
					return Utils::Same2DBits(local.Scale, input.Local.Scale);
				case PhysicsShapeInputParts::ScaleRotation:
					return Utils::Same2DBits(local.Scale, input.Local.Scale) && Utils::Same2DBits(local.Rotation, input.Local.Rotation);
				case PhysicsShapeInputParts::Whole:
					break;
			}
			return Utils::Same2DBits(local.Translation, input.Local.Translation) && Utils::Same2DBits(local.Rotation, input.Local.Rotation)
				&& Utils::Same2DBits(local.Scale, input.Local.Scale);
		};
		std::set<UUID> owners;
		for (const auto& [handle, entry] : Bodies)
		{
			bool changed = false;
			if (hierarchyChanged && IsPresent(entry.Owner))
			{
				const std::vector<PhysicsShapeInput> current = CaptureShapeInputs(entry.Plan);
				changed = !std::equal(current.begin(), current.end(), entry.ShapeInputs.begin(), entry.ShapeInputs.end(),
					[&sameParts](const PhysicsShapeInput& now, const PhysicsShapeInput& then)
				{
					return now.Entity == then.Entity && now.Parts == then.Parts && sameParts(then, now.Local);
				});
			}
			else
			{
				changed = std::ranges::any_of(entry.ShapeInputs, [&registry, &sameParts](const PhysicsShapeInput& input)
				{
					const TransformComponent* local = registry.valid(input.Entity) ? registry.try_get<TransformComponent>(input.Entity) : nullptr;
					return local == nullptr || !sameParts(input, *local);
				});
			}
			if (changed)
				owners.insert(entry.Owner);
		}
		return owners;
	}

	std::vector<PhysicsShapeInput> Physics2DSystem::State::CaptureShapeInputs(const Physics2DBodyPlan& plan) const
	{
		// What a shape bakes in: the owner's world scale (the scales from the root down to the owner, and the rotations below
		// a non-uniformly scaled ancestor), then every local transform below the owner down to each collider, each entity once.
		const Scene& scene = *Specification.RuntimeScene;
		std::vector<PhysicsShapeInput> inputs;
		const ConstEntity owner = scene.FindEntityByID(plan.Owner);
		std::vector<ConstEntity> chain;
		for (ConstEntity current = owner; current.IsValid(); current = current.GetParent())
			chain.push_back(current);
		std::set<entt::entity> captured;
		bool nonUniformAbove = false;
		for (auto link = chain.rbegin(); link != chain.rend(); ++link)
		{
			const TransformComponent& local = link->GetComponent<TransformComponent>();
			const PhysicsShapeInputParts parts = nonUniformAbove ? PhysicsShapeInputParts::ScaleRotation : PhysicsShapeInputParts::Scale;
			inputs.push_back(PhysicsShapeInput{ .Entity = link->GetHandle(), .Parts = parts, .Local = local });
			captured.insert(link->GetHandle());
			const glm::vec3 magnitude = glm::abs(local.Scale);
			nonUniformAbove = nonUniformAbove || magnitude.x != magnitude.y || magnitude.y != magnitude.z;
		}
		for (const Physics2DColliderPlan& collider : plan.Colliders)
		{
			for (ConstEntity current = scene.FindEntityByID(collider.Entity); current.IsValid() && current != owner; current = current.GetParent())
			{
				if (captured.insert(current.GetHandle()).second)
				{
					inputs.push_back(
						PhysicsShapeInput{ .Entity = current.GetHandle(), .Parts = PhysicsShapeInputParts::Whole, .Local = current.GetComponent<TransformComponent>() });
				}
			}
		}
		return inputs;
	}

	Result<Ref<const PhysicsShape>> Physics2DSystem::State::BuildShape(const Physics2DBodyPlan& plan, Body2DShapeDescription& description)
	{
		ENGINE_TRY_ASSIGN(description, DescribePhysics2DBodyShape(*Specification.RuntimeScene, plan));
		ENGINE_TRY_ASSIGN(const BodyShapeDescription laid, MakeBody2DShape(description));
		return PhysicsShape::Create(laid);
	}

	bool Physics2DSystem::State::CreateBody(const Physics2DBodyPlan& plan, std::optional<std::pair<glm::vec2, float>> velocities)
	{
		const Scene& scene = *Specification.RuntimeScene;
		const ConstEntity owner = scene.FindEntityByID(plan.Owner);
		Body2DShapeDescription description;
		Result<Ref<const PhysicsShape>> shape = BuildShape(plan, description);
		if (!shape)
		{
			RaiseBodyError(plan, shape.error());
			return false;
		}

		const glm::mat4 world = Utils::Get2DWorldMatrix(owner);
		Body2DDescription body = Utils::MakeBody2DSettings(plan, owner);
		const Body2DDescription settings = body;
		if (plan.MotionType == PhysicsMotionType::Dynamic)
		{
			// The world checks the same; checked here, the refusal names the RigidBody2D's field.
			if (const Status checked = (*shape)->CheckDynamicBody(body.Mass, body.FixedRotation ? Physics2DTranslationDofs : Physics2DDofs); !checked)
			{
				RaiseBodyError(plan, checked.error(), "Mass");
				return false;
			}
		}
		const Result<Physics2DWorld*> physicsWorld = EnsureWorld();
		if (!physicsWorld)
		{
			RaiseBodyError(plan, physicsWorld.error());
			return false;
		}
		body.Shape = *shape;
		body.Pose = Utils::GetPlanarPose(owner, world);
		if (velocities.has_value())
		{
			body.LinearVelocity = velocities->first;
			body.AngularVelocity = velocities->second;
		}
		else if (plan.Origin == PhysicsBodyOrigin::RigidBody && plan.MotionType == PhysicsMotionType::Dynamic)
		{
			// Only Dynamic bodies start moving: a Kinematic body follows its Transform.
			const RigidBody2DComponent& component = owner.GetComponent<RigidBody2DComponent>();
			body.LinearVelocity = component.InitialLinearVelocity;
			body.AngularVelocity = component.InitialAngularVelocity;
		}
		const Result<BodyHandle> handle = (*physicsWorld)->CreateBody(body);
		if (!handle)
		{
			RaiseBodyError(plan, handle.error());
			return false;
		}

		Physics2DBodyEntry entry;
		entry.Handle = *handle;
		entry.Owner = plan.Owner;
		entry.Origin = plan.Origin;
		entry.Shape = std::move(*shape);
		const TransformComponent& local = owner.GetComponent<TransformComponent>();
		entry.WrittenTranslation = local.Translation;
		entry.WrittenRotation = local.Rotation;
		entry.PlacedMatrix = world;
		entry.PlacedPose = body.Pose;
		entry.Settings = settings;
		entry.ShapeDescription = std::move(description);
		const uint32_t value = handle->GetValue();
		BodiesByOwner[{ plan.Owner, plan.Origin }] = value;
		AssignPlan(Bodies.emplace(value, std::move(entry)).first->second, plan);
		return true;
	}

	void Physics2DSystem::State::AssignPlan(Physics2DBodyEntry& entry, const Physics2DBodyPlan& plan)
	{
		const uint32_t handle = entry.Handle.GetValue();
		for (const UUID collider : entry.Colliders)
		{
			if (const auto gathered = GatheredColliders.find(collider); gathered != GatheredColliders.end() && gathered->second == handle)
				GatheredColliders.erase(gathered);
		}
		entry.Plan = plan;
		entry.Colliders.clear();
		for (const Physics2DColliderPlan& collider : plan.Colliders)
		{
			entry.Colliders.push_back(collider.Entity);
			if (collider.Entity != plan.Owner)
				GatheredColliders[collider.Entity] = handle;
		}
		entry.ShapeInputs = CaptureShapeInputs(plan);
	}

	void Physics2DSystem::State::RemoveBody(uint32_t handle, std::set<UUID>& woken)
	{
		const auto found = Bodies.find(handle);
		if (found == Bodies.end())
			return;
		const Physics2DBodyEntry& entry = found->second;
		PurgeBodyContacts(handle, woken);
		World->DestroyBody(entry.Handle);
		BodiesByOwner.erase({ entry.Owner, entry.Origin });
		for (const UUID collider : entry.Colliders)
		{
			if (const auto gathered = GatheredColliders.find(collider); gathered != GatheredColliders.end() && gathered->second == handle)
				GatheredColliders.erase(gathered);
		}
		Bodies.erase(found);
	}

	bool Physics2DSystem::State::ReplaceShape(Physics2DBodyEntry& entry, const Physics2DBodyPlan& plan, std::set<UUID>& woken)
	{
		const uint32_t handle = entry.Handle.GetValue();
		Result<Body2DShapeDescription> description = DescribePhysics2DBodyShape(*Specification.RuntimeScene, plan);
		if (!description)
		{
			// A body refused after an edit: removed, its pairs end at PostStep.
			RaiseBodyError(plan, description.error());
			RemoveBody(handle, woken);
			return false;
		}
		// An input written back to the same value changes nothing.
		if (!Utils::SameShape2DDescription(*description, entry.ShapeDescription))
		{
			Result<Ref<const PhysicsShape>> shape = MakeBody2DShape(*description).and_then([](const BodyShapeDescription& laid)
			{
				return PhysicsShape::Create(laid);
			});
			if (!shape)
			{
				RaiseBodyError(plan, shape.error());
				RemoveBody(handle, woken);
				return false;
			}
			if (plan.MotionType == PhysicsMotionType::Dynamic)
			{
				const PhysicsDofs dofs = entry.Settings.FixedRotation ? Physics2DTranslationDofs : Physics2DDofs;
				if (const Status checked = (*shape)->CheckDynamicBody(entry.Settings.Mass, dofs); !checked)
				{
					RaiseBodyError(plan, checked.error(), "Mass");
					RemoveBody(handle, woken);
					return false;
				}
			}
			if (const Status replaced = World->SetShape(entry.Handle, *shape); !replaced)
			{
				RaiseBodyError(plan, replaced.error());
				RemoveBody(handle, woken);
				return false;
			}
			entry.Shape = std::move(*shape);
			entry.ShapeDescription = std::move(*description);
		}
		AssignPlan(entry, plan);
		ReattributedBodies.insert(handle);
		return true;
	}

	void Physics2DSystem::State::Rebuild(bool hierarchyChanged, std::set<UUID>& woken)
	{
		Scene& scene = *Specification.RuntimeScene;
		const Physics2DComposition composition = ComposePhysics2DBodies(scene, Layers, Specification.Limits.MaxBodies);
		++Compositions;
		bool loggedBodyLimit = false;
		for (const PhysicsDiagnostic& diagnostic : composition.Diagnostics)
		{
			const bool isBodyLimit = diagnostic.Code == PhysicsLimitExceededCode && diagnostic.Subject == "bodies";
			RaiseDiagnostic(diagnostic, isBodyLimit && loggedBodyLimit);
			loggedBodyLimit = loggedBodyLimit || isBodyLimit;
		}

		std::set<std::pair<UUID, PhysicsBodyOrigin>> wanted;
		for (const Physics2DBodyPlan& plan : composition.Bodies)
		{
			if (plan.IsCreatable)
				wanted.emplace(plan.Owner, plan.Origin);
		}

		// Removals first, except for entities pending destruction or disabled, which keep their bodies until FlushDestroyed.
		std::vector<std::pair<UUID, PhysicsBodyOrigin>> stale;
		for (const auto& [key, handle] : BodiesByOwner)
		{
			if (!wanted.contains(key) && IsPresent(key.first))
				stale.push_back(key);
		}
		std::sort(stale.begin(), stale.end());
		for (const auto& [owner, origin] : stale)
		{
			if (const Physics2DBodyEntry* entry = FindOwnedBody(owner, origin))
				RemoveBody(entry->Handle.GetValue(), woken);
		}

		const std::set<UUID> shapeChanges = FindShapeChanges(hierarchyChanged);
		for (const Physics2DBodyPlan& plan : composition.Bodies)
		{
			if (!plan.IsCreatable)
				continue;
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			Physics2DBodyEntry* existing = FindOwnedBody(plan.Owner, plan.Origin);
			if (existing == nullptr)
			{
				static_cast<void>(CreateBody(plan, std::nullopt));
				continue;
			}

			const bool settingsChanged = existing->Plan.MotionType != plan.MotionType || existing->Plan.IsSensor != plan.IsSensor
				|| !Utils::SameBody2DSettings(existing->Settings, Utils::MakeBody2DSettings(plan, owner));
			if (settingsChanged)
			{
				// A RigidBody2D change creates the body again, with its current velocities.
				const uint32_t handle = existing->Handle.GetValue();
				const auto velocities = std::make_pair(World->GetLinearVelocity(existing->Handle), World->GetAngularVelocity(existing->Handle));
				RemoveBody(handle, woken);
				static_cast<void>(CreateBody(plan, velocities));
				continue;
			}

			const bool touched = shapeChanges.contains(plan.Owner) || ChangedEntities.contains(plan.Owner)
				|| std::ranges::any_of(plan.Colliders, [this](const Physics2DColliderPlan& collider)
			{
				return ChangedEntities.contains(collider.Entity);
			}) || !Utils::Same2DColliders(plan.Colliders, existing->Plan.Colliders);
			if (touched)
				static_cast<void>(ReplaceShape(*existing, plan, woken));
		}
	}

	void Physics2DSystem::State::RefreshShapes(const std::set<UUID>& owners, std::set<UUID>& woken)
	{
		for (const UUID owner : owners)
		{
			for (const PhysicsBodyOrigin origin : Utils::Body2DOrigins)
			{
				Physics2DBodyEntry* entry = FindOwnedBody(owner, origin);
				if (entry == nullptr || !IsPresent(owner))
					continue;
				const Physics2DBodyPlan plan = entry->Plan;
				static_cast<void>(ReplaceShape(*entry, plan, woken));
			}
		}
	}

	std::optional<PhysicsOverlapHit> Physics2DSystem::State::ResolveHit(BodyHandle body, uint32_t collider) const
	{
		const auto found = Bodies.find(body.GetValue());
		if (found == Bodies.end())
			return std::nullopt;
		const Physics2DBodyEntry& entry = found->second;
		const UUID entity = collider < entry.Colliders.size() ? entry.Colliders[collider] : entry.Owner;
		if (!IsPresent(entry.Owner) || !IsPresent(entity))
			return std::nullopt;
		return PhysicsOverlapHit{ .Entity = entity, .Body = entry.Owner };
	}

	BodyHandle Physics2DSystem::State::FindOwnedBodyHandle(UUID owner) const
	{
		for (const PhysicsBodyOrigin origin : Utils::Body2DOrigins)
		{
			if (const Physics2DBodyEntry* body = FindOwnedBody(owner, origin))
				return body->Handle;
		}
		return BodyHandle();
	}

	// --- Physics2DSystem ----------------------------------------------------------------------------------------------

	Physics2DSystem::Physics2DSystem(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	Physics2DSystem::~Physics2DSystem()
	{
		State& state = *m_State;
		if (state.Specification.RuntimeScene != nullptr)
			state.DisconnectSignals();
		// The bodies, then the world; no events.
		if (state.World != nullptr)
		{
			for (const auto& [handle, entry] : state.Bodies)
				state.World->DestroyBody(entry.Handle);
		}
		state.Bodies.clear();
		state.World.reset();
	}

	Result<Scope<Physics2DSystem>> Physics2DSystem::Create(const Physics2DSystemSpecification& specification)
	{
		if (specification.RuntimeScene == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "RuntimeScene: a 2D physics system needs the play session's runtime scene");
		if (specification.FixedHz == 0)
			return MakeError(ErrorCode::InvalidArgument, "FixedHz: the fixed rate must be at least 1 Hz (got 0)");
		if (const Status gravity = Utils::Check2DVector(specification.Gravity, MaxPhysicsGravity, "gravity"); !gravity)
		{
			return std::unexpected(Error(gravity.error())
					.WithContext("in the project's Physics2D.Gravity")
					.WithHint(std::format("give each component of Physics2D.Gravity a magnitude of at most {} m/s^2", MaxPhysicsGravity)));
		}
		// What Physics2DWorld::Create (PhysicsWorld::Create) checks, so the world's creation at the first 2D body cannot fail
		// for a reason the session could have reported at its start.
		if (!PhysicsEngine::IsInitialized())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "a 2D physics world needs the physics engine, which is not initialized")
					.WithHint("ProcessContext's Physics step initializes it"));
		}
		if (const Status limits = ValidatePhysicsWorldLimits(specification.Limits); !limits)
			return std::unexpected(Error(limits.error()).WithContext("in the 2D physics world's limits"));

		Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(specification.Layers, specification.Collisions);
		if (!layers)
			return std::unexpected(std::move(layers).error().WithContext("while reading the project's physics layers for the 2D world"));

		Scope<Physics2DSystem> system = CreateScope<Physics2DSystem>(ConstructionKey());
		State& state = *system->m_State;
		state.Specification = specification;
		state.Layers = std::move(*layers);
		state.Gravity = specification.Gravity;
		state.CollisionSteps = std::max<uint32_t>(1, (60 + specification.FixedHz - 1) / specification.FixedHz);
		state.ConnectSignals();

		// Every 2D body in canonical order, so queries work at tick 0; the world only when one is made.
		std::set<UUID> woken;
		if (state.SceneHolds2DComponents())
			state.Rebuild(true, woken);
		state.RecordHierarchy();
		state.ComponentsChanged = false;
		state.ChangedEntities.clear();
		state.FlushCandidates.clear();
		state.PendingListenerDiagnostics.clear(); // no listener yet: GetDiagnostics holds them
		return system;
	}

	void Physics2DSystem::PreStep(const SimStep& step)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Dispatching, "Physics2DSystem::PreStep was called from a physics listener callback");
		// A scene without a 2D component and without a world has nothing to compose or move, whatever changed (an entity
		// enabled or disabled, a 3D component): every 3D game's case.
		if (state.World == nullptr && !state.SceneHolds2DComponents())
		{
			state.ComponentsChanged = false;
			state.ChangedEntities.clear();
			state.DeliverPendingDiagnostics();
			return;
		}

		Scene& scene = *state.Specification.RuntimeScene;
		entt::registry& registry = scene.GetRegistry();
		const auto deltaTime = static_cast<float>(step.FixedDelta);
		std::set<UUID> woken;

		// Rebuilds, and the partners of destroyed bodies woken so the step reports what still touches.
		const bool hierarchyChanged = state.HasHierarchyChanged();
		if (state.ComponentsChanged || hierarchyChanged)
			state.Rebuild(hierarchyChanged, woken);
		else if (const std::set<UUID> changed = state.FindShapeChanges(false); !changed.empty())
			state.RefreshShapes(changed, woken);
		state.ComponentsChanged = false;
		state.ChangedEntities.clear();

		// Kinematic moves and teleports, in canonical order of the owners.
		if (state.World != nullptr && !state.Bodies.empty())
		{
			const std::vector<UUID> order(scene.GetCanonicalOrder().begin(), scene.GetCanonicalOrder().end());
			for (const UUID id : order)
			{
				const Entity entity = scene.FindEntityByID(id);
				if (!entity.IsValid() || entity.HasComponent<HierarchyDisabledTag>())
					continue;
				for (const PhysicsBodyOrigin origin : Utils::Body2DOrigins)
				{
					Physics2DBodyEntry* entry = state.FindOwnedBody(id, origin);
					if (entry == nullptr)
						continue;
					const glm::mat4 world = Utils::Get2DWorldMatrix(entity);
					switch (entry->Plan.MotionType)
					{
						case PhysicsMotionType::Kinematic:
						{
							const Physics2DPose target = Utils::GetPlanarPose(entity, world);
							if (!Utils::IsPlaceable2DPose(target))
							{
								// Not applied: the entity shows where the body is, and the body stops there.
								const Physics2DPose current = state.World->GetPose(entry->Handle);
								static_cast<void>(Utils::Write2DWorldPose(entity, current));
								state.World->MoveKinematic(entry->Handle, current, deltaTime);
								entry->PlacedPose = Utils::GetCurrentPlanarPose(entity);
								break;
							}
							// Teleported earlier in the tick (a write outside the steps): placed, never swept, and at rest there. A
							// write that left the planar pose as it was (the world Z) is no teleport: the body moves on to it.
							if (registry.all_of<InterpolationResetTag>(entity.GetHandle()) && !Utils::Same2DBits(target, entry->PlacedPose))
							{
								state.World->SetPose(entry->Handle, target, true);
								state.World->SetLinearVelocity(entry->Handle, glm::vec2(0.0f));
								state.World->SetAngularVelocity(entry->Handle, 0.0f);
								state.CollectPartners(id, woken);
							}
							else
							{
								state.World->MoveKinematic(entry->Handle, target, deltaTime);
							}
							entry->PlacedPose = target;
							break;
						}
						case PhysicsMotionType::Dynamic:
						{
							const TransformComponent& local = entity.GetComponent<TransformComponent>();
							if (Utils::Same2DBits(local.Translation, entry->WrittenTranslation) && Utils::Same2DBits(local.Rotation, entry->WrittenRotation))
								break;
							const Physics2DPose target = Utils::GetPlanarPose(entity, world);
							if (!Utils::IsPlaceable2DPose(target))
							{
								Utils::Restore2DLocalPose(entity, entry->WrittenTranslation, entry->WrittenRotation);
								break;
							}
							// A write that leaves the planar pose as it was at the last placement or write-back (the world Z)
							// moves and wakes nothing.
							if (!Utils::Same2DBits(target, entry->PlacedPose))
							{
								state.World->SetPose(entry->Handle, target, true);
								state.CollectPartners(id, woken);
								entry->PlacedPose = target;
							}
							entry->WrittenTranslation = local.Translation;
							entry->WrittenRotation = local.Rotation;
							break;
						}
						case PhysicsMotionType::Static:
						{
							if (Utils::Same2DBits(world, entry->PlacedMatrix))
								break;
							const Physics2DPose target = Utils::GetPlanarPose(entity, world);
							if (!Utils::IsPlaceable2DPose(target))
							{
								Utils::Restore2DLocalPose(entity, entry->WrittenTranslation, entry->WrittenRotation);
								break;
							}
							// Moved in the plane: placed with activation, since a static body cannot wake, so the world wakes what
							// rested on it and what it now touches. A world matrix that moved only out of the plane (the world Z)
							// leaves the body where it is and wakes nothing.
							if (!Utils::Same2DBits(target, entry->PlacedPose))
							{
								state.World->SetPose(entry->Handle, target, true);
								state.CollectPartners(id, woken);
								entry->PlacedPose = target;
							}
							const TransformComponent& local = entity.GetComponent<TransformComponent>();
							entry->WrittenTranslation = local.Translation;
							entry->WrittenRotation = local.Rotation;
							entry->PlacedMatrix = world;
							break;
						}
					}
				}
			}
			state.WakeOwners(woken);
		}

		// Dormant contacts a body of which is awake now: the step will report them again if they still touch.
		state.EligibleDormantContacts.clear();
		if (state.World != nullptr)
		{
			for (const auto& [contact, contactState] : state.Contacts)
			{
				if (!contactState.Dormant)
					continue;
				const BodyHandle a(contact.BodyA);
				const BodyHandle b(contact.BodyB);
				const bool awake = (state.World->IsBodyValid(a) && !state.World->IsSleeping(a)) || (state.World->IsBodyValid(b) && !state.World->IsSleeping(b));
				if (awake)
					state.EligibleDormantContacts.insert(contact);
			}
		}

		state.DeliverPendingDiagnostics();
	}

	void Physics2DSystem::Step(const SimStep& step)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Dispatching, "Physics2DSystem::Step was called from a physics listener callback");
		if (state.World == nullptr)
		{
			state.DeliverPendingDiagnostics();
			return;
		}
		const Status stepped = state.World->Step(static_cast<float>(step.FixedDelta), state.CollisionSteps);
		const PhysicsWorldStats stats = state.World->GetStats();
		const PhysicsWorldLimits& limits = state.Specification.Limits;
		if (!stepped)
		{
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsLimitExceededCode);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Subject = stats.ContactConstraintCount >= limits.MaxContactConstraints ? "contactConstraints" : "bodyPairs";
			diagnostic.Message = std::format("the 2D physics step at tick {} overflowed: {}", step.Tick,
				Utils::Strip2DCode(stepped.error().GetMessageText(), PhysicsLimitExceededCode));
			diagnostic.Hint = "fewer 2D bodies touching at once, or fewer 2D bodies in the scene";
			state.RaiseDiagnostic(std::move(diagnostic));
		}

		// A warning when a count first reaches LimitWarningFraction of its limit; the world has logged it already.
		const std::array<std::tuple<std::string_view, uint32_t, uint32_t>, 3> counts = { {
			{ "bodies", stats.BodyCount, limits.MaxBodies },
			{ "bodyPairs", stats.BodyPairCount, limits.MaxBodyPairs },
			{ "contactConstraints", stats.ContactConstraintCount, limits.MaxContactConstraints },
		} };
		for (const auto& [subject, count, limit] : counts)
		{
			if (static_cast<float>(count) < PhysicsWorldLimits::LimitWarningFraction * static_cast<float>(limit))
				continue;
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsLimitExceededCode);
			diagnostic.Severity = DiagnosticSeverity::Warning;
			diagnostic.Subject = std::string(subject);
			diagnostic.Message = std::format("the 2D physics world holds {} {} of at most {}", count, subject, limit);
			diagnostic.Hint = "merge 2D colliders into compounds or remove 2D bodies before the limit is reached";
			state.RaiseDiagnostic(std::move(diagnostic), true);
		}
		state.DeliverPendingDiagnostics();
	}

	void Physics2DSystem::PostStep(const SimStep& step)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Dispatching, "Physics2DSystem::PostStep was called from a physics listener callback");
		state.LastEvents.clear();
		if (state.World == nullptr)
			return;
		Scene& scene = *state.Specification.RuntimeScene;

		// Write-back, in canonical order: Dynamic bodies that are awake or have a parent.
		if (!state.Bodies.empty())
		{
			const std::vector<UUID> order(scene.GetCanonicalOrder().begin(), scene.GetCanonicalOrder().end());
			for (const UUID id : order)
			{
				const Entity entity = scene.FindEntityByID(id);
				if (!entity.IsValid() || entity.HasComponent<HierarchyDisabledTag>())
					continue;
				if (Physics2DBodyEntry* entry = state.FindOwnedBody(id, PhysicsBodyOrigin::RigidBody);
					entry != nullptr && entry->Plan.MotionType == PhysicsMotionType::Dynamic
					&& (!state.World->IsSleeping(entry->Handle) || entity.GetParent().IsValid()))
				{
					std::tie(entry->WrittenTranslation, entry->WrittenRotation) = Utils::Write2DWorldPose(entity, state.World->GetPose(entry->Handle));
					entry->PlacedPose = Utils::GetCurrentPlanarPose(entity);
				}
			}
		}

		// Map, collapse into pairs, sort, dispatch (PhysicsSystem::PostStep's rules).
		const auto resolve = [&state](BodyHandle handle, uint32_t collider) -> std::optional<Resolved2DBody>
		{
			if (const auto body = state.Bodies.find(handle.GetValue()); body != state.Bodies.end())
			{
				const Physics2DBodyEntry& entry = body->second;
				const UUID entity = collider < entry.Colliders.size() ? entry.Colliders[collider] : entry.Owner;
				return Resolved2DBody{ .Owner = entry.Owner, .Collider = entity };
			}
			return std::nullopt;
		};

		std::map<PhysicsPairKey, Pair2DBefore> before;
		std::map<PhysicsPairKey, First2DContact> firstContacts;
		const auto touch = [&state, &before](const PhysicsPairKey& key) -> PhysicsPair&
		{
			PhysicsPair& pair = state.Pairs[key];
			before.try_emplace(key, Pair2DBefore{ .Contacts = pair.Contacts.size(), .Touched = pair.Touched });
			return pair;
		};
		const auto touchContact = [&state, &touch](const PhysicsSubShapeKey& contact)
		{
			const auto found = state.Contacts.find(contact);
			if (found == state.Contacts.end())
				return;
			static_cast<void>(touch(found->second.Pair));
			for (const PhysicsPairKey& key : found->second.ColliderPairs)
				static_cast<void>(touch(key));
		};
		const auto offerFirstContact = [&firstContacts](const PhysicsPairKey& key, const First2DContact& candidate)
		{
			if (const auto [first, added] = firstContacts.try_emplace(key, candidate); !added && candidate < first->second)
				first->second = candidate;
		};
		std::map<UUID, bool> scriptedColliders;
		const auto isScriptedCollider = [&state, &scriptedColliders](UUID entity)
		{
			const auto [found, added] = scriptedColliders.try_emplace(entity, false);
			if (added)
				found->second = state.IsScriptedGatheredCollider(entity);
			return found->second;
		};
		const auto forEachColliderPair = [&isScriptedCollider](const PhysicsSubShapeContact& contact, const auto& visit)
		{
			for (size_t side = 0; side < 2; ++side)
			{
				const UUID collider = contact.Colliders[side];
				const UUID partner = contact.Owners[1 - side];
				if (!collider.IsValid() || collider == contact.Owners[side] || collider == partner || !isScriptedCollider(collider))
					continue;
				visit(Utils::Make2DColliderPairKey(collider, partner, contact.Pair.Kind));
			}
		};
		const auto offerTouchingContact = [&offerFirstContact, &forEachColliderPair](const PhysicsSubShapeKey& key, const PhysicsSubShapeContact& contact)
		{
			offerFirstContact(contact.Pair, Utils::Make2DFirstContact(key, contact, Utils::Get2DLowSide(contact.Pair, contact)));
			forEachColliderPair(contact, [&offerFirstContact, &key, &contact](const PhysicsPairKey& pair)
			{
				offerFirstContact(pair, Utils::Make2DFirstContact(key, contact, Utils::Get2DLowSide(pair, contact)));
			});
		};
		const auto findContact = [&state](const PhysicsSubShapeKey& key) -> PhysicsSubShapeContact*
		{
			const auto found = state.Contacts.find(key);
			ENGINE_CORE_VERIFY(found != state.Contacts.end(), "Physics2DSystem: an index names a contact that does not exist (indexes and Contacts out of step)");
			return found != state.Contacts.end() ? &found->second : nullptr;
		};
		const auto markTouched = [&state](const PhysicsSubShapeContact& contact)
		{
			state.Pairs[contact.Pair].Touched = true;
			for (const PhysicsPairKey& key : contact.ColliderPairs)
				state.Pairs[key].Touched = true;
		};

		for (const ContactEvent& record : state.World->DrainContactEvents())
		{
			const PhysicsSubShapeKey contact = Utils::Make2DSubShapeKey(record);
			if (record.Kind == ContactEventKind::Removed)
			{
				const auto found = state.Contacts.find(contact);
				if (found == state.Contacts.end())
					continue;
				touchContact(contact);
				const auto pair = state.Pairs.find(found->second.Pair);
				ENGINE_CORE_VERIFY(pair != state.Pairs.end() && pair->second.Contacts.contains(contact),
					"Physics2DSystem: a contact's pair does not hold it (Contacts and Pairs out of step)");
				const BodyHandle a(contact.BodyA);
				const BodyHandle b(contact.BodyB);
				const bool asleep = state.World->IsBodyValid(a) && state.World->IsBodyValid(b) && state.World->IsSleeping(a) && state.World->IsSleeping(b);
				if (asleep)
					found->second.Dormant = true;
				else
					state.EraseContact(contact);
				continue;
			}

			const std::optional<Resolved2DBody> a = resolve(record.BodyA, record.ColliderA);
			const std::optional<Resolved2DBody> b = resolve(record.BodyB, record.ColliderB);
			if (!a.has_value() || !b.has_value() || a->Owner == b->Owner)
				continue;
			const bool recordAIsFirst = record.BodyA.GetValue() <= record.BodyB.GetValue();
			const std::array<UUID, 2> owners = recordAIsFirst ? std::array<UUID, 2>{ a->Owner, b->Owner } : std::array<UUID, 2>{ b->Owner, a->Owner };
			const std::array<UUID, 2> colliders = recordAIsFirst ? std::array<UUID, 2>{ a->Collider, b->Collider }
																 : std::array<UUID, 2>{ b->Collider, a->Collider };
			const glm::vec3 normal = recordAIsFirst ? record.Normal : -record.Normal;
			const auto keep = [&record, &normal](PhysicsSubShapeContact& target)
			{
				target.Point = record.Point;
				target.Normal = normal;
				target.RelativeSpeed = record.RelativeNormalSpeed;
			};

			if (record.Kind == ContactEventKind::Touched)
			{
				const auto found = state.Contacts.find(contact);
				if (found == state.Contacts.end() || found->second.Owners != owners)
					continue;
				touchContact(contact);
				PhysicsSubShapeContact& contactState = found->second;
				state.SetContactSides(contact, contactState, owners, colliders);
				contactState.Touching = true;
				keep(contactState);
				markTouched(contactState);
				offerTouchingContact(contact, contactState);
				continue;
			}

			const bool aIsLow = a->Owner < b->Owner;
			const PhysicsPairKey key{ .Low = aIsLow ? a->Owner : b->Owner,
				.High = aIsLow ? b->Owner : a->Owner,
				.Kind = record.IsSensor ? PhysicsPairKind::Trigger : PhysicsPairKind::Collision };
			if (const auto existing = state.Contacts.find(contact); existing != state.Contacts.end() && existing->second.Pair != key)
			{
				touchContact(contact);
				state.EraseContact(contact);
			}
			static_cast<void>(touch(key));
			state.PairsByOwner[key.Low].insert(key);
			state.PairsByOwner[key.High].insert(key);
			const auto existing = state.Contacts.find(contact);
			const bool isNew = existing == state.Contacts.end();
			if (!isNew)
				touchContact(contact);
			PhysicsSubShapeContact& contactState = isNew ? state.AddContact(contact, key) : existing->second;
			const bool wasTouching = !isNew && contactState.Touching;
			contactState.Dormant = false;
			contactState.Touching = wasTouching || record.Touching;
			state.SetContactSides(contact, contactState, owners, colliders);
			if (!wasTouching || record.Touching)
				keep(contactState);
			if (contactState.Touching)
				markTouched(contactState);
			if (record.Touching)
				offerTouchingContact(contact, contactState);
		}
		// Dormant contacts that a step begun with one of their bodies awake did not report again have really ended.
		const std::vector<PhysicsSubShapeKey> eligible(state.EligibleDormantContacts.begin(), state.EligibleDormantContacts.end());
		state.EligibleDormantContacts.clear();
		for (const PhysicsSubShapeKey& contact : eligible)
		{
			const auto found = state.Contacts.find(contact);
			if (found == state.Contacts.end())
				continue;
			touchContact(contact);
			const auto pair = state.Pairs.find(found->second.Pair);
			ENGINE_CORE_VERIFY(pair != state.Pairs.end() && pair->second.Contacts.contains(contact),
				"Physics2DSystem: a contact's pair does not hold it (Contacts and Pairs out of step)");
			if (found->second.Dormant)
				state.EraseContact(contact);
		}

		// The contacts of bodies whose shape PreStep replaced in place may name other colliders now.
		const std::set<uint32_t> reshaped = std::exchange(state.ReattributedBodies, {});
		for (const uint32_t handle : reshaped)
		{
			const auto contacts = state.ContactsByBody.find(handle);
			if (contacts == state.ContactsByBody.end() || !state.Bodies.contains(handle) || !state.World->IsBodyValid(BodyHandle(handle)))
				continue;
			const std::vector<PhysicsSubShapeKey> keys(contacts->second.begin(), contacts->second.end());
			for (const PhysicsSubShapeKey& contact : keys)
			{
				PhysicsSubShapeContact* contactState = findContact(contact);
				if (contactState == nullptr)
					continue;
				std::array<UUID, 2> colliders = contactState->Colliders;
				const std::array<std::pair<uint32_t, uint32_t>, 2> sides = { { { contact.BodyA, contact.SubShapeA }, { contact.BodyB, contact.SubShapeB } } };
				for (size_t side = 0; side < 2; ++side)
				{
					if (sides[side].first != handle)
						continue;
					const BodyHandle body(handle);
					if (const std::optional<Resolved2DBody> resolved = resolve(body, state.World->GetSubShapeCollider(body, sides[side].second)))
						colliders[side] = resolved->Collider;
				}
				if (colliders == contactState->Colliders)
					continue;
				touchContact(contact);
				state.SetContactSides(contact, *contactState, contactState->Owners, colliders);
			}
		}

		// The collider pairs (M20).
		std::set<UUID> colliders;
		for (const auto& [collider, contacts] : state.GatheredContacts)
			colliders.insert(collider);
		for (const auto& [collider, pairs] : state.ColliderPairsByCollider)
			colliders.insert(collider);
		for (const UUID collider : colliders)
		{
			if (!isScriptedCollider(collider))
			{
				const auto owned = state.ColliderPairsByCollider.find(collider);
				if (owned == state.ColliderPairsByCollider.end())
					continue;
				const bool leaving = state.IsPresent(collider);
				const std::vector<PhysicsPairKey> keys(owned->second.begin(), owned->second.end());
				for (const PhysicsPairKey& key : keys)
				{
					PhysicsPair& pair = touch(key);
					const std::vector<PhysicsSubShapeKey> contacts(pair.Contacts.begin(), pair.Contacts.end());
					for (const PhysicsSubShapeKey& contact : contacts)
					{
						if (PhysicsSubShapeContact* contactState = findContact(contact))
							state.UnlinkColliderPair(contact, *contactState, key);
					}
					if (leaving && pair.Active)
						pair.Unconfirmed = true;
				}
				continue;
			}
			const auto gathered = state.GatheredContacts.find(collider);
			if (gathered == state.GatheredContacts.end())
				continue;
			const std::vector<PhysicsSubShapeKey> contacts(gathered->second.begin(), gathered->second.end());
			for (const PhysicsSubShapeKey& contact : contacts)
			{
				PhysicsSubShapeContact* contactState = findContact(contact);
				const std::optional<size_t> side = contactState != nullptr ? Utils::Find2DGatheredSide(*contactState, collider) : std::nullopt;
				const UUID partner = side.has_value() ? contactState->Owners[1 - *side] : UUID();
				if (!side.has_value() || partner == collider)
					continue;
				const PhysicsPairKey key = Utils::Make2DColliderPairKey(collider, partner, contactState->Pair.Kind);
				if (std::binary_search(contactState->ColliderPairs.begin(), contactState->ColliderPairs.end(), key))
					continue;
				PhysicsPair& pair = touch(key);
				pair.ColliderBody = contactState->Owners[*side];
				state.LinkColliderPair(contact, *contactState, key);
				if (contactState->Touching)
					pair.Touched = true;
			}
		}

		// Transitions over the step.
		std::vector<Pending2DPairEvent> events;
		std::vector<PhysicsPairKey> finished;
		for (auto& [key, pair] : state.Pairs)
		{
			const size_t after = pair.Contacts.size();
			const auto counted = before.find(key);
			const Pair2DBefore previous = counted != before.end() ? counted->second : Pair2DBefore{ .Contacts = after, .Touched = pair.Touched };
			if (after > 0)
			{
				pair.Unconfirmed = false;
				if (!pair.Active && pair.Touched && !previous.Touched)
				{
					Pending2DPairEvent event;
					event.Key = key;
					event.Type = Utils::Get2DEnterType(key.Kind);
					event.ColliderBody = pair.ColliderBody;
					auto first = firstContacts.find(key);
					if (first == firstContacts.end())
					{
						for (const PhysicsSubShapeKey& contact : pair.Contacts)
						{
							const PhysicsSubShapeContact* contactState = findContact(contact);
							if (contactState != nullptr && contactState->Touching)
								offerFirstContact(key, Utils::Make2DFirstContact(contact, *contactState, Utils::Get2DLowSide(key, *contactState)));
						}
						first = firstContacts.find(key);
					}
					if (first != firstContacts.end())
					{
						event.LowCollider = first->second.LowCollider;
						event.HighCollider = first->second.HighCollider;
						if (key.Kind == PhysicsPairKind::Collision)
						{
							event.Point = first->second.Point;
							event.Normal = first->second.Normal;
							event.RelativeSpeed = first->second.RelativeSpeed;
						}
					}
					events.push_back(event);
				}
				continue;
			}
			if (!pair.Active)
			{
				finished.push_back(key);
				continue;
			}
			Pending2DPairEvent event;
			event.Key = key;
			event.Type = Utils::Get2DExitType(key.Kind);
			event.Synthesized = pair.Unconfirmed || previous.Contacts == 0;
			event.ColliderBody = pair.ColliderBody;
			events.push_back(event);
		}
		for (const PhysicsPairKey& key : finished)
			state.ErasePair(key);

		// The canonical order (PhysicsEventOrderKey).
		std::sort(events.begin(), events.end(), [](const Pending2DPairEvent& a, const Pending2DPairEvent& b)
		{
			return Utils::Get2DPairOrderKey(a.Key, a.Type, a.ColliderBody) < Utils::Get2DPairOrderKey(b.Key, b.Type, b.ColliderBody);
		});

		// Dispatch, with IPhysicsEventListener's drop rules.
		std::vector<bool> blocked(events.size());
		for (size_t index = 0; index < events.size(); ++index)
			blocked[index] = !state.IsPresent(events[index].Key.Low) || !state.IsPresent(events[index].Key.High);
		const auto deliver = [&state, &step](PhysicsEventType type, UUID self, UUID other, UUID selfBody, const PhysicsContact& contact, bool synthesized)
		{
			const PhysicsEvent event{ .Type = type, .Self = self, .Other = other, .Contact = contact, .Synthesized = synthesized, .Tick = step.Tick, .SelfBody = selfBody };
			state.LastEvents.push_back(event);
			if (state.Listener != nullptr)
				state.Listener->OnPhysics2DEvent(event);
		};
		state.Dispatching = true;
		for (size_t index = 0; index < events.size(); ++index)
		{
			const Pending2DPairEvent& event = events[index];
			const auto found = state.Pairs.find(event.Key);
			if (found == state.Pairs.end() || blocked[index])
				continue;
			PhysicsPair& pair = found->second;
			const UUID low = event.Key.Low;
			const UUID high = event.Key.High;
			const bool isColliderPair = event.Key.IsColliderPair();
			const bool lowReceives = !isColliderPair || event.Key.Collider == low;
			const bool highReceives = !isColliderPair || event.Key.Collider == high;
			const UUID lowBody = isColliderPair && lowReceives ? pair.ColliderBody : low;
			const UUID highBody = isColliderPair && highReceives ? pair.ColliderBody : high;
			if (Utils::Is2DEnter(event.Type))
			{
				pair.Active = true;
				pair.SinceTick = step.Tick;
				pair.LowCollider = event.LowCollider;
				pair.HighCollider = event.HighCollider;
				const PhysicsContact lowContact{ .Point = event.Point,
					.Normal = event.Normal,
					.RelativeSpeed = event.RelativeSpeed,
					.Collider = event.LowCollider,
					.OtherCollider = event.HighCollider };
				const PhysicsContact highContact{ .Point = event.Point,
					.Normal = event.Key.Kind == PhysicsPairKind::Collision ? -event.Normal : event.Normal,
					.RelativeSpeed = event.RelativeSpeed,
					.Collider = event.HighCollider,
					.OtherCollider = event.LowCollider };
				if (lowReceives && state.IsPresent(low))
				{
					pair.EnteredLow = true;
					deliver(event.Type, low, high, lowBody, lowContact, false);
				}
				if (highReceives && state.IsPresent(high))
				{
					pair.EnteredHigh = true;
					deliver(event.Type, high, low, highBody, highContact, false);
				}
				continue;
			}

			const bool toLow = pair.EnteredLow;
			const bool toHigh = pair.EnteredHigh;
			const PhysicsContact lowContact{ .Collider = pair.LowCollider, .OtherCollider = pair.HighCollider };
			const PhysicsContact highContact{ .Collider = pair.HighCollider, .OtherCollider = pair.LowCollider };
			state.ErasePair(event.Key);
			if (toLow && state.IsPresent(low))
				deliver(event.Type, low, high, lowBody, lowContact, event.Synthesized);
			if (toHigh && state.IsPresent(high))
				deliver(event.Type, high, low, highBody, highContact, event.Synthesized);
		}
		state.Dispatching = false;
	}

	void Physics2DSystem::FlushDestroyed(uint64_t tick)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Dispatching, "Physics2DSystem::FlushDestroyed was called from a physics listener callback");
		std::set<UUID> handled;
		while (true)
		{
			std::vector<UUID> gone;
			const std::set<UUID> candidates = std::move(state.FlushCandidates);
			state.FlushCandidates.clear();
			for (const UUID candidate : candidates)
			{
				if (handled.contains(candidate) || state.IsPresent(candidate))
					continue;
				handled.insert(candidate);
				const bool ownsBody = std::ranges::any_of(Utils::Body2DOrigins, [&state, candidate](PhysicsBodyOrigin origin)
				{
					return state.FindOwnedBody(candidate, origin) != nullptr;
				});
				if (ownsBody || state.PairsByOwner.contains(candidate))
					gone.push_back(candidate);
			}
			if (gone.empty())
				break;

			std::set<PhysicsPairKey> closing;
			std::set<UUID> woken;
			for (const UUID owner : gone)
			{
				if (const auto owned = state.PairsByOwner.find(owner); owned != state.PairsByOwner.end())
					closing.insert(owned->second.begin(), owned->second.end());
				state.CollectPartners(owner, woken);
			}
			std::vector<Closing2DPair> exits;
			for (const PhysicsPairKey& key : closing)
			{
				const auto found = state.Pairs.find(key);
				ENGINE_CORE_VERIFY(found != state.Pairs.end(), "Physics2DSystem: an owner's pair does not exist (PairsByOwner and Pairs out of step)");
				if (found != state.Pairs.end() && found->second.Active)
				{
					const PhysicsPair& pair = found->second;
					exits.push_back(Closing2DPair{ .Key = key,
						.ToLow = pair.EnteredLow,
						.ToHigh = pair.EnteredHigh,
						.LowCollider = pair.LowCollider,
						.HighCollider = pair.HighCollider,
						.ColliderBody = pair.ColliderBody });
				}
				state.ErasePair(key);
			}
			std::sort(exits.begin(), exits.end(), [](const Closing2DPair& a, const Closing2DPair& b)
			{
				return Utils::Get2DPairOrderKey(a.Key, Utils::Get2DExitType(a.Key.Kind), a.ColliderBody)
					< Utils::Get2DPairOrderKey(b.Key, Utils::Get2DExitType(b.Key.Kind), b.ColliderBody);
			});
			const auto deliver = [&state, tick](PhysicsEventType type, UUID self, UUID other, UUID selfBody, UUID collider, UUID otherCollider)
			{
				const PhysicsEvent event{ .Type = type,
					.Self = self,
					.Other = other,
					.Contact = PhysicsContact{ .Collider = collider, .OtherCollider = otherCollider },
					.Synthesized = true,
					.Tick = tick,
					.SelfBody = selfBody };
				state.LastEvents.push_back(event);
				if (state.Listener != nullptr)
					state.Listener->OnPhysics2DEvent(event);
			};
			state.Dispatching = true;
			for (const Closing2DPair& ending : exits)
			{
				const PhysicsEventType type = Utils::Get2DExitType(ending.Key.Kind);
				const bool isColliderPair = ending.Key.IsColliderPair();
				const UUID lowBody = isColliderPair && ending.Key.Collider == ending.Key.Low ? ending.ColliderBody : ending.Key.Low;
				const UUID highBody = isColliderPair && ending.Key.Collider == ending.Key.High ? ending.ColliderBody : ending.Key.High;
				if (ending.ToLow && state.IsPresent(ending.Key.Low))
					deliver(type, ending.Key.Low, ending.Key.High, lowBody, ending.LowCollider, ending.HighCollider);
				if (ending.ToHigh && state.IsPresent(ending.Key.High))
					deliver(type, ending.Key.High, ending.Key.Low, highBody, ending.HighCollider, ending.LowCollider);
			}
			state.Dispatching = false;

			for (const UUID owner : gone)
			{
				for (const PhysicsBodyOrigin origin : Utils::Body2DOrigins)
				{
					if (const Physics2DBodyEntry* entry = state.FindOwnedBody(owner, origin))
						state.RemoveBody(entry->Handle.GetValue(), woken);
				}
			}
			for (const UUID owner : gone)
				woken.erase(owner);
			state.WakeOwners(woken);
		}
	}

	void Physics2DSystem::SetEventListener(IPhysics2DEventListener* listener)
	{
		ENGINE_CORE_ASSERT(!m_State->Dispatching, "Physics2DSystem::SetEventListener was called from a physics listener callback");
		m_State->Listener = listener;
	}

	std::span<const PhysicsEvent> Physics2DSystem::GetLastEvents() const
	{
		return m_State->LastEvents;
	}

	// --- Bodies -------------------------------------------------------------------------------------------------------

	Status Physics2DSystem::AddForce(UUID entity, const glm::vec2& force)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::Check2DVector(force, MaxVector2DMagnitude, "force"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::Make2DMotionTypeError(entity, entry->Plan.MotionType, "AddForce", "Dynamic");
		m_State->World->AddForce(entry->Handle, force);
		return {};
	}

	Status Physics2DSystem::AddForceAtPosition(UUID entity, const glm::vec2& force, const glm::vec2& position)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::Check2DVector(force, MaxVector2DMagnitude, "force"));
		ENGINE_TRY(Utils::Check2DVector(position, MaxPhysicsCoordinate, "position"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::Make2DMotionTypeError(entity, entry->Plan.MotionType, "AddForceAtPosition", "Dynamic");
		m_State->World->AddForceAtPosition(entry->Handle, force, position);
		return {};
	}

	Status Physics2DSystem::AddTorque(UUID entity, float torque)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::Check2DScalar(torque, MaxVector2DMagnitude, "torque"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::Make2DMotionTypeError(entity, entry->Plan.MotionType, "AddTorque", "Dynamic");
		m_State->World->AddTorque(entry->Handle, torque);
		return {};
	}

	Status Physics2DSystem::AddImpulse(UUID entity, const glm::vec2& impulse)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::Check2DVector(impulse, MaxVector2DMagnitude, "impulse"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::Make2DMotionTypeError(entity, entry->Plan.MotionType, "AddImpulse", "Dynamic");
		m_State->World->AddImpulse(entry->Handle, impulse);
		return {};
	}

	Status Physics2DSystem::AddAngularImpulse(UUID entity, float angularImpulse)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::Check2DScalar(angularImpulse, MaxVector2DMagnitude, "angular impulse"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::Make2DMotionTypeError(entity, entry->Plan.MotionType, "AddAngularImpulse", "Dynamic");
		m_State->World->AddAngularImpulse(entry->Handle, angularImpulse);
		return {};
	}

	Result<glm::vec2> Physics2DSystem::GetLinearVelocity(UUID entity) const
	{
		ENGINE_TRY_ASSIGN(const Physics2DBodyEntry* entry, m_State->FindRigidBody(entity));
		return m_State->World->GetLinearVelocity(entry->Handle);
	}

	Status Physics2DSystem::SetLinearVelocity(UUID entity, const glm::vec2& velocity)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::Check2DVector(velocity, MaxVector2DMagnitude, "velocity"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::Make2DVelocityError(entity, entry->Plan.MotionType, "SetLinearVelocity");
		m_State->World->SetLinearVelocity(entry->Handle, velocity);
		return {};
	}

	Result<float> Physics2DSystem::GetAngularVelocity(UUID entity) const
	{
		ENGINE_TRY_ASSIGN(const Physics2DBodyEntry* entry, m_State->FindRigidBody(entity));
		return m_State->World->GetAngularVelocity(entry->Handle);
	}

	Status Physics2DSystem::SetAngularVelocity(UUID entity, float velocity)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::Check2DScalar(velocity, MaxVector2DMagnitude, "angular velocity"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::Make2DVelocityError(entity, entry->Plan.MotionType, "SetAngularVelocity");
		m_State->World->SetAngularVelocity(entry->Handle, velocity);
		return {};
	}

	Status Physics2DSystem::MoveKinematic(UUID entity, const glm::vec2& position, float angle)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::Check2DVector(position, MaxPhysicsCoordinate, "position"));
		ENGINE_TRY(Utils::Check2DScalar(angle, std::numeric_limits<float>::max(), "angle"));
		if (entry->Plan.MotionType != PhysicsMotionType::Kinematic)
			return Utils::Make2DMotionTypeError(entity, entry->Plan.MotionType, "MoveKinematic", "Kinematic");
		// The entity's Transform is the single source of a kinematic body's pose: the next PreStep sweeps the body there.
		const Physics2DPose target{ .Position = position, .Angle = Utils::RadiansFromDegrees(angle) };
		static_cast<void>(Utils::Write2DWorldPose(m_State->Specification.RuntimeScene->FindEntityByID(entity), target));
		return {};
	}

	Status Physics2DSystem::Teleport(UUID entity, const glm::vec2& position, std::optional<float> angle)
	{
		State& state = *m_State;
		Scene& scene = *state.Specification.RuntimeScene;
		const bool ownsBody = std::ranges::any_of(Utils::Body2DOrigins, [&state, entity](PhysicsBodyOrigin origin)
		{
			return state.FindOwnedBody(entity, origin) != nullptr;
		});
		if (!ownsBody || !state.IsPresent(entity))
			return Utils::MakeNo2DBodyError(entity);
		ENGINE_TRY(Utils::Check2DVector(position, MaxPhysicsCoordinate, "position"));
		if (angle.has_value())
			ENGINE_TRY(Utils::Check2DScalar(*angle, std::numeric_limits<float>::max(), "angle"));
		const Entity target = scene.FindEntityByID(entity);

		// The pose the Transform write gives, through the parent and in float, may round past the range: it is checked before
		// anything is applied, and the Transform is put back when it fails.
		TransformComponent& transform = target.GetComponent<TransformComponent>();
		const glm::vec3 previousTranslation = transform.Translation;
		const glm::quat previousRotation = transform.Rotation;
		if (angle.has_value())
		{
			static_cast<void>(Utils::Write2DWorldPose(target, Physics2DPose{ .Position = position, .Angle = Utils::RadiansFromDegrees(*angle) }));
		}
		else
		{
			// The world rotation is kept as it is (its swing too); only the world X and Y move.
			const Entity parent = target.GetParent();
			if (parent.IsValid())
			{
				const glm::mat4 parentWorld = TransformSystem::ComputeWorldMatrix(parent);
				const float worldZ = (parentWorld * glm::vec4(transform.Translation, 1.0f)).z;
				transform.Translation = glm::vec3(glm::affineInverse(parentWorld) * glm::vec4(position, worldZ, 1.0f));
			}
			else
			{
				transform.Translation = glm::vec3(position, transform.Translation.z);
			}
		}
		const glm::mat4 world = TransformSystem::ComputeWorldMatrix(target);
		const Physics2DPose placed = Utils::GetPlanarPose(target, world);
		if (!Utils::IsPlaceable2DPose(placed))
		{
			Utils::Restore2DLocalPose(target, previousTranslation, previousRotation);
			return MakeError(ErrorCode::InvalidArgument,
				"the position ({}, {}) is out of range under the entity's parent: each coordinate of the world position must be at most {} m", position.x,
				position.y, MaxPhysicsCoordinate);
		}

		// The written values, copied before the structural change below (a component reference does not survive one).
		const glm::vec3 writtenTranslation = transform.Translation;
		const glm::quat writtenRotation = transform.Rotation;
		scene.GetRegistry().emplace_or_replace<InterpolationResetTag>(target.GetHandle());
		std::set<UUID> partners;
		state.CollectPartners(entity, partners);
		for (const PhysicsBodyOrigin origin : Utils::Body2DOrigins)
		{
			Physics2DBodyEntry* entry = state.FindOwnedBody(entity, origin);
			if (entry == nullptr)
				continue;
			state.World->SetPose(entry->Handle, placed, true);
			entry->WrittenTranslation = writtenTranslation;
			entry->WrittenRotation = writtenRotation;
			entry->PlacedMatrix = world;
			entry->PlacedPose = placed;
		}
		state.WakeOwners(partners);
		return {};
	}

	Result<bool> Physics2DSystem::IsSleeping(UUID entity) const
	{
		ENGINE_TRY_ASSIGN(const Physics2DBodyEntry* entry, m_State->FindRigidBody(entity));
		return m_State->World->IsSleeping(entry->Handle);
	}

	Status Physics2DSystem::WakeUp(UUID entity)
	{
		ENGINE_TRY_ASSIGN(Physics2DBodyEntry * entry, m_State->FindRigidBody(entity));
		if (entry->Plan.MotionType != PhysicsMotionType::Static)
			m_State->World->WakeUp(entry->Handle);
		return {};
	}

	Result<PhysicsMotionType> Physics2DSystem::GetMotionType(UUID entity) const
	{
		ENGINE_TRY_ASSIGN(const Physics2DBodyEntry* entry, m_State->FindRigidBody(entity));
		return entry->Plan.MotionType;
	}

	// --- Queries ------------------------------------------------------------------------------------------------------

	namespace Utils {

		// The shortest direction a query normalizes, and the smallest and largest radius or half extent of a query shape, as
		// the 3D system's (PhysicsSystemQueries.cpp).
		constexpr float Min2DDirectionLength = 1.0e-6f;
		constexpr float Min2DQueryExtent = PhysicsShape::MinColliderSize / 2.0f;
		constexpr float Max2DQueryExtent = PhysicsShape::MaxColliderSize / 2.0f;

		[[nodiscard]] static Status Check2DPoint(const glm::vec2& value, std::string_view name)
		{
			if (!std::isfinite(value.x) || !std::isfinite(value.y))
				return MakeError(ErrorCode::InvalidArgument, "the query's {} ({}, {}) is not finite", name, value.x, value.y);
			if (std::abs(value.x) > MaxPhysicsCoordinate || std::abs(value.y) > MaxPhysicsCoordinate)
			{
				return MakeError(ErrorCode::InvalidArgument, "the query's {} ({}, {}) is out of range: each coordinate must be at most {} m", name, value.x,
					value.y, MaxPhysicsCoordinate);
			}
			return {};
		}

		[[nodiscard]] static Status Check2DExtent(float value, std::string_view name)
		{
			if (!std::isfinite(value) || !(value >= Min2DQueryExtent) || value > Max2DQueryExtent)
				return MakeError(ErrorCode::InvalidArgument, "the query's {} {} is not from {} to {} m", name, value, Min2DQueryExtent, Max2DQueryExtent);
			return {};
		}

		// `direction` scaled to unit length, by its largest component first, so a long but finite vector never overflows.
		[[nodiscard]] static Result<glm::vec2> Normalize2DDirection(const glm::vec2& direction)
		{
			if (!std::isfinite(direction.x) || !std::isfinite(direction.y))
				return MakeError(ErrorCode::InvalidArgument, "the query's direction ({}, {}) is not finite", direction.x, direction.y);
			const float largest = std::max(std::abs(direction.x), std::abs(direction.y));
			const glm::vec2 scaled = largest > 0.0f ? direction / largest : glm::vec2(0.0f);
			const float length = glm::length(scaled);
			if (!(largest * length >= Min2DDirectionLength))
				return MakeError(ErrorCode::InvalidArgument, "the query's direction ({}, {}) is shorter than {} and has no direction", direction.x, direction.y,
					Min2DDirectionLength);
			return scaled / length;
		}

		[[nodiscard]] static bool IsBefore2D(const Physics2DRaycastHit& a, const Physics2DRaycastHit& b)
		{
			return std::tie(a.Distance, a.Entity, a.Body, a.Point.x, a.Point.y, a.Normal.x, a.Normal.y)
				< std::tie(b.Distance, b.Entity, b.Body, b.Point.x, b.Point.y, b.Normal.x, b.Normal.y);
		}

		[[nodiscard]] static bool IsBefore2D(const PhysicsOverlapHit& a, const PhysicsOverlapHit& b)
		{
			return std::tie(a.Entity, a.Body) < std::tie(b.Entity, b.Body);
		}

	}

	Result<std::optional<Physics2DRaycastHit>> Physics2DSystem::Raycast(const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
		PhysicsLayerMask layers) const
	{
		ENGINE_TRY_ASSIGN(std::vector<Physics2DRaycastHit> hits, RaycastAll(origin, direction, maxDistance, layers));
		if (hits.empty())
			return std::optional<Physics2DRaycastHit>();
		return std::optional<Physics2DRaycastHit>(hits.front());
	}

	Result<std::vector<Physics2DRaycastHit>> Physics2DSystem::RaycastAll(const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
		PhysicsLayerMask layers) const
	{
		ENGINE_TRY(Utils::Check2DPoint(origin, "origin"));
		ENGINE_TRY_ASSIGN(const glm::vec2 unit, Utils::Normalize2DDirection(direction));
		if (!std::isfinite(maxDistance) || !(maxDistance > 0.0f))
			return MakeError(ErrorCode::InvalidArgument, "the query's distance {} is not a finite value above 0", maxDistance);
		ENGINE_TRY(Utils::Check2DPoint(origin + unit * maxDistance, "end point"));
		std::vector<Physics2DRaycastHit> hits;
		if (m_State->World == nullptr)
			return hits;
		for (const PhysicsQueryHit& hit : m_State->World->RaycastAll(origin, unit, maxDistance, PhysicsQueryFilter{ .Layers = layers }))
		{
			if (const std::optional<PhysicsOverlapHit> owner = m_State->ResolveHit(hit.Body, hit.Collider))
			{
				hits.push_back(Physics2DRaycastHit{ .Entity = owner->Entity,
					.Body = owner->Body,
					.Point = glm::vec2(hit.Point.x, hit.Point.y),
					.Normal = glm::vec2(hit.Normal.x, hit.Normal.y),
					.Distance = hit.Distance });
			}
		}
		std::sort(hits.begin(), hits.end(), [](const Physics2DRaycastHit& a, const Physics2DRaycastHit& b)
		{
			return Utils::IsBefore2D(a, b);
		});
		return hits;
	}

	Result<std::vector<PhysicsOverlapHit>> Physics2DSystem::OverlapCircle(const glm::vec2& center, float radius, PhysicsLayerMask layers) const
	{
		ENGINE_TRY(Utils::Check2DPoint(center, "centre"));
		ENGINE_TRY(Utils::Check2DExtent(radius, "radius"));
		std::vector<PhysicsOverlapHit> hits;
		if (m_State->World == nullptr)
			return hits;
		for (const PhysicsOverlap& overlap :
			m_State->World->Overlap(CircleShape2D{ .Radius = radius }, Physics2DPose{ .Position = center }, PhysicsQueryFilter{ .Layers = layers }))
		{
			if (const std::optional<PhysicsOverlapHit> owner = m_State->ResolveHit(overlap.Body, overlap.Collider))
				hits.push_back(*owner);
		}
		std::sort(hits.begin(), hits.end(), [](const PhysicsOverlapHit& a, const PhysicsOverlapHit& b)
		{
			return Utils::IsBefore2D(a, b);
		});
		hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
		return hits;
	}

	Result<std::vector<PhysicsOverlapHit>> Physics2DSystem::OverlapBox(const glm::vec2& center, const glm::vec2& halfExtents, float angle,
		PhysicsLayerMask layers) const
	{
		ENGINE_TRY(Utils::Check2DPoint(center, "centre"));
		ENGINE_TRY(Utils::Check2DExtent(halfExtents.x, "half extent x"));
		ENGINE_TRY(Utils::Check2DExtent(halfExtents.y, "half extent y"));
		if (!std::isfinite(angle))
			return MakeError(ErrorCode::InvalidArgument, "the query's angle {} is not finite", angle);
		std::vector<PhysicsOverlapHit> hits;
		if (m_State->World == nullptr)
			return hits;
		const Physics2DPose pose{ .Position = center, .Angle = Utils::RadiansFromDegrees(angle) };
		for (const PhysicsOverlap& overlap : m_State->World->Overlap(BoxShape2D{ .HalfExtents = halfExtents }, pose, PhysicsQueryFilter{ .Layers = layers }))
		{
			if (const std::optional<PhysicsOverlapHit> owner = m_State->ResolveHit(overlap.Body, overlap.Collider))
				hits.push_back(*owner);
		}
		std::sort(hits.begin(), hits.end(), [](const PhysicsOverlapHit& a, const PhysicsOverlapHit& b)
		{
			return Utils::IsBefore2D(a, b);
		});
		hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
		return hits;
	}

	std::optional<Aabb> Physics2DSystem::GetBodyBounds(UUID entity) const
	{
		if (m_State->World == nullptr || !m_State->IsPresent(entity))
			return std::nullopt;
		const BodyHandle body = m_State->FindOwnedBodyHandle(entity);
		if (!body.IsValid())
			return std::nullopt;
		return m_State->World->GetBodyBounds(body);
	}

	std::optional<Aabb> Physics2DSystem::GetColliderBounds(UUID entity) const
	{
		if (m_State->World == nullptr || !entity.IsValid() || !m_State->IsPresent(entity))
			return std::nullopt;
		// An entity's 2D colliders may sit in two bodies: its solid ones in an ancestor's compound, its triggers in its own
		// implicit sensor body. Every one counts.
		Aabb bounds;
		for (const auto& [value, entry] : m_State->Bodies)
		{
			if (!m_State->IsPresent(entry.Owner))
				continue;
			for (size_t index = 0; index < entry.Colliders.size(); ++index)
			{
				if (entry.Colliders[index] != entity)
					continue;
				if (const std::optional<Aabb> collider = m_State->World->GetSubShapeBounds(entry.Handle, static_cast<uint32_t>(index)))
					bounds.Extend(*collider);
			}
		}
		if (bounds.IsEmpty())
			return std::nullopt;
		return bounds;
	}

	const PhysicsLayerTable& Physics2DSystem::GetLayers() const
	{
		return m_State->Layers;
	}

	glm::vec2 Physics2DSystem::GetGravity() const
	{
		return m_State->World != nullptr ? m_State->World->GetGravity() : m_State->Gravity;
	}

	Status Physics2DSystem::SetGravity(const glm::vec2& gravity)
	{
		ENGINE_TRY(Utils::Check2DVector(gravity, MaxPhysicsGravity, "gravity"));
		if (m_State->World != nullptr)
			ENGINE_TRY(m_State->World->SetGravity(gravity));
		m_State->Gravity = gravity;
		return {};
	}

	// --- Observability ------------------------------------------------------------------------------------------------

	std::span<const PhysicsDiagnostic> Physics2DSystem::GetDiagnostics() const
	{
		return m_State->Diagnostics;
	}

	Physics2DSystemStats Physics2DSystem::GetStats() const
	{
		const State& state = *m_State;
		Physics2DSystemStats stats;
		stats.Compositions = state.Compositions;
		if (state.World == nullptr)
			return stats;
		const PhysicsWorldStats world = state.World->GetStats();
		stats.BodyCount = world.BodyCount;
		stats.ActiveBodyCount = world.ActiveBodyCount;
		stats.ContactPairCount = static_cast<uint32_t>(std::ranges::count_if(state.Pairs, [](const auto& pair)
		{
			return pair.second.Active && !pair.first.IsColliderPair();
		}));
		return stats;
	}

	std::optional<Physics2DBodyReport> Physics2DSystem::GetBodyInfo(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	void Physics2DSystem::AppendStateHash(XXH64Hasher& hasher) const
	{
		const State& state = *m_State;
		if (state.World == nullptr || state.Bodies.empty())
			return;

		const Scene& scene = *state.Specification.RuntimeScene;
		std::vector<std::byte> bytes;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			for (const PhysicsBodyOrigin origin : Utils::Body2DOrigins)
			{
				const Physics2DBodyEntry* entry = state.FindOwnedBody(id, origin);
				if (entry == nullptr)
					continue;
				const uint64_t value = id.GetValue();
				for (uint32_t shift = 0; shift < 64; shift += 8)
					bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
				const Physics2DPose pose = state.World->GetPose(entry->Handle);
				const glm::vec2 linear = state.World->GetLinearVelocity(entry->Handle);
				Utils::Append2DFloat(bytes, pose.Position.x);
				Utils::Append2DFloat(bytes, pose.Position.y);
				Utils::Append2DFloat(bytes, pose.Angle);
				Utils::Append2DFloat(bytes, linear.x);
				Utils::Append2DFloat(bytes, linear.y);
				Utils::Append2DFloat(bytes, state.World->GetAngularVelocity(entry->Handle));
				bytes.push_back(static_cast<std::byte>(state.World->IsSleeping(entry->Handle) ? 1 : 0));
			}
		}
		hasher.Update(bytes);
	}

}
