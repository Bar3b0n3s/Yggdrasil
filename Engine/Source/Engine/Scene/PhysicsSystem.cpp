#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <format>
#include <tuple>
#include <utility>
#include <variant>

// The play session's physics (PhysicsSystem.h). How the parts fit:
//   - Rebuilds. EnTT signals on the physics components, MeshRenderer and DisabledTag, and a comparison of the hierarchy
//     whenever the scene's revision moved, tell PreStep that the composition may have changed: it composes again
//     (ComposePhysicsBodies) and diffs the plans against the created bodies by (owner, origin). Everything a body's shape
//     was built from that changes without a signal (local transforms, written directly by runtime systems and scripts, and
//     mesh versions) is compared bit for bit every PreStep (PhysicsShapeInput, PhysicsMeshInput), and the entities it was
//     built from are compared again when the hierarchy changed. A body is described again only when one of these changed,
//     so a moving body whose shape inputs stay the same is never touched.
//   - Pairs. Records are keyed by sub-shape contact (PhysicsSubShapeKey) and grouped into pairs of owners
//     (PhysicsPairKey). A pair's reference count is its number of contacts; enters and exits are its transitions from and
//     to zero over a step. Jolt reports OnContactRemoved when a pair of bodies falls asleep, although they still touch;
//     such a removal (both bodies asleep after the step, no character listener involved) leaves the contact dormant
//     instead: it keeps counting, a later OnContactAdded for it changes nothing, and it is dropped (possibly ending its
//     pair) only after a step that began with one of its bodies awake and did not report it again. So sleeping never ends
//     a pair, and a sleeping body that is moved away still gets its exit.
//   - Touching (M20, Docs/Decisions/0031-m20-contract.md decision 3). A contact remembers whether it touched (its Added
//     record's Touching, or a Touched record since); a pair remembers whether one of its contacts touched since it began
//     (PhysicsPair::Touched), and an inactive pair enters at the PostStep that sets it. A pair whose enter was dropped has
//     set it already, so it never enters again; the flush that follows removes it.
//   - Collider pairs (M20, decision 2). A contact also counts in a collider pair for each side whose collider is a scripted
//     gathered collider. PostStep keeps them in step after the records: it resolves again the colliders of the contacts of
//     bodies whose shape PreStep replaced in place (Jolt keeps a persisting contact's sub-shape IDs, whose collider may have
//     changed), links each scripted gathered collider's contacts to its collider pairs, and unlinks every contact of a
//     collider that is not one any more (an active pair of a collider still present then ends with a synthesized exit, as an
//     unconfirmed pair does). Collider pairs never wake bodies and never count in the stats or physics.bodyInfo, so they
//     cannot change the simulation or what body pairs report. They see the sub-shape contacts Jolt reports: its manifold
//     reduction merges a body pair's contacts whose normals agree into the manifold of the first sub-shape pair it found,
//     so of two coplanar pieces under one body, only that one counts the merged contact.
//   - Callbacks to the listener run with Dispatching set; the step hooks assert it is clear. Diagnostics raised inside a
//     hook are logged at once and heard by the listener when the hook's own work is done, so a listener that changes the
//     scene never runs in the middle of a rebuild.
//   - Range. Every pose the system hands to the world or a character passes IsPlaceablePhysicsPose first. A Transform
//     written beyond it is not applied: PreStep writes the body's own pose back into it (the values PostStep last wrote,
//     or the pose the body was placed at), and a Kinematic body stops there instead of keeping its last velocity.
//   - The cross-indexes (Pairs, Contacts, ContactsByBody, PairsByOwner, GatheredContacts, ColliderPairsByCollider,
//     BodiesByOwner, GatheredColliders) are kept in step by the functions that change them; a lookup that finds one out of
//     step is verified (ENGINE_CORE_VERIFY, also in Dist), since going on would corrupt the pair state.

namespace Engine {

	namespace {

		// The largest force, impulse, torque, velocity or gravity component that the body functions accept (PhysicsSystem.h
		// "out-of-range value"; positions: MaxPhysicsCoordinate): far beyond any game, and far enough below the float range
		// that Jolt's products of such values stay finite (PhysicsWorld bounds what forces add to a velocity in one step).
		constexpr float MaxVectorMagnitude = 1.0e12f;

		// One pair event before dispatch (§9.4 steps 2 to 4).
		struct PendingPairEvent
		{
			PhysicsPairKey Key{};
			PhysicsEventType Type = PhysicsEventType::CollisionEnter;
			bool Synthesized = false;
			// CollisionEnter: the first contact, its normal pointing from the pair's Low owner towards its High owner.
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
			float RelativeSpeed = 0.0f;
			// Enter events: the collider entities of the first sub-shape pair.
			UUID LowCollider{};
			UUID HighCollider{};
			// A collider pair's: its collider's body owner (PhysicsEvent::SelfBody).
			UUID ColliderBody{};
		};

		// A pair as PostStep first found it, before the step's records changed it.
		struct PairBefore
		{
			size_t Contacts = 0;
			bool Touched = false;
		};

		// The first contact of a pair entering this step, oriented from the pair's Low side to its High one (a collider pair's
		// sides are its collider's and its partner's). The step's records arrive in a different order every run, so "first" is
		// the smallest by a total order on the record's content.
		struct FirstContact
		{
			UUID LowCollider{};
			UUID HighCollider{};
			uint32_t LowSubShape = 0;
			uint32_t HighSubShape = 0;
			std::array<uint32_t, 7> Bits{}; // point, normal and speed as bit patterns
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
			float RelativeSpeed = 0.0f;

			[[nodiscard]] bool operator<(const FirstContact& other) const
			{
				return std::tie(LowCollider, HighCollider, LowSubShape, HighSubShape, Bits)
					< std::tie(other.LowCollider, other.HighCollider, other.LowSubShape, other.HighSubShape, other.Bits);
			}
		};

		// A pair FlushDestroyed closes: who gets a synthesized exit, and the colliders it names.
		struct ClosingPair
		{
			PhysicsPairKey Key{};
			bool ToLow = false;
			bool ToHigh = false;
			UUID LowCollider{};
			UUID HighCollider{};
			UUID ColliderBody{};
		};

		// A body or character inner body resolved to its owner and the collider entity of a sub-shape.
		struct ResolvedBody
		{
			UUID Owner{};
			UUID Collider{};
		};

	}

	namespace Utils {

		template<typename Component, auto Handler, typename Instance>
		static void ConnectComponentSignals(entt::registry& registry, Instance& instance)
		{
			registry.on_construct<Component>().template connect<Handler>(instance);
			registry.on_update<Component>().template connect<Handler>(instance);
			registry.on_destroy<Component>().template connect<Handler>(instance);
		}

		template<typename Component, typename Instance>
		static void DisconnectComponentSignals(entt::registry& registry, Instance& instance)
		{
			registry.on_construct<Component>().disconnect(&instance);
			registry.on_update<Component>().disconnect(&instance);
			registry.on_destroy<Component>().disconnect(&instance);
		}

		static bool SameBits(float a, float b)
		{
			return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
		}

		static bool SameBits(const glm::vec3& a, const glm::vec3& b)
		{
			return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z);
		}

		static bool SameBits(const glm::quat& a, const glm::quat& b)
		{
			return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z) && SameBits(a.w, b.w);
		}

		static bool SameBits(const glm::mat4& a, const glm::mat4& b)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!SameBits(a[column][row], b[column][row]))
						return false;
				}
			}
			return true;
		}

		static std::unexpected<Error> MakeNoBodyError(UUID entity)
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("entity {} has no physics body", entity))
					.WithHint("bodies exist for enabled entities with a RigidBody whose colliders the physics composition accepts"));
		}

		static std::unexpected<Error> MakeMotionTypeError(UUID entity, PhysicsMotionType type, std::string_view operation, std::string_view needed)
		{
			return std::unexpected(Error(ErrorCode::InvalidState,
				std::format("the body of entity {} is {}, and {} needs a {} body", entity, PhysicsMotionTypeToString(type), operation, needed)));
		}

		// The refusal of a velocity on a body that is not Dynamic: a Kinematic body's velocity comes from its Transform at every
		// step (PreStep's MoveKinematic), so a velocity set on it would be overwritten before the step uses it.
		static std::unexpected<Error> MakeVelocityError(UUID entity, PhysicsMotionType type, std::string_view operation)
		{
			std::unexpected<Error> error = MakeMotionTypeError(entity, type, operation, "Dynamic");
			if (type == PhysicsMotionType::Kinematic)
				return std::unexpected(std::move(error).error().WithHint("Kinematic bodies follow their Transform: move them with MoveKinematic"));
			return error;
		}

		// InvalidArgument unless every component is finite and at most `limit` in magnitude.
		static Status CheckVector(const glm::vec3& value, float limit, std::string_view what)
		{
			if (!IsFinite(value))
				return MakeError(ErrorCode::InvalidArgument, "the {} ({}, {}, {}) is not finite", what, value.x, value.y, value.z);
			if (std::abs(value.x) > limit || std::abs(value.y) > limit || std::abs(value.z) > limit)
				return MakeError(ErrorCode::InvalidArgument, "the {} ({}, {}, {}) is out of range: each component must be at most {}", what, value.x,
					value.y, value.z, limit);
			return {};
		}

		// The unit quaternion `rotation` normalized, or InvalidArgument when it is not finite or not unit (within the Quat field
		// rule's tolerance).
		static Result<glm::quat> CheckRotation(const glm::quat& rotation)
		{
			if (!IsFinite(rotation))
				return MakeError(ErrorCode::InvalidArgument, "the rotation ({}, {}, {}, {}) is not finite", rotation.x, rotation.y, rotation.z, rotation.w);
			const double length = std::sqrt(static_cast<double>(rotation.x) * rotation.x + static_cast<double>(rotation.y) * rotation.y
				+ static_cast<double>(rotation.z) * rotation.z + static_cast<double>(rotation.w) * rotation.w);
			if (std::abs(length - 1.0) > UnitQuaternionTolerance)
				return MakeError(ErrorCode::InvalidArgument, "the rotation ({}, {}, {}, {}) is not a unit quaternion (length {})", rotation.x, rotation.y,
					rotation.z, rotation.w, length);
			return glm::normalize(rotation);
		}

		// The entity's world matrix: WorldTransformComponent, which the step hooks find current (PlaySession runs
		// TransformSystem::Update before PreStep and before the system is created), else computed from the parent chain.
		static glm::mat4 GetWorldMatrix(ConstEntity entity)
		{
			if (const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>())
				return world->Matrix;
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		// A body origin's world pose from its entity: the world translation and the world rotation (TransformSystem's
		// convention: the parent's world rotation times the local one), without scale (baked into the shape).
		static PhysicsPose GetWorldPose(ConstEntity entity, const glm::mat4& world)
		{
			return PhysicsPose{ .Position = glm::vec3(world[3]), .Rotation = TransformSystem::GetWorldRotation(entity) };
		}

		// Writes the entity's local Translation and Rotation back to `translation` and `rotation` (values the system wrote or
		// placed the body from), directly (a runtime write, §5.1): a Transform written beyond the physics range is not applied.
		static void RestoreLocalPose(Entity entity, const glm::vec3& translation, const glm::quat& rotation)
		{
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			transform.Translation = translation;
			transform.Rotation = rotation;
		}

		// Writes `pose` as the entity's local Translation and Rotation, through the parent's current world inverse, directly
		// (a runtime write, §5.1); returns what it wrote.
		static std::pair<glm::vec3, glm::quat> WriteWorldPose(Entity entity, const PhysicsPose& pose)
		{
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			const Entity parent = entity.GetParent();
			if (parent.IsValid())
			{
				const glm::mat4 parentWorld = TransformSystem::ComputeWorldMatrix(parent);
				transform.Translation = glm::vec3(glm::affineInverse(parentWorld) * glm::vec4(pose.Position, 1.0f));
				transform.Rotation = glm::normalize(glm::conjugate(TransformSystem::GetWorldRotation(parent)) * pose.Rotation);
			}
			else
			{
				transform.Translation = pose.Position;
				transform.Rotation = pose.Rotation;
			}
			return { transform.Translation, transform.Rotation };
		}

		// The body settings of `plan` (BodyDescription without its shape, pose and velocities, §9.2 "Body settings").
		static BodyDescription MakeBodySettings(const PhysicsBodyPlan& plan, ConstEntity owner)
		{
			BodyDescription settings;
			settings.MotionType = plan.MotionType;
			settings.Layer = plan.Layer;
			settings.IsSensor = plan.IsSensor;
			// §9.2: sensors detect kinematic platforms and character inner bodies.
			settings.CollideKinematicVsNonDynamic = plan.IsSensor;
			settings.UserData = plan.Owner.GetValue();
			settings.CollisionGroup = plan.CollisionGroup.GetValue();
			if (plan.Origin == PhysicsBodyOrigin::RigidBody)
			{
				const RigidBodyComponent& body = owner.GetComponent<RigidBodyComponent>();
				settings.MotionQuality = ToPhysicsMotionQuality(body.MotionQuality);
				settings.AllowedDofs = GetRigidBodyDofs(body);
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

		static bool SameBodySettings(const BodyDescription& a, const BodyDescription& b)
		{
			return std::tie(a.MotionType, a.Layer, a.IsSensor, a.CollideKinematicVsNonDynamic, a.MotionQuality, a.AllowedDofs, a.Mass, a.Friction,
					   a.Restitution, a.LinearDamping, a.AngularDamping, a.GravityFactor, a.MaxLinearVelocity, a.MaxAngularVelocity, a.AllowSleeping,
					   a.EnhancedInternalEdgeRemoval, a.UserData, a.CollisionGroup)
				== std::tie(b.MotionType, b.Layer, b.IsSensor, b.CollideKinematicVsNonDynamic, b.MotionQuality, b.AllowedDofs, b.Mass, b.Friction,
					b.Restitution, b.LinearDamping, b.AngularDamping, b.GravityFactor, b.MaxLinearVelocity, b.MaxAngularVelocity, b.AllowSleeping,
					b.EnhancedInternalEdgeRemoval, b.UserData, b.CollisionGroup);
		}

		static bool SameGeometry(const PhysicsShapeGeometry& a, const PhysicsShapeGeometry& b)
		{
			if (a.index() != b.index())
				return false;
			if (const BoxShapeGeometry* box = std::get_if<BoxShapeGeometry>(&a))
				return box->HalfExtents == std::get<BoxShapeGeometry>(b).HalfExtents;
			if (const SphereShapeGeometry* sphere = std::get_if<SphereShapeGeometry>(&a))
				return sphere->Radius == std::get<SphereShapeGeometry>(b).Radius;
			if (const CapsuleShapeGeometry* capsule = std::get_if<CapsuleShapeGeometry>(&a))
			{
				const CapsuleShapeGeometry& other = std::get<CapsuleShapeGeometry>(b);
				return capsule->HalfHeight == other.HalfHeight && capsule->Radius == other.Radius;
			}
			if (const ConvexHullShapeGeometry* hull = std::get_if<ConvexHullShapeGeometry>(&a))
				return hull->Points == std::get<ConvexHullShapeGeometry>(b).Points;
			if (const MeshShapeGeometry* mesh = std::get_if<MeshShapeGeometry>(&a))
			{
				const MeshShapeGeometry& other = std::get<MeshShapeGeometry>(b);
				return mesh->Vertices == other.Vertices && mesh->Indices == other.Indices;
			}
			return std::get<SharedShapeGeometry>(a).Shape == std::get<SharedShapeGeometry>(b).Shape;
		}

		static bool SameShapeDescription(const BodyShapeDescription& a, const BodyShapeDescription& b)
		{
			return std::equal(a.Colliders.begin(), a.Colliders.end(), b.Colliders.begin(), b.Colliders.end(),
				[](const ColliderShapeDescription& left, const ColliderShapeDescription& right)
			{
				return left.UserData == right.UserData && left.Scale == right.Scale && left.Position == right.Position && left.Rotation == right.Rotation
					&& SameGeometry(left.Geometry, right.Geometry);
			});
		}

		static bool SameColliders(const std::vector<PhysicsColliderPlan>& a, const std::vector<PhysicsColliderPlan>& b)
		{
			return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const PhysicsColliderPlan& left, const PhysicsColliderPlan& right)
			{
				return left.Entity == right.Entity && left.Type == right.Type && left.IsTrigger == right.IsTrigger;
			});
		}

		static bool SameCharacterDescription(const CharacterControllerDescription& a, const CharacterControllerDescription& b)
		{
			return std::tie(a.Height, a.Radius, a.MaxSlopeAngle, a.StepHeight, a.Mass, a.Layer, a.UserData, a.CollisionGroup)
				== std::tie(b.Height, b.Radius, b.MaxSlopeAngle, b.StepHeight, b.Mass, b.Layer, b.UserData, b.CollisionGroup);
		}

		// `message` without a leading "<PHYSICS CODE>: ".
		static std::string_view StripCode(std::string_view message, std::string_view code)
		{
			if (!code.empty() && message.starts_with(code) && message.size() > code.size() + 1)
				return message.substr(code.size() + 2);
			return message;
		}

		// The sub-shape key of a record, its bodies ordered by handle value.
		static PhysicsSubShapeKey MakeSubShapeKey(const ContactEvent& record)
		{
			if (record.BodyA.GetValue() <= record.BodyB.GetValue())
				return { record.BodyA.GetValue(), record.SubShapeA, record.BodyB.GetValue(), record.SubShapeB, record.FromCharacter };
			return { record.BodyB.GetValue(), record.SubShapeB, record.BodyA.GetValue(), record.SubShapeA, record.FromCharacter };
		}

		static PhysicsEventType GetEnterType(PhysicsPairKind kind)
		{
			return kind == PhysicsPairKind::Trigger ? PhysicsEventType::TriggerEnter : PhysicsEventType::CollisionEnter;
		}

		static PhysicsEventType GetExitType(PhysicsPairKind kind)
		{
			return kind == PhysicsPairKind::Trigger ? PhysicsEventType::TriggerExit : PhysicsEventType::CollisionExit;
		}

		static bool IsEnter(PhysicsEventType type)
		{
			return type == PhysicsEventType::CollisionEnter || type == PhysicsEventType::TriggerEnter;
		}

		// The side (0: the contact's BodyA, 1: its BodyB) whose collider is `collider` gathered into another owner's body.
		static std::optional<size_t> FindGatheredSide(const PhysicsSubShapeContact& contact, UUID collider)
		{
			for (size_t side = 0; side < 2; ++side)
			{
				if (contact.Colliders[side] == collider && contact.Owners[side] != collider)
					return side;
			}
			return std::nullopt;
		}

		// The collider pair of `collider` with the partner's body owner `partner` (PhysicsPairKey).
		static PhysicsPairKey MakeColliderPairKey(UUID collider, UUID partner, PhysicsPairKind kind)
		{
			const bool colliderIsLow = collider < partner;
			return PhysicsPairKey{ .Low = colliderIsLow ? collider : partner, .High = colliderIsLow ? partner : collider, .Kind = kind, .Collider = collider };
		}

		// The side of `contact` on which `key`'s Low party is: a body pair's Low owner, or a collider pair's collider or partner.
		static size_t GetLowSide(const PhysicsPairKey& key, const PhysicsSubShapeContact& contact)
		{
			if (!key.IsColliderPair())
				return contact.Owners[0] == key.Low ? 0 : 1;
			const size_t colliderSide = FindGatheredSide(contact, key.Collider).value_or(0);
			return key.Low == key.Collider ? colliderSide : 1 - colliderSide;
		}

		// `contact` as the enter contact of a pair whose Low party is on its side `lowSide`.
		static FirstContact MakeFirstContact(const PhysicsSubShapeKey& key, const PhysicsSubShapeContact& contact, size_t lowSide)
		{
			const std::array<uint32_t, 2> subShapes = { key.SubShapeA, key.SubShapeB };
			FirstContact candidate;
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

		// The canonical order key (PhysicsEventOrderKey) of a pair's event of `type`: a body pair's owners, or a collider pair's
		// collider body owner and partner with the collider.
		static PhysicsEventOrderKey GetPairOrderKey(const PhysicsPairKey& key, PhysicsEventType type, UUID colliderBody)
		{
			if (!key.IsColliderPair())
				return PhysicsEventOrderKey{ .Low = key.Low, .High = key.High, .Type = type, .Collider = UUID() };
			const UUID partner = key.Low == key.Collider ? key.High : key.Low;
			const bool bodyIsLow = colliderBody < partner;
			return PhysicsEventOrderKey{ .Low = bodyIsLow ? colliderBody : partner, .High = bodyIsLow ? partner : colliderBody, .Type = type, .Collider = key.Collider };
		}

		static void AppendFloat(std::vector<std::byte>& bytes, float value)
		{
			const auto bits = std::bit_cast<uint32_t>(value);
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffu));
		}

		static void AppendVector(std::vector<std::byte>& bytes, const glm::vec3& value)
		{
			AppendFloat(bytes, value.x);
			AppendFloat(bytes, value.y);
			AppendFloat(bytes, value.z);
		}

		// The body origins of one owner, in the order its bodies are listed, hashed and reported (PhysicsComposition.h).
		constexpr std::array<PhysicsBodyOrigin, 3> BodyOrigins = { PhysicsBodyOrigin::RigidBody, PhysicsBodyOrigin::ImplicitStatic,
			PhysicsBodyOrigin::ImplicitSensor };

	}

	// --- State: signals and structure ---------------------------------------------------------------------------------

	void PhysicsSystem::State::ConnectSignals()
	{
		entt::registry& registry = Specification.RuntimeScene->GetRegistry();
		Utils::ConnectComponentSignals<RigidBodyComponent, &State::OnComponentChanged>(registry, *this);
		Utils::ConnectComponentSignals<BoxColliderComponent, &State::OnComponentChanged>(registry, *this);
		Utils::ConnectComponentSignals<SphereColliderComponent, &State::OnComponentChanged>(registry, *this);
		Utils::ConnectComponentSignals<CapsuleColliderComponent, &State::OnComponentChanged>(registry, *this);
		Utils::ConnectComponentSignals<MeshColliderComponent, &State::OnComponentChanged>(registry, *this);
		Utils::ConnectComponentSignals<CharacterControllerComponent, &State::OnComponentChanged>(registry, *this);
		Utils::ConnectComponentSignals<MeshRendererComponent, &State::OnComponentChanged>(registry, *this);
		registry.on_construct<DisabledTag>().connect<&State::OnComponentChanged>(*this);
		registry.on_destroy<DisabledTag>().connect<&State::OnComponentChanged>(*this);
		registry.on_construct<PendingDestroyTag>().connect<&State::OnEntityLeavingPlay>(*this);
		registry.on_construct<HierarchyDisabledTag>().connect<&State::OnEntityLeavingPlay>(*this);
	}

	void PhysicsSystem::State::DisconnectSignals()
	{
		entt::registry& registry = Specification.RuntimeScene->GetRegistry();
		Utils::DisconnectComponentSignals<RigidBodyComponent>(registry, *this);
		Utils::DisconnectComponentSignals<BoxColliderComponent>(registry, *this);
		Utils::DisconnectComponentSignals<SphereColliderComponent>(registry, *this);
		Utils::DisconnectComponentSignals<CapsuleColliderComponent>(registry, *this);
		Utils::DisconnectComponentSignals<MeshColliderComponent>(registry, *this);
		Utils::DisconnectComponentSignals<CharacterControllerComponent>(registry, *this);
		Utils::DisconnectComponentSignals<MeshRendererComponent>(registry, *this);
		registry.on_construct<DisabledTag>().disconnect(this);
		registry.on_destroy<DisabledTag>().disconnect(this);
		registry.on_construct<PendingDestroyTag>().disconnect(this);
		registry.on_construct<HierarchyDisabledTag>().disconnect(this);
	}

	void PhysicsSystem::State::OnComponentChanged(entt::registry& registry, entt::entity entity)
	{
		ComponentsChanged = true;
		// An entity being destroyed may have lost its IDComponent already; the hierarchy change covers it.
		if (const IDComponent* id = registry.try_get<IDComponent>(entity))
			ChangedEntities.insert(id->ID);
	}

	void PhysicsSystem::State::OnEntityLeavingPlay(entt::registry& registry, entt::entity entity)
	{
		if (const IDComponent* id = registry.try_get<IDComponent>(entity))
			FlushCandidates.insert(id->ID);
	}

	bool PhysicsSystem::State::IsPresent(UUID entity) const
	{
		const ConstEntity found = std::as_const(*Specification.RuntimeScene).FindEntityByID(entity);
		return found.IsValid() && !found.HasComponent<HierarchyDisabledTag>();
	}

	bool PhysicsSystem::State::HasHierarchyChanged()
	{
		const Scene& scene = *Specification.RuntimeScene;
		if (scene.GetRevision() == SeenRevision)
			return false;
		const std::vector<std::pair<UUID, UUID>> previous = std::move(SeenHierarchy);
		RecordHierarchy();
		return SeenHierarchy != previous;
	}

	void PhysicsSystem::State::RecordHierarchy()
	{
		const Scene& scene = *Specification.RuntimeScene;
		SeenRevision = scene.GetRevision();
		SeenHierarchy.clear();
		const std::span<const UUID> order = scene.GetCanonicalOrder();
		SeenHierarchy.reserve(order.size());
		for (const UUID id : order)
			SeenHierarchy.emplace_back(id, scene.FindEntityByID(id).GetComponent<RelationshipComponent>().Parent);
	}

	// --- State: diagnostics -------------------------------------------------------------------------------------------

	void PhysicsSystem::State::RaiseDiagnostic(PhysicsDiagnostic diagnostic, bool alreadyLogged)
	{
		// Severity is part of the key: a limit's warning near it and its error at it are two diagnostics.
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

	void PhysicsSystem::State::RaiseBodyError(const PhysicsBodyPlan& plan, const Error& error, std::string_view field)
	{
		const Scene& scene = *Specification.RuntimeScene;
		std::string_view code = GetPhysicsDiagnosticCode(error);
		const std::string_view reason = Utils::StripCode(error.GetMessageText(), code);
		// A refusal without a physics code is a value the world does not place (a position beyond MaxPhysicsCoordinate) or
		// one no write path lets through (the registry's field rules hold for every edit); it is reported as the body's
		// invalid shape so the body is still refused with a diagnostic, never an assert.
		const bool carriesCode = !code.empty();
		if (!carriesCode)
			code = PhysicsInvalidShapeCode;

		UUID entity = plan.Owner;
		std::string component;
		if (plan.Origin == PhysicsBodyOrigin::Character)
			component = "CharacterController";
		else if (plan.Origin == PhysicsBodyOrigin::RigidBody && (code != PhysicsInvalidShapeCode || !carriesCode || !field.empty()))
			component = "RigidBody";
		if (component.empty() && code == PhysicsInvalidShapeCode)
		{
			// The collider at fault: the entity the error names (DescribePhysicsBodyShape), or the collider index Jolt's shape
			// error names (PhysicsShape::Create).
			if (error.GetLocation().Entity.IsValid())
				entity = error.GetLocation().Entity;
			else if (const std::optional<uint32_t> index = Utils::ParseColliderIndex(reason); index.has_value() && *index < plan.Colliders.size())
				entity = plan.Colliders[*index].Entity;
			const auto collider = std::find_if(plan.Colliders.begin(), plan.Colliders.end(), [entity](const PhysicsColliderPlan& candidate)
			{
				return candidate.Entity == entity;
			});
			if (collider != plan.Colliders.end())
				component = std::string(Utils::GetColliderComponentName(collider->Type));
		}

		const ConstEntity owner = scene.FindEntityByID(plan.Owner);
		const std::string ownerName = owner.IsValid() ? owner.GetName() : std::string();
		PhysicsDiagnostic diagnostic;
		diagnostic.Code = std::string(code);
		diagnostic.Severity = DiagnosticSeverity::Error;
		diagnostic.Entity = entity;
		diagnostic.Component = std::move(component);
		diagnostic.Field = std::string(field);
		diagnostic.Message = std::format("the {} of '{}' ({}) is not created: {}", plan.Origin == PhysicsBodyOrigin::Character ? "character" : "body",
			ownerName, plan.Owner, reason);
		diagnostic.Hint = error.GetHint().empty() ? std::string("fix the component values the message names; the rest of the scene keeps simulating")
												  : error.GetHint();
		if (code == PhysicsLimitExceededCode)
			diagnostic.Subject = "bodies";
		RaiseDiagnostic(std::move(diagnostic));
	}

	void PhysicsSystem::State::DeliverPendingDiagnostics()
	{
		if (PendingListenerDiagnostics.empty())
			return;
		std::vector<PhysicsDiagnostic> pending = std::move(PendingListenerDiagnostics);
		PendingListenerDiagnostics.clear();
		if (Listener == nullptr)
			return;
		Dispatching = true;
		for (const PhysicsDiagnostic& diagnostic : pending)
			Listener->OnPhysicsDiagnostic(diagnostic);
		Dispatching = false;
	}

	// --- State: contacts and pairs ------------------------------------------------------------------------------------

	PhysicsSubShapeContact& PhysicsSystem::State::AddContact(const PhysicsSubShapeKey& key, const PhysicsPairKey& pair)
	{
		PhysicsSubShapeContact& contact = Contacts[key];
		contact.Pair = pair;
		const auto found = Pairs.find(pair);
		ENGINE_CORE_VERIFY(found != Pairs.end(), "PhysicsSystem: a contact is added to a pair that does not exist");
		if (found != Pairs.end())
			found->second.Contacts.insert(key);
		ContactsByBody[key.BodyA].insert(key);
		ContactsByBody[key.BodyB].insert(key);
		return contact;
	}

	void PhysicsSystem::State::EraseContact(const PhysicsSubShapeKey& key)
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

	void PhysicsSystem::State::ErasePair(const PhysicsPairKey& key)
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
			// A collider pair's contacts stay in their body pair.
			const auto found = Contacts.find(contact);
			ENGINE_CORE_VERIFY(found != Contacts.end(), "PhysicsSystem: a collider pair counts a contact that does not exist (Pairs and Contacts out of step)");
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

	void PhysicsSystem::State::PurgeBodyContacts(uint32_t handle, std::set<UUID>& partners)
	{
		const auto found = ContactsByBody.find(handle);
		if (found == ContactsByBody.end())
			return;
		const std::set<PhysicsSubShapeKey> contacts = found->second;
		for (const PhysicsSubShapeKey& contact : contacts)
		{
			const auto state = Contacts.find(contact);
			ENGINE_CORE_VERIFY(state != Contacts.end(), "PhysicsSystem: a body's contact does not exist (ContactsByBody and Contacts out of step)");
			if (state == Contacts.end())
				continue;
			// Its body pair first, then its collider pairs (M20), each of which follows the body pair's rule.
			std::vector<PhysicsPairKey> keys = { state->second.Pair };
			keys.insert(keys.end(), state->second.ColliderPairs.begin(), state->second.ColliderPairs.end());
			EraseContact(contact);
			for (const PhysicsPairKey& key : keys)
			{
				const auto pairEntry = Pairs.find(key);
				ENGINE_CORE_VERIFY(pairEntry != Pairs.end(), "PhysicsSystem: a contact names a pair that does not exist (Contacts and Pairs out of step)");
				if (pairEntry == Pairs.end())
					continue;
				PhysicsPair& pair = pairEntry->second;
				// The partners woken are the body pairs' owners only: a collider pair never changes the simulation.
				if (!key.IsColliderPair())
				{
					partners.insert(key.Low);
					partners.insert(key.High);
				}
				if (pair.Contacts.empty())
				{
					if (pair.Active)
						pair.Unconfirmed = true; // §9.4: PostStep ends it unless a contact re-establishes it
					else
						ErasePair(key);
				}
			}
		}
	}

	void PhysicsSystem::State::SetContactSides(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const std::array<UUID, 2>& owners,
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

	void PhysicsSystem::State::LinkColliderPair(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const PhysicsPairKey& pair)
	{
		const auto found = Pairs.find(pair);
		ENGINE_CORE_VERIFY(found != Pairs.end(), "PhysicsSystem: a contact is linked to a collider pair that does not exist");
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

	void PhysicsSystem::State::UnlinkColliderPair(const PhysicsSubShapeKey& key, PhysicsSubShapeContact& contact, const PhysicsPairKey& pair)
	{
		if (const auto found = Pairs.find(pair); found != Pairs.end())
			found->second.Contacts.erase(key);
		std::erase(contact.ColliderPairs, pair);
	}

	bool PhysicsSystem::State::IsScriptedGatheredCollider(UUID entity) const
	{
		if (!GatheredColliders.contains(entity))
			return false;
		const ConstEntity found = std::as_const(*Specification.RuntimeScene).FindEntityByID(entity);
		if (!found.IsValid() || found.HasComponent<HierarchyDisabledTag>())
			return false;
		// A Script component names a script when one of its slots has one (Docs/Decisions/0031-m20-contract.md decision 17).
		const ScriptComponent* script = found.TryGetComponent<ScriptComponent>();
		return script != nullptr && std::ranges::any_of(script->Scripts, [](const ScriptSlot& slot)
		{
			return slot.Script.IsValid();
		});
	}

	void PhysicsSystem::State::CollectPartners(UUID owner, std::set<UUID>& partners) const
	{
		const auto owned = PairsByOwner.find(owner);
		if (owned == PairsByOwner.end())
			return;
		for (const PhysicsPairKey& key : owned->second)
		{
			// Body pairs only: a collider pair never changes the simulation.
			if (!key.IsColliderPair())
				partners.insert(key.Low == owner ? key.High : key.Low);
		}
	}

	void PhysicsSystem::State::WakeOwners(const std::set<UUID>& owners)
	{
		for (const UUID owner : owners)
		{
			for (const PhysicsBodyOrigin origin : Utils::BodyOrigins)
			{
				const PhysicsBodyEntry* entry = FindOwnedBody(owner, origin);
				if (entry != nullptr && entry->Plan.MotionType != PhysicsMotionType::Static)
					World->WakeUp(entry->Handle);
			}
			if (const auto character = Characters.find(owner); character != Characters.end())
				World->WakeUp(character->second.Controller->GetInnerBody());
		}
	}

	// --- State: bodies ------------------------------------------------------------------------------------------------

	const PhysicsBodyEntry* PhysicsSystem::State::FindOwnedBody(UUID owner, PhysicsBodyOrigin origin) const
	{
		const auto found = BodiesByOwner.find({ owner, origin });
		return found != BodiesByOwner.end() ? FindBody(found->second) : nullptr;
	}

	PhysicsBodyEntry* PhysicsSystem::State::FindOwnedBody(UUID owner, PhysicsBodyOrigin origin)
	{
		const auto found = BodiesByOwner.find({ owner, origin });
		return found != BodiesByOwner.end() ? FindBody(found->second) : nullptr;
	}

	const PhysicsBodyEntry* PhysicsSystem::State::FindBody(uint32_t handle) const
	{
		const auto found = Bodies.find(handle);
		ENGINE_CORE_VERIFY(found != Bodies.end(), "PhysicsSystem: an index names body 0x{:08x}, which the body table does not hold", handle);
		return found != Bodies.end() ? &found->second : nullptr;
	}

	PhysicsBodyEntry* PhysicsSystem::State::FindBody(uint32_t handle)
	{
		const auto found = Bodies.find(handle);
		ENGINE_CORE_VERIFY(found != Bodies.end(), "PhysicsSystem: an index names body 0x{:08x}, which the body table does not hold", handle);
		return found != Bodies.end() ? &found->second : nullptr;
	}

	Result<PhysicsBodyEntry*> PhysicsSystem::State::FindRigidBody(UUID entity)
	{
		PhysicsBodyEntry* entry = FindOwnedBody(entity, PhysicsBodyOrigin::RigidBody);
		if (entry == nullptr || !IsPresent(entity))
			return Utils::MakeNoBodyError(entity);
		return entry;
	}

	Result<const PhysicsBodyEntry*> PhysicsSystem::State::FindRigidBody(UUID entity) const
	{
		const PhysicsBodyEntry* entry = FindOwnedBody(entity, PhysicsBodyOrigin::RigidBody);
		if (entry == nullptr || !IsPresent(entity))
			return Utils::MakeNoBodyError(entity);
		return entry;
	}

	std::set<UUID> PhysicsSystem::State::FindShapeChanges(bool hierarchyChanged) const
	{
		const entt::registry& registry = Specification.RuntimeScene->GetRegistry();
		const auto sameParts = [](const PhysicsShapeInput& input, const TransformComponent& local)
		{
			switch (input.Parts)
			{
				case PhysicsShapeInputParts::Scale:
					return Utils::SameBits(local.Scale, input.Local.Scale);
				case PhysicsShapeInputParts::ScaleRotation:
					return Utils::SameBits(local.Scale, input.Local.Scale) && Utils::SameBits(local.Rotation, input.Local.Rotation);
				case PhysicsShapeInputParts::Whole:
					break;
			}
			return Utils::SameBits(local.Translation, input.Local.Translation) && Utils::SameBits(local.Rotation, input.Local.Rotation)
				&& Utils::SameBits(local.Scale, input.Local.Scale);
		};
		std::set<UUID> owners;
		for (const auto& [handle, entry] : Bodies)
		{
			bool changed = false;
			if (hierarchyChanged && IsPresent(entry.Owner))
			{
				// The entities a shape bakes in may be others now (a reparent above the owner or between it and a collider).
				const std::vector<PhysicsShapeInput> current = CaptureShapeInputs(entry.Plan);
				changed = !std::equal(current.begin(), current.end(), entry.ShapeInputs.begin(), entry.ShapeInputs.end(),
					[&sameParts](const PhysicsShapeInput& now, const PhysicsShapeInput& then)
				{
					return now.Entity == then.Entity && now.Parts == then.Parts && sameParts(then, now.Local);
				});
			}
			else
			{
				changed = std::any_of(entry.ShapeInputs.begin(), entry.ShapeInputs.end(), [&registry, &sameParts](const PhysicsShapeInput& input)
				{
					const TransformComponent* local = registry.valid(input.Entity) ? registry.try_get<TransformComponent>(input.Entity) : nullptr;
					return local == nullptr || !sameParts(input, *local);
				});
			}
			const bool meshChanged = Specification.Assets != nullptr
				&& std::any_of(entry.MeshInputs.begin(), entry.MeshInputs.end(), [this](const PhysicsMeshInput& input)
			{
				return Specification.Assets->GetVersion(input.Mesh) != input.Version;
			});
			if (changed || meshChanged)
				owners.insert(entry.Owner);
		}
		return owners;
	}

	bool PhysicsSystem::State::CreateBody(const PhysicsBodyPlan& plan, std::optional<std::pair<glm::vec3, glm::vec3>> velocities)
	{
		const Scene& scene = *Specification.RuntimeScene;
		const ConstEntity owner = scene.FindEntityByID(plan.Owner);
		Result<BodyShapeDescription> description = DescribePhysicsBodyShape(scene, plan, Specification.Assets, &MeshShapes);
		if (!description)
		{
			RaiseBodyError(plan, description.error());
			return false;
		}
		Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(*description);
		if (!shape)
		{
			RaiseBodyError(plan, shape.error());
			return false;
		}

		const glm::mat4 world = Utils::GetWorldMatrix(owner);
		BodyDescription body = Utils::MakeBodySettings(plan, owner);
		const BodyDescription settings = body;
		if (plan.MotionType == PhysicsMotionType::Dynamic)
		{
			// The world checks the same; checked here, the refusal names the RigidBody's field.
			if (const Status checked = (*shape)->CheckDynamicBody(body.Mass, body.AllowedDofs); !checked)
			{
				RaiseBodyError(plan, checked.error(), Utils::GetDynamicBodyField(checked.error()));
				return false;
			}
		}
		body.Shape = *shape;
		body.Pose = Utils::GetWorldPose(owner, world);
		if (velocities.has_value())
		{
			body.LinearVelocity = velocities->first;
			body.AngularVelocity = velocities->second;
		}
		else if (plan.Origin == PhysicsBodyOrigin::RigidBody && plan.MotionType == PhysicsMotionType::Dynamic)
		{
			// Only Dynamic bodies start moving: a Kinematic body follows its Transform (PhysicsSystem.h).
			const RigidBodyComponent& component = owner.GetComponent<RigidBodyComponent>();
			body.LinearVelocity = component.InitialLinearVelocity;
			body.AngularVelocity = component.InitialAngularVelocity;
		}
		const Result<BodyHandle> handle = World->CreateBody(body);
		if (!handle)
		{
			RaiseBodyError(plan, handle.error());
			return false;
		}

		PhysicsBodyEntry entry;
		entry.Handle = *handle;
		entry.Owner = plan.Owner;
		entry.Origin = plan.Origin;
		entry.Shape = std::move(*shape);
		const TransformComponent& local = owner.GetComponent<TransformComponent>();
		entry.WrittenTranslation = local.Translation;
		entry.WrittenRotation = local.Rotation;
		entry.PlacedMatrix = world;
		entry.Settings = settings;
		entry.ShapeDescription = std::move(*description);
		const uint32_t value = handle->GetValue();
		BodiesByOwner[{ plan.Owner, plan.Origin }] = value;
		AssignPlan(Bodies.emplace(value, std::move(entry)).first->second, plan);
		return true;
	}

	std::vector<PhysicsShapeInput> PhysicsSystem::State::CaptureShapeInputs(const PhysicsBodyPlan& plan) const
	{
		// What a shape bakes in: the owner's world scale (the scales from the root down to the owner, and the rotations below a
		// non-uniformly scaled ancestor), then every local transform below the owner down to each collider, each entity once.
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
		for (const PhysicsColliderPlan& collider : plan.Colliders)
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

	std::vector<PhysicsMeshInput> PhysicsSystem::State::CaptureMeshInputs(const PhysicsBodyPlan& plan) const
	{
		std::vector<PhysicsMeshInput> inputs;
		if (Specification.Assets == nullptr)
			return inputs;
		const Scene& scene = *Specification.RuntimeScene;
		for (const PhysicsColliderPlan& collider : plan.Colliders)
		{
			const ConstEntity entity = scene.FindEntityByID(collider.Entity);
			if (collider.Type != PhysicsColliderType::Mesh || !entity.IsValid())
				continue;
			AssetHandle mesh = entity.GetComponent<MeshColliderComponent>().Mesh.GetHandle();
			if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>(); !mesh.IsValid() && renderer != nullptr)
				mesh = renderer->Mesh.GetHandle();
			inputs.push_back(PhysicsMeshInput{ .Mesh = mesh, .Version = Specification.Assets->GetVersion(mesh) });
		}
		return inputs;
	}

	void PhysicsSystem::State::AssignPlan(PhysicsBodyEntry& entry, const PhysicsBodyPlan& plan)
	{
		const uint32_t handle = entry.Handle.GetValue();
		for (const UUID collider : entry.Colliders)
		{
			if (const auto gathered = GatheredColliders.find(collider); gathered != GatheredColliders.end() && gathered->second == handle)
				GatheredColliders.erase(gathered);
		}
		entry.Plan = plan;
		entry.Colliders.clear();
		for (const PhysicsColliderPlan& collider : plan.Colliders)
		{
			entry.Colliders.push_back(collider.Entity);
			if (collider.Entity != plan.Owner)
				GatheredColliders[collider.Entity] = handle;
		}

		entry.ShapeInputs = CaptureShapeInputs(plan);
		entry.MeshInputs = CaptureMeshInputs(plan);
	}

	void PhysicsSystem::State::RemoveBody(uint32_t handle, std::set<UUID>& woken)
	{
		const auto found = Bodies.find(handle);
		if (found == Bodies.end())
			return;
		const PhysicsBodyEntry& entry = found->second;
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

	bool PhysicsSystem::State::ReplaceShape(PhysicsBodyEntry& entry, const PhysicsBodyPlan& plan, std::set<UUID>& woken)
	{
		const Scene& scene = *Specification.RuntimeScene;
		const uint32_t handle = entry.Handle.GetValue();
		Result<BodyShapeDescription> description = DescribePhysicsBodyShape(scene, plan, Specification.Assets, &MeshShapes);
		if (!description)
		{
			// A body refused after an edit: removed, its pairs end at PostStep (§9.4).
			RaiseBodyError(plan, description.error());
			RemoveBody(handle, woken);
			return false;
		}
		// An input written back to the same value (a transform restored, a hierarchy change elsewhere) changes nothing.
		if (!Utils::SameShapeDescription(*description, entry.ShapeDescription))
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(*description);
			if (!shape)
			{
				RaiseBodyError(plan, shape.error());
				RemoveBody(handle, woken);
				return false;
			}
			if (plan.MotionType == PhysicsMotionType::Dynamic)
			{
				if (const Status checked = (*shape)->CheckDynamicBody(entry.Settings.Mass, entry.Settings.AllowedDofs); !checked)
				{
					RaiseBodyError(plan, checked.error(), Utils::GetDynamicBodyField(checked.error()));
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
		// The collider behind a sub-shape ID of the body may be another one now (M20, PhysicsSystemState.h).
		ReattributedBodies.insert(handle);
		return true;
	}

	bool PhysicsSystem::State::CreateCharacter(const PhysicsBodyPlan& plan, std::optional<glm::vec3> velocity)
	{
		const Scene& scene = *Specification.RuntimeScene;
		const ConstEntity owner = scene.FindEntityByID(plan.Owner);
		const CharacterControllerComponent& component = owner.GetComponent<CharacterControllerComponent>();
		CharacterControllerDescription description;
		description.Height = component.Height;
		description.Radius = component.Radius;
		description.MaxSlopeAngle = component.MaxSlopeAngle;
		description.StepHeight = component.StepHeight;
		description.Mass = component.Mass;
		description.Layer = plan.Layer;
		description.Pose = Utils::GetWorldPose(owner, Utils::GetWorldMatrix(owner));
		description.UserData = plan.Owner.GetValue();
		description.CollisionGroup = plan.CollisionGroup.GetValue();
		Result<Scope<CharacterController>> controller = CharacterController::Create(*World, description);
		if (!controller)
		{
			RaiseBodyError(plan, controller.error());
			return false;
		}
		if (velocity.has_value())
			(*controller)->SetVelocity(*velocity);

		PhysicsCharacterEntry entry;
		entry.Owner = plan.Owner;
		entry.Controller = std::move(*controller);
		const TransformComponent& local = owner.GetComponent<TransformComponent>();
		entry.WrittenTranslation = local.Translation;
		entry.WrittenRotation = local.Rotation;
		entry.Description = description;
		entry.GravityFactor = component.GravityFactor;
		InnerBodies[entry.Controller->GetInnerBody().GetValue()] = plan.Owner;
		Characters.emplace(plan.Owner, std::move(entry));
		return true;
	}

	void PhysicsSystem::State::RemoveCharacter(UUID owner, std::set<UUID>& woken)
	{
		const auto found = Characters.find(owner);
		if (found == Characters.end())
			return;
		const uint32_t inner = found->second.Controller->GetInnerBody().GetValue();
		PurgeBodyContacts(inner, woken);
		InnerBodies.erase(inner);
		Characters.erase(found);
	}

	void PhysicsSystem::State::Rebuild(bool hierarchyChanged, std::set<UUID>& woken)
	{
		Scene& scene = *Specification.RuntimeScene;
		const PhysicsComposition composition = ComposePhysicsBodies(scene, Layers, Specification.Limits.MaxBodies);
		// Each entity refused by the body limit gets its diagnostic (recorded and heard by the listener), but only the first
		// is logged: a scene far over the limit would log thousands of identical lines.
		bool loggedBodyLimit = false;
		for (const PhysicsDiagnostic& diagnostic : composition.Diagnostics)
		{
			const bool isBodyLimit = diagnostic.Code == PhysicsLimitExceededCode && diagnostic.Subject == "bodies";
			RaiseDiagnostic(diagnostic, isBodyLimit && loggedBodyLimit);
			loggedBodyLimit = loggedBodyLimit || isBodyLimit;
		}

		std::set<std::pair<UUID, PhysicsBodyOrigin>> wanted;
		for (const PhysicsBodyPlan& plan : composition.Bodies)
		{
			if (plan.IsCreatable)
				wanted.emplace(plan.Owner, plan.Origin);
		}

		// Removals first, so their slots are free: bodies and characters no creatable plan wants any more, except those of
		// entities pending destruction or disabled, which keep theirs until FlushDestroyed (§5.7 step 8).
		std::vector<std::pair<UUID, PhysicsBodyOrigin>> stale;
		for (const auto& [key, handle] : BodiesByOwner)
		{
			if (!wanted.contains(key) && IsPresent(key.first))
				stale.push_back(key);
		}
		for (const auto& [owner, entry] : Characters)
		{
			if (!wanted.contains({ owner, PhysicsBodyOrigin::Character }) && IsPresent(owner))
				stale.emplace_back(owner, PhysicsBodyOrigin::Character);
		}
		std::sort(stale.begin(), stale.end());
		for (const auto& [owner, origin] : stale)
		{
			if (origin == PhysicsBodyOrigin::Character)
				RemoveCharacter(owner, woken);
			else if (const PhysicsBodyEntry* entry = FindOwnedBody(owner, origin))
				RemoveBody(entry->Handle.GetValue(), woken);
		}

		const std::set<UUID> shapeChanges = FindShapeChanges(hierarchyChanged);
		for (const PhysicsBodyPlan& plan : composition.Bodies)
		{
			if (!plan.IsCreatable)
				continue;
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);

			if (plan.Origin == PhysicsBodyOrigin::Character)
			{
				const auto existing = Characters.find(plan.Owner);
				if (existing == Characters.end())
				{
					static_cast<void>(CreateCharacter(plan, std::nullopt));
					continue;
				}
				const CharacterControllerComponent& component = owner.GetComponent<CharacterControllerComponent>();
				existing->second.GravityFactor = component.GravityFactor;
				CharacterControllerDescription wantedDescription = existing->second.Description;
				wantedDescription.Height = component.Height;
				wantedDescription.Radius = component.Radius;
				wantedDescription.MaxSlopeAngle = component.MaxSlopeAngle;
				wantedDescription.StepHeight = component.StepHeight;
				wantedDescription.Mass = component.Mass;
				wantedDescription.Layer = plan.Layer;
				wantedDescription.CollisionGroup = plan.CollisionGroup.GetValue();
				if (Utils::SameCharacterDescription(wantedDescription, existing->second.Description))
					continue;
				const glm::vec3 velocity = existing->second.Controller->GetVelocity();
				RemoveCharacter(plan.Owner, woken);
				static_cast<void>(CreateCharacter(plan, velocity));
				continue;
			}

			PhysicsBodyEntry* existing = FindOwnedBody(plan.Owner, plan.Origin);
			if (existing == nullptr)
			{
				static_cast<void>(CreateBody(plan, std::nullopt));
				continue;
			}

			const bool settingsChanged = existing->Plan.MotionType != plan.MotionType || existing->Plan.IsSensor != plan.IsSensor
				|| !Utils::SameBodySettings(existing->Settings, Utils::MakeBodySettings(plan, owner));
			if (settingsChanged)
			{
				// §9.2: a RigidBody change creates the body again, with its current velocities.
				const uint32_t handle = existing->Handle.GetValue();
				const auto velocities = std::make_pair(World->GetLinearVelocity(existing->Handle), World->GetAngularVelocity(existing->Handle));
				RemoveBody(handle, woken);
				static_cast<void>(CreateBody(plan, velocities));
				continue;
			}

			const bool touched = shapeChanges.contains(plan.Owner) || ChangedEntities.contains(plan.Owner)
				|| std::any_of(plan.Colliders.begin(), plan.Colliders.end(), [this](const PhysicsColliderPlan& collider)
			{
				return ChangedEntities.contains(collider.Entity);
			}) || !Utils::SameColliders(plan.Colliders, existing->Plan.Colliders);
			if (touched)
				static_cast<void>(ReplaceShape(*existing, plan, woken));
		}
		MeshShapes.Prune();
	}

	void PhysicsSystem::State::RefreshShapes(const std::set<UUID>& owners, std::set<UUID>& woken)
	{
		for (const UUID owner : owners)
		{
			for (const PhysicsBodyOrigin origin : Utils::BodyOrigins)
			{
				PhysicsBodyEntry* entry = FindOwnedBody(owner, origin);
				if (entry == nullptr || !IsPresent(owner))
					continue;
				const PhysicsBodyPlan plan = entry->Plan;
				static_cast<void>(ReplaceShape(*entry, plan, woken));
			}
		}
		MeshShapes.Prune();
	}

	// --- PhysicsSystem ------------------------------------------------------------------------------------------------

	PhysicsSystem::PhysicsSystem(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PhysicsSystem::~PhysicsSystem()
	{
		State& state = *m_State;
		if (state.Specification.RuntimeScene != nullptr && state.World != nullptr)
			state.DisconnectSignals();
		// Characters first (their inner bodies live in the world), then the bodies, then the world; no events.
		state.Characters.clear();
		if (state.World != nullptr)
		{
			for (const auto& [handle, entry] : state.Bodies)
				state.World->DestroyBody(entry.Handle);
		}
		state.Bodies.clear();
		state.MeshShapes.Clear();
		state.World.reset();
	}

	Result<Scope<PhysicsSystem>> PhysicsSystem::Create(const PhysicsSystemSpecification& specification)
	{
		if (specification.RuntimeScene == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "RuntimeScene: a physics system needs the play session's runtime scene");
		if (specification.FixedHz == 0)
			return MakeError(ErrorCode::InvalidArgument, "FixedHz: the fixed rate must be at least 1 Hz (got 0)");
		if (const Status gravity = Utils::CheckVector(specification.Gravity, MaxPhysicsGravity, "gravity"); !gravity)
		{
			return std::unexpected(Error(gravity.error())
					.WithContext("in the project's Physics.Gravity")
					.WithHint(std::format("give each component of Physics.Gravity a magnitude of at most {} m/s^2", MaxPhysicsGravity)));
		}

		Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(specification.Layers, specification.Collisions);
		if (!layers)
			return std::unexpected(std::move(layers).error().WithContext("while reading the project's physics layers"));
		PhysicsWorldSpecification worldSpecification;
		worldSpecification.Gravity = specification.Gravity;
		worldSpecification.Layers = *layers;
		worldSpecification.Limits = specification.Limits;
		Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create(worldSpecification);
		if (!world)
			return std::unexpected(std::move(world).error().WithContext("while creating the physics world"));

		Scope<PhysicsSystem> system = CreateScope<PhysicsSystem>(ConstructionKey());
		State& state = *system->m_State;
		state.Specification = specification;
		state.Layers = std::move(*layers);
		state.World = std::move(*world);
		// §9.3 in integers: max(1, ceil(60 / FixedHz)).
		state.CollisionSteps = std::max<uint32_t>(1, (60 + specification.FixedHz - 1) / specification.FixedHz);
		state.ConnectSignals();

		// §5.6: every body and character in canonical order, so queries and physics.bodyInfo work at tick 0.
		std::set<UUID> woken;
		state.Rebuild(true, woken);
		state.RecordHierarchy();
		state.ComponentsChanged = false;
		state.ChangedEntities.clear();
		state.FlushCandidates.clear();
		state.PendingListenerDiagnostics.clear(); // no listener yet: GetDiagnostics holds them
		return system;
	}

	void PhysicsSystem::PreStep(const SimStep& step)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Dispatching, "PhysicsSystem::PreStep was called from a physics listener callback");
		Scene& scene = *state.Specification.RuntimeScene;
		entt::registry& registry = scene.GetRegistry();
		const auto deltaTime = static_cast<float>(step.FixedDelta);
		std::set<UUID> woken;

		// 1-2. Rebuilds, and the partners of destroyed bodies woken so the step reports what still touches.
		const bool hierarchyChanged = state.HasHierarchyChanged();
		if (state.ComponentsChanged || hierarchyChanged)
			state.Rebuild(hierarchyChanged, woken);
		else if (const std::set<UUID> changed = state.FindShapeChanges(false); !changed.empty())
			state.RefreshShapes(changed, woken);
		state.ComponentsChanged = false;
		state.ChangedEntities.clear();

		// 3-4. Kinematic moves and teleports, in canonical order of the owners; entities pending destruction or disabled keep
		// their bodies as they are until FlushDestroyed.
		const std::vector<UUID> order(scene.GetCanonicalOrder().begin(), scene.GetCanonicalOrder().end());
		for (const UUID id : order)
		{
			const Entity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || entity.HasComponent<HierarchyDisabledTag>())
				continue;
			for (const PhysicsBodyOrigin origin : Utils::BodyOrigins)
			{
				PhysicsBodyEntry* entry = state.FindOwnedBody(id, origin);
				if (entry == nullptr)
					continue;
				const glm::mat4 world = Utils::GetWorldMatrix(entity);
				switch (entry->Plan.MotionType)
				{
					case PhysicsMotionType::Kinematic:
					{
						const PhysicsPose target = Utils::GetWorldPose(entity, world);
						if (!IsPlaceablePhysicsPose(target))
						{
							// Not applied: the entity shows where the body is, and the body stops there (Jolt keeps a kinematic
							// body's last velocity otherwise, so it would drift on).
							const PhysicsPose current = state.World->GetPose(entry->Handle);
							static_cast<void>(Utils::WriteWorldPose(entity, current));
							state.World->MoveKinematic(entry->Handle, current, deltaTime);
							break;
						}
						if (registry.all_of<InterpolationResetTag>(entity.GetHandle()))
						{
							// Teleported in steps 1 to 4: placed, never swept, so what rests on it is not flung, and at rest
							// there, so the step does not carry it on with the velocity of its last sweep.
							state.World->SetPose(entry->Handle, target, true);
							state.World->SetLinearVelocity(entry->Handle, glm::vec3(0.0f));
							state.World->SetAngularVelocity(entry->Handle, glm::vec3(0.0f));
							state.CollectPartners(id, woken);
						}
						else
						{
							state.World->MoveKinematic(entry->Handle, target, deltaTime);
						}
						break;
					}
					case PhysicsMotionType::Dynamic:
					{
						const TransformComponent& local = entity.GetComponent<TransformComponent>();
						if (Utils::SameBits(local.Translation, entry->WrittenTranslation) && Utils::SameBits(local.Rotation, entry->WrittenRotation))
							break;
						const PhysicsPose target = Utils::GetWorldPose(entity, world);
						if (!IsPlaceablePhysicsPose(target))
						{
							// Not applied: the entity shows where the body is again.
							Utils::RestoreLocalPose(entity, entry->WrittenTranslation, entry->WrittenRotation);
							break;
						}
						state.World->SetPose(entry->Handle, target, true);
						entry->WrittenTranslation = local.Translation;
						entry->WrittenRotation = local.Rotation;
						state.CollectPartners(id, woken);
						break;
					}
					case PhysicsMotionType::Static:
					{
						if (Utils::SameBits(world, entry->PlacedMatrix))
							break;
						const PhysicsPose target = Utils::GetWorldPose(entity, world);
						if (!IsPlaceablePhysicsPose(target))
						{
							// Not applied: the entity's local pose goes back to the one the body was placed from.
							Utils::RestoreLocalPose(entity, entry->WrittenTranslation, entry->WrittenRotation);
							break;
						}
						// Activated: a static body cannot wake, so the world wakes what rested on it and what it now touches.
						state.World->SetPose(entry->Handle, target, true);
						const TransformComponent& local = entity.GetComponent<TransformComponent>();
						entry->WrittenTranslation = local.Translation;
						entry->WrittenRotation = local.Rotation;
						entry->PlacedMatrix = world;
						state.CollectPartners(id, woken);
						break;
					}
				}
			}
		}
		state.WakeOwners(woken);

		// 5. Characters: a Transform write teleports them, then each runs its update with the velocity of its last
		// MoveCharacter, which the update consumes.
		const glm::vec3 gravity = state.World->GetGravity();
		for (const UUID id : order)
		{
			const auto character = state.Characters.find(id);
			if (character == state.Characters.end())
				continue;
			const Entity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || entity.HasComponent<HierarchyDisabledTag>())
				continue;
			PhysicsCharacterEntry& entry = character->second;
			const TransformComponent& local = entity.GetComponent<TransformComponent>();
			if (!Utils::SameBits(local.Translation, entry.WrittenTranslation) || !Utils::SameBits(local.Rotation, entry.WrittenRotation))
			{
				if (const PhysicsPose target = Utils::GetWorldPose(entity, Utils::GetWorldMatrix(entity)); IsPlaceablePhysicsPose(target))
				{
					entry.Controller->SetPose(target);
					entry.WrittenTranslation = local.Translation;
					entry.WrittenRotation = local.Rotation;
				}
				else
				{
					// Not applied: the entity shows where the character is again.
					Utils::RestoreLocalPose(entity, entry.WrittenTranslation, entry.WrittenRotation);
				}
			}
			// A gravity factor so large that the product overflows is content the session cannot simulate: no gravity then
			// (the registry gives GravityFactor no maximum).
			glm::vec3 characterGravity = gravity * entry.GravityFactor;
			if (!Utils::IsFinite(characterGravity))
				characterGravity = glm::vec3(0.0f);
			entry.Controller->Update(deltaTime, entry.DesiredVelocity, characterGravity);
			entry.DesiredVelocity = glm::vec3(0.0f);
		}

		// Dormant contacts a body of which is awake now: the step will report them again if they still touch.
		state.EligibleDormantContacts.clear();
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

		state.DeliverPendingDiagnostics();
	}

	void PhysicsSystem::Step(const SimStep& step)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Dispatching, "PhysicsSystem::Step was called from a physics listener callback");
		const Status stepped = state.World->Step(static_cast<float>(step.FixedDelta), state.CollisionSteps);
		const PhysicsWorldStats stats = state.World->GetStats();
		const PhysicsWorldLimits& limits = state.World->GetLimits();
		if (!stepped)
		{
			// §9.1: a full cache drops contacts beyond it; the session keeps running.
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsLimitExceededCode);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Subject = stats.ContactConstraintCount >= limits.MaxContactConstraints ? "contactConstraints" : "bodyPairs";
			diagnostic.Message = std::format("the physics step at tick {} overflowed: {}", step.Tick,
				Utils::StripCode(stepped.error().GetMessageText(), PhysicsLimitExceededCode));
			diagnostic.Hint = "fewer bodies touching at once, or fewer bodies in the scene";
			state.RaiseDiagnostic(std::move(diagnostic));
		}

		// §9.1 "approaching a limit": a warning when a count first reaches LimitWarningFraction of its limit. The world has
		// logged it already (PhysicsWorldLimits), so the diagnostic is only recorded and heard by the listener.
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
			diagnostic.Message = std::format("the physics world holds {} {} of at most {}", count, subject, limit);
			diagnostic.Hint = "merge colliders into compounds or remove bodies before the limit is reached";
			state.RaiseDiagnostic(std::move(diagnostic), true);
		}
		state.DeliverPendingDiagnostics();
	}

	void PhysicsSystem::PostStep(const SimStep& step)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Dispatching, "PhysicsSystem::PostStep was called from a physics listener callback");
		Scene& scene = *state.Specification.RuntimeScene;
		state.LastEvents.clear();

		// Write-back (§9.3), in canonical order: Dynamic bodies that are awake or have a parent, then characters.
		const std::vector<UUID> order(scene.GetCanonicalOrder().begin(), scene.GetCanonicalOrder().end());
		for (const UUID id : order)
		{
			const Entity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || entity.HasComponent<HierarchyDisabledTag>())
				continue;
			if (PhysicsBodyEntry* entry = state.FindOwnedBody(id, PhysicsBodyOrigin::RigidBody);
				entry != nullptr && entry->Plan.MotionType == PhysicsMotionType::Dynamic
				&& (!state.World->IsSleeping(entry->Handle) || entity.GetParent().IsValid()))
			{
				std::tie(entry->WrittenTranslation, entry->WrittenRotation) = Utils::WriteWorldPose(entity, state.World->GetPose(entry->Handle));
			}
			if (const auto character = state.Characters.find(id); character != state.Characters.end())
			{
				PhysicsCharacterEntry& entry = character->second;
				std::tie(entry.WrittenTranslation, entry.WrittenRotation) = Utils::WriteWorldPose(entity, entry.Controller->GetPose());
			}
		}

		// §9.4 steps 1 to 4: map, collapse into pairs, sort, dispatch.
		const auto resolve = [&state](BodyHandle handle, uint32_t collider) -> std::optional<ResolvedBody>
		{
			if (const auto body = state.Bodies.find(handle.GetValue()); body != state.Bodies.end())
			{
				const PhysicsBodyEntry& entry = body->second;
				const UUID entity = collider < entry.Colliders.size() ? entry.Colliders[collider] : entry.Owner;
				return ResolvedBody{ .Owner = entry.Owner, .Collider = entity };
			}
			if (const auto inner = state.InnerBodies.find(handle.GetValue()); inner != state.InnerBodies.end())
				return ResolvedBody{ .Owner = inner->second, .Collider = inner->second };
			return std::nullopt;
		};

		// Each pair as the step found it (captured before anything changes it), and the first touching contact of each pair
		// the step's records touched.
		std::map<PhysicsPairKey, PairBefore> before;
		std::map<PhysicsPairKey, FirstContact> firstContacts;
		const auto touch = [&state, &before](const PhysicsPairKey& key) -> PhysicsPair&
		{
			PhysicsPair& pair = state.Pairs[key];
			before.try_emplace(key, PairBefore{ .Contacts = pair.Contacts.size(), .Touched = pair.Touched });
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
		const auto offerFirstContact = [&firstContacts](const PhysicsPairKey& key, const FirstContact& candidate)
		{
			if (const auto [first, added] = firstContacts.try_emplace(key, candidate); !added && candidate < first->second)
				first->second = candidate;
		};
		// M20 (decision 2): whether an entity is a scripted gathered collider, decided once per entity and PostStep.
		std::map<UUID, bool> scriptedColliders;
		const auto isScriptedCollider = [&state, &scriptedColliders](UUID entity)
		{
			const auto [found, added] = scriptedColliders.try_emplace(entity, false);
			if (added)
				found->second = state.IsScriptedGatheredCollider(entity);
			return found->second;
		};
		// The collider pairs a contact counts in by its sides: one for each side whose collider is a scripted gathered collider
		// (and is not the partner's owner itself).
		const auto forEachColliderPair = [&isScriptedCollider](const PhysicsSubShapeContact& contact, const auto& visit)
		{
			for (size_t side = 0; side < 2; ++side)
			{
				const UUID collider = contact.Colliders[side];
				const UUID partner = contact.Owners[1 - side];
				if (!collider.IsValid() || collider == contact.Owners[side] || collider == partner || !isScriptedCollider(collider))
					continue;
				visit(Utils::MakeColliderPairKey(collider, partner, contact.Pair.Kind));
			}
		};
		// A touching record's contact is a candidate for the enter contact of its body pair and of its collider pairs.
		const auto offerTouchingContact = [&offerFirstContact, &forEachColliderPair](const PhysicsSubShapeKey& key, const PhysicsSubShapeContact& contact)
		{
			offerFirstContact(contact.Pair, Utils::MakeFirstContact(key, contact, Utils::GetLowSide(contact.Pair, contact)));
			forEachColliderPair(contact, [&offerFirstContact, &key, &contact](const PhysicsPairKey& pair)
			{
				offerFirstContact(pair, Utils::MakeFirstContact(key, contact, Utils::GetLowSide(pair, contact)));
			});
		};
		// The state of a contact an index names (verified: going on with the indexes out of step would corrupt the pairs).
		const auto findContact = [&state](const PhysicsSubShapeKey& key) -> PhysicsSubShapeContact*
		{
			const auto found = state.Contacts.find(key);
			ENGINE_CORE_VERIFY(found != state.Contacts.end(), "PhysicsSystem: an index names a contact that does not exist (indexes and Contacts out of step)");
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
			const PhysicsSubShapeKey contact = Utils::MakeSubShapeKey(record);
			if (record.Kind == ContactEventKind::Removed)
			{
				const auto found = state.Contacts.find(contact);
				if (found == state.Contacts.end())
					continue; // a contact of a body already destroyed, or one never recorded
				touchContact(contact);
				const auto pair = state.Pairs.find(found->second.Pair);
				ENGINE_CORE_VERIFY(pair != state.Pairs.end() && pair->second.Contacts.contains(contact),
					"PhysicsSystem: a contact's pair does not hold it (Contacts and Pairs out of step)");
				const BodyHandle a(contact.BodyA);
				const BodyHandle b(contact.BodyB);
				const bool asleep = !record.FromCharacter && state.World->IsBodyValid(a) && state.World->IsBodyValid(b) && state.World->IsSleeping(a)
					&& state.World->IsSleeping(b);
				if (asleep)
					found->second.Dormant = true;
				else
					state.EraseContact(contact);
				continue;
			}

			const std::optional<ResolvedBody> a = resolve(record.BodyA, record.ColliderA);
			const std::optional<ResolvedBody> b = resolve(record.BodyB, record.ColliderB);
			if (!a.has_value() || !b.has_value() || a->Owner == b->Owner)
				continue;
			// The record's sides in the contact's order (its lower body handle first), with its normal from the first to the
			// second.
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
				// A speculative contact touches now (decision 3): its pairs may enter.
				const auto found = state.Contacts.find(contact);
				if (found == state.Contacts.end() || found->second.Owners != owners)
					continue; // a contact the system never recorded (an owner it does not know), or one of other bodies
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
				// The same sub-shape pair now belongs to another pair of owners (a body rebuilt in place): move it.
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
			// A contact Jolt adds again (after its bodies slept) keeps having touched; one that touched now has.
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
				"PhysicsSystem: a contact's pair does not hold it (Contacts and Pairs out of step)");
			if (found->second.Dormant)
				state.EraseContact(contact);
		}

		// M20 (decision 2): the contacts of bodies whose shape PreStep replaced in place may name other colliders now; resolve
		// the colliders of their sub-shapes again.
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
					if (const std::optional<ResolvedBody> resolved = resolve(body, state.World->GetSubShapeCollider(body, sides[side].second)))
						colliders[side] = resolved->Collider;
				}
				if (colliders == contactState->Colliders)
					continue;
				touchContact(contact);
				state.SetContactSides(contact, *contactState, contactState->Owners, colliders);
			}
		}

		// M20 (decision 2): the collider pairs. A scripted gathered collider's contacts count in its collider pairs; a collider
		// that is not one (any more) counts in none, and when it is still present, its active collider pairs end with
		// synthesized exits.
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
				const std::optional<size_t> side = contactState != nullptr ? Utils::FindGatheredSide(*contactState, collider) : std::nullopt;
				const UUID partner = side.has_value() ? contactState->Owners[1 - *side] : UUID();
				if (!side.has_value() || partner == collider)
					continue;
				const PhysicsPairKey key = Utils::MakeColliderPairKey(collider, partner, contactState->Pair.Kind);
				if (std::binary_search(contactState->ColliderPairs.begin(), contactState->ColliderPairs.end(), key))
					continue;
				PhysicsPair& pair = touch(key);
				pair.ColliderBody = contactState->Owners[*side];
				state.LinkColliderPair(contact, *contactState, key);
				if (contactState->Touching)
					pair.Touched = true;
			}
		}

		// Transitions over the step: an inactive pair enters when it has touched since the step began (decision 3: a contact
		// added without touching cannot start it), a count that fell to zero exits; an active pair left without contacts
		// otherwise (a body PreStep destroyed and nothing re-established, or a collider that left) ends with a synthesized
		// exit.
		std::vector<PendingPairEvent> events;
		std::vector<PhysicsPairKey> finished;
		for (auto& [key, pair] : state.Pairs)
		{
			const size_t after = pair.Contacts.size();
			const auto counted = before.find(key);
			const PairBefore previous = counted != before.end() ? counted->second : PairBefore{ .Contacts = after, .Touched = pair.Touched };
			if (after > 0)
			{
				pair.Unconfirmed = false;
				if (!pair.Active && pair.Touched && !previous.Touched)
				{
					PendingPairEvent event;
					event.Key = key;
					event.Type = Utils::GetEnterType(key.Kind);
					event.ColliderBody = pair.ColliderBody;
					auto first = firstContacts.find(key);
					if (first == firstContacts.end())
					{
						// No touching record of the step (a collider pair that its collider's script or a resolution started):
						// the first of its touching contacts.
						for (const PhysicsSubShapeKey& contact : pair.Contacts)
						{
							const PhysicsSubShapeContact* contactState = findContact(contact);
							if (contactState != nullptr && contactState->Touching)
								offerFirstContact(key, Utils::MakeFirstContact(contact, *contactState, Utils::GetLowSide(key, *contactState)));
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
			PendingPairEvent event;
			event.Key = key;
			event.Type = Utils::GetExitType(key.Kind);
			event.Synthesized = pair.Unconfirmed || previous.Contacts == 0;
			event.ColliderBody = pair.ColliderBody;
			events.push_back(event);
		}
		for (const PhysicsPairKey& key : finished)
			state.ErasePair(key);

		// The canonical order (PhysicsEventOrderKey): a total order on the step's events, since each pair has one event.
		std::sort(events.begin(), events.end(), [](const PendingPairEvent& a, const PendingPairEvent& b)
		{
			return Utils::GetPairOrderKey(a.Key, a.Type, a.ColliderBody) < Utils::GetPairOrderKey(b.Key, b.Type, b.ColliderBody);
		});

		// Dispatch (§9.4 steps 3 and 4; the drop rules of IPhysicsEventListener). A body event goes to Low, then High; a
		// collider event to its collider alone, with SelfBody its body's owner.
		std::vector<bool> blocked(events.size());
		for (size_t index = 0; index < events.size(); ++index)
			blocked[index] = !state.IsPresent(events[index].Key.Low) || !state.IsPresent(events[index].Key.High);
		const auto deliver = [&state, &step](PhysicsEventType type, UUID self, UUID other, UUID selfBody, const PhysicsContact& contact, bool synthesized)
		{
			const PhysicsEvent event{ .Type = type, .Self = self, .Other = other, .Contact = contact, .Synthesized = synthesized, .Tick = step.Tick, .SelfBody = selfBody };
			state.LastEvents.push_back(event);
			if (state.Listener != nullptr)
				state.Listener->OnPhysicsEvent(event);
		};
		state.Dispatching = true;
		for (size_t index = 0; index < events.size(); ++index)
		{
			const PendingPairEvent& event = events[index];
			const auto found = state.Pairs.find(event.Key);
			if (found == state.Pairs.end() || blocked[index])
				continue; // an enter of a pair with a gone entity never becomes active; an exit is left to FlushDestroyed
			PhysicsPair& pair = found->second;
			const UUID low = event.Key.Low;
			const UUID high = event.Key.High;
			const bool isColliderPair = event.Key.IsColliderPair();
			const bool lowReceives = !isColliderPair || event.Key.Collider == low;
			const bool highReceives = !isColliderPair || event.Key.Collider == high;
			const UUID lowBody = isColliderPair && lowReceives ? pair.ColliderBody : low;
			const UUID highBody = isColliderPair && highReceives ? pair.ColliderBody : high;
			if (Utils::IsEnter(event.Type))
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
				// Callbacks cannot erase pairs (only the step hooks do, and they are not reentrant), so `pair` stays valid.
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

	void PhysicsSystem::FlushDestroyed(uint64_t tick)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Dispatching, "PhysicsSystem::FlushDestroyed was called from a physics listener callback");
		std::set<UUID> handled;
		while (true)
		{
			// The entities that left play since the last pass and still have a body, a character or a pair.
			std::vector<UUID> gone;
			const std::set<UUID> candidates = std::move(state.FlushCandidates);
			state.FlushCandidates.clear();
			for (const UUID candidate : candidates)
			{
				if (handled.contains(candidate) || state.IsPresent(candidate))
					continue;
				handled.insert(candidate);
				const bool ownsBody = std::any_of(Utils::BodyOrigins.begin(), Utils::BodyOrigins.end(), [&state, candidate](PhysicsBodyOrigin origin)
				{
					return state.FindOwnedBody(candidate, origin) != nullptr;
				});
				if (ownsBody || state.Characters.contains(candidate) || state.PairsByOwner.contains(candidate))
					gone.push_back(candidate);
			}
			if (gone.empty())
				break;

			// Close their pairs: a synthesized exit for each side that got the enter and is still here, in the canonical order
			// (PhysicsEventOrderKey; a collider pair's to its collider alone).
			std::set<PhysicsPairKey> closing;
			std::set<UUID> woken;
			for (const UUID owner : gone)
			{
				if (const auto owned = state.PairsByOwner.find(owner); owned != state.PairsByOwner.end())
					closing.insert(owned->second.begin(), owned->second.end());
				state.CollectPartners(owner, woken);
			}
			std::vector<ClosingPair> exits;
			for (const PhysicsPairKey& key : closing)
			{
				const auto found = state.Pairs.find(key);
				ENGINE_CORE_VERIFY(found != state.Pairs.end(), "PhysicsSystem: an owner's pair does not exist (PairsByOwner and Pairs out of step)");
				if (found != state.Pairs.end() && found->second.Active)
				{
					const PhysicsPair& pair = found->second;
					exits.push_back(ClosingPair{ .Key = key,
						.ToLow = pair.EnteredLow,
						.ToHigh = pair.EnteredHigh,
						.LowCollider = pair.LowCollider,
						.HighCollider = pair.HighCollider,
						.ColliderBody = pair.ColliderBody });
				}
				state.ErasePair(key);
			}
			std::sort(exits.begin(), exits.end(), [](const ClosingPair& a, const ClosingPair& b)
			{
				return Utils::GetPairOrderKey(a.Key, Utils::GetExitType(a.Key.Kind), a.ColliderBody)
					< Utils::GetPairOrderKey(b.Key, Utils::GetExitType(b.Key.Kind), b.ColliderBody);
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
					state.Listener->OnPhysicsEvent(event);
			};
			state.Dispatching = true;
			for (const ClosingPair& ending : exits)
			{
				const PhysicsEventType type = Utils::GetExitType(ending.Key.Kind);
				const bool isColliderPair = ending.Key.IsColliderPair();
				const UUID lowBody = isColliderPair && ending.Key.Collider == ending.Key.Low ? ending.ColliderBody : ending.Key.Low;
				const UUID highBody = isColliderPair && ending.Key.Collider == ending.Key.High ? ending.ColliderBody : ending.Key.High;
				if (ending.ToLow && state.IsPresent(ending.Key.Low))
					deliver(type, ending.Key.Low, ending.Key.High, lowBody, ending.LowCollider, ending.HighCollider);
				if (ending.ToHigh && state.IsPresent(ending.Key.High))
					deliver(type, ending.Key.High, ending.Key.Low, highBody, ending.HighCollider, ending.LowCollider);
			}
			state.Dispatching = false;

			// Then their bodies and characters; what rested on them wakes (a re-enabled entity gets its body back at the next
			// PreStep).
			for (const UUID owner : gone)
			{
				for (const PhysicsBodyOrigin origin : Utils::BodyOrigins)
				{
					if (const PhysicsBodyEntry* entry = state.FindOwnedBody(owner, origin))
						state.RemoveBody(entry->Handle.GetValue(), woken);
				}
				state.RemoveCharacter(owner, woken);
			}
			for (const UUID owner : gone)
				woken.erase(owner);
			state.WakeOwners(woken);
		}
	}

	void PhysicsSystem::SetEventListener(IPhysicsEventListener* listener)
	{
		ENGINE_CORE_ASSERT(!m_State->Dispatching, "PhysicsSystem::SetEventListener was called from a physics listener callback");
		m_State->Listener = listener;
	}

	std::span<const PhysicsEvent> PhysicsSystem::GetLastEvents() const
	{
		return m_State->LastEvents;
	}

	Status PhysicsSystem::AddForce(UUID entity, const glm::vec3& force)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::CheckVector(force, MaxVectorMagnitude, "force"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::MakeMotionTypeError(entity, entry->Plan.MotionType, "AddForce", "Dynamic");
		m_State->World->AddForce(entry->Handle, force);
		return {};
	}

	Status PhysicsSystem::AddForceAtPosition(UUID entity, const glm::vec3& force, const glm::vec3& position)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::CheckVector(force, MaxVectorMagnitude, "force"));
		ENGINE_TRY(Utils::CheckVector(position, MaxPhysicsCoordinate, "position"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::MakeMotionTypeError(entity, entry->Plan.MotionType, "AddForceAtPosition", "Dynamic");
		m_State->World->AddForceAtPosition(entry->Handle, force, position);
		return {};
	}

	Status PhysicsSystem::AddTorque(UUID entity, const glm::vec3& torque)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::CheckVector(torque, MaxVectorMagnitude, "torque"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::MakeMotionTypeError(entity, entry->Plan.MotionType, "AddTorque", "Dynamic");
		m_State->World->AddTorque(entry->Handle, torque);
		return {};
	}

	Status PhysicsSystem::AddImpulse(UUID entity, const glm::vec3& impulse)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::CheckVector(impulse, MaxVectorMagnitude, "impulse"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::MakeMotionTypeError(entity, entry->Plan.MotionType, "AddImpulse", "Dynamic");
		m_State->World->AddImpulse(entry->Handle, impulse);
		return {};
	}

	Status PhysicsSystem::AddAngularImpulse(UUID entity, const glm::vec3& angularImpulse)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::CheckVector(angularImpulse, MaxVectorMagnitude, "angular impulse"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::MakeMotionTypeError(entity, entry->Plan.MotionType, "AddAngularImpulse", "Dynamic");
		m_State->World->AddAngularImpulse(entry->Handle, angularImpulse);
		return {};
	}

	Result<glm::vec3> PhysicsSystem::GetLinearVelocity(UUID entity) const
	{
		ENGINE_TRY_ASSIGN(const PhysicsBodyEntry* entry, m_State->FindRigidBody(entity));
		return m_State->World->GetLinearVelocity(entry->Handle);
	}

	Status PhysicsSystem::SetLinearVelocity(UUID entity, const glm::vec3& velocity)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::CheckVector(velocity, MaxVectorMagnitude, "velocity"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::MakeVelocityError(entity, entry->Plan.MotionType, "SetLinearVelocity");
		m_State->World->SetLinearVelocity(entry->Handle, velocity);
		return {};
	}

	Result<glm::vec3> PhysicsSystem::GetAngularVelocity(UUID entity) const
	{
		ENGINE_TRY_ASSIGN(const PhysicsBodyEntry* entry, m_State->FindRigidBody(entity));
		return m_State->World->GetAngularVelocity(entry->Handle);
	}

	Status PhysicsSystem::SetAngularVelocity(UUID entity, const glm::vec3& velocity)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::CheckVector(velocity, MaxVectorMagnitude, "angular velocity"));
		if (entry->Plan.MotionType != PhysicsMotionType::Dynamic)
			return Utils::MakeVelocityError(entity, entry->Plan.MotionType, "SetAngularVelocity");
		m_State->World->SetAngularVelocity(entry->Handle, velocity);
		return {};
	}

	Status PhysicsSystem::MoveKinematic(UUID entity, const glm::vec3& position, const glm::quat& rotation)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		ENGINE_TRY(Utils::CheckVector(position, MaxPhysicsCoordinate, "position"));
		ENGINE_TRY_ASSIGN(const glm::quat unit, Utils::CheckRotation(rotation));
		if (entry->Plan.MotionType != PhysicsMotionType::Kinematic)
			return Utils::MakeMotionTypeError(entity, entry->Plan.MotionType, "MoveKinematic", "Kinematic");
		// The entity's Transform is the single source of a kinematic body's pose: the next PreStep sweeps the body there.
		const PhysicsPose target{ .Position = position, .Rotation = unit };
		static_cast<void>(Utils::WriteWorldPose(m_State->Specification.RuntimeScene->FindEntityByID(entity), target));
		return {};
	}

	Status PhysicsSystem::Teleport(UUID entity, const glm::vec3& position, std::optional<glm::quat> rotation)
	{
		State& state = *m_State;
		Scene& scene = *state.Specification.RuntimeScene;
		const bool ownsBody = std::any_of(Utils::BodyOrigins.begin(), Utils::BodyOrigins.end(), [&state, entity](PhysicsBodyOrigin origin)
		{
			return state.FindOwnedBody(entity, origin) != nullptr;
		});
		if ((!ownsBody && !state.Characters.contains(entity)) || !state.IsPresent(entity))
			return Utils::MakeNoBodyError(entity);
		ENGINE_TRY(Utils::CheckVector(position, MaxPhysicsCoordinate, "position"));
		const Entity target = scene.FindEntityByID(entity);
		glm::quat unit = TransformSystem::GetWorldRotation(target);
		if (rotation.has_value())
		{
			ENGINE_TRY_ASSIGN(const glm::quat checked, Utils::CheckRotation(*rotation));
			unit = checked;
		}

		// The pose the Transform write gives, through the parent and in float, may round past the range the requested one
		// was within: it is checked before anything is applied, and the Transform is put back when it fails.
		TransformComponent& transform = target.GetComponent<TransformComponent>();
		const glm::vec3 previousTranslation = transform.Translation;
		const glm::quat previousRotation = transform.Rotation;
		const auto [translation, local] = Utils::WriteWorldPose(target, PhysicsPose{ .Position = position, .Rotation = unit });
		const glm::mat4 world = TransformSystem::ComputeWorldMatrix(target);
		const PhysicsPose placed = Utils::GetWorldPose(target, world);
		if (!IsPlaceablePhysicsPose(placed))
		{
			Utils::RestoreLocalPose(target, previousTranslation, previousRotation);
			return MakeError(ErrorCode::InvalidArgument,
				"the position ({}, {}, {}) is out of range under the entity's parent: each coordinate of the world position must be at most {} m",
				position.x, position.y, position.z, MaxPhysicsCoordinate);
		}

		scene.GetRegistry().emplace_or_replace<InterpolationResetTag>(target.GetHandle());
		std::set<UUID> partners;
		state.CollectPartners(entity, partners);
		for (const PhysicsBodyOrigin origin : Utils::BodyOrigins)
		{
			PhysicsBodyEntry* entry = state.FindOwnedBody(entity, origin);
			if (entry == nullptr)
				continue;
			// Activated: a Dynamic body wakes, and a Static one wakes what rested on it and what it now touches.
			state.World->SetPose(entry->Handle, placed, true);
			entry->WrittenTranslation = translation;
			entry->WrittenRotation = local;
			entry->PlacedMatrix = world;
		}
		if (const auto character = state.Characters.find(entity); character != state.Characters.end())
		{
			character->second.Controller->SetPose(placed);
			character->second.WrittenTranslation = translation;
			character->second.WrittenRotation = local;
		}
		state.WakeOwners(partners);
		return {};
	}

	Result<bool> PhysicsSystem::IsSleeping(UUID entity) const
	{
		ENGINE_TRY_ASSIGN(const PhysicsBodyEntry* entry, m_State->FindRigidBody(entity));
		return m_State->World->IsSleeping(entry->Handle);
	}

	Status PhysicsSystem::WakeUp(UUID entity)
	{
		ENGINE_TRY_ASSIGN(PhysicsBodyEntry * entry, m_State->FindRigidBody(entity));
		if (entry->Plan.MotionType != PhysicsMotionType::Static)
			m_State->World->WakeUp(entry->Handle);
		return {};
	}

	const PhysicsLayerTable& PhysicsSystem::GetLayers() const
	{
		return m_State->Layers;
	}

	glm::vec3 PhysicsSystem::GetGravity() const
	{
		return m_State->World->GetGravity();
	}

	Status PhysicsSystem::SetGravity(const glm::vec3& gravity)
	{
		ENGINE_TRY(Utils::CheckVector(gravity, MaxPhysicsGravity, "gravity"));
		return m_State->World->SetGravity(gravity);
	}

	std::optional<PhysicsBodyReport> PhysicsSystem::GetBodyInfo(UUID entity) const
	{
		const State& state = *m_State;
		// An entity pending destruction or disabled has no body for scripts and automation, as for the body functions.
		if (!state.IsPresent(entity))
			return std::nullopt;
		const PhysicsBodyEntry* entry = state.FindOwnedBody(entity, PhysicsBodyOrigin::RigidBody);
		const PhysicsCharacterEntry* character = nullptr;
		if (entry == nullptr)
		{
			if (const auto found = state.Characters.find(entity); found != state.Characters.end())
				character = &found->second;
		}
		if (entry == nullptr && character == nullptr)
			entry = state.FindOwnedBody(entity, PhysicsBodyOrigin::ImplicitStatic);
		if (entry == nullptr && character == nullptr)
			entry = state.FindOwnedBody(entity, PhysicsBodyOrigin::ImplicitSensor);
		if (entry == nullptr && character == nullptr)
		{
			if (const auto gathered = state.GatheredColliders.find(entity); gathered != state.GatheredColliders.end())
				entry = state.FindBody(gathered->second);
		}
		if ((entry == nullptr && character == nullptr) || (entry != nullptr && !state.IsPresent(entry->Owner)))
			return std::nullopt;

		PhysicsBodyReport report;
		report.Owner = entry != nullptr ? entry->Owner : character->Owner;
		if (entry != nullptr)
		{
			report.Origin = entry->Origin;
			report.MotionType = entry->Plan.MotionType;
			report.IsSensor = entry->Plan.IsSensor;
			report.Layer = entry->Plan.Layer;
			report.Colliders = entry->Colliders;
			report.Pose = state.World->GetPose(entry->Handle);
			report.LinearVelocity = state.World->GetLinearVelocity(entry->Handle);
			report.AngularVelocity = state.World->GetAngularVelocity(entry->Handle);
			report.IsSleeping = state.World->IsSleeping(entry->Handle);
			report.Bounds = state.World->GetBodyBounds(entry->Handle).value_or(Aabb{});
		}
		else
		{
			const CharacterGroundState ground = character->Controller->GetGroundState();
			report.Origin = PhysicsBodyOrigin::Character;
			report.MotionType = PhysicsMotionType::Kinematic;
			report.Layer = character->Description.Layer;
			report.Pose = character->Controller->GetPose();
			report.LinearVelocity = character->Controller->GetVelocity();
			report.Bounds = state.World->GetBodyBounds(character->Controller->GetInnerBody()).value_or(Aabb{});
			report.Character = PhysicsCharacterState{ .IsGrounded = ground.IsGrounded, .GroundNormal = ground.GroundNormal, .Velocity = report.LinearVelocity };
		}
		report.LayerName = report.Layer < state.Layers.GetLayerCount() ? state.Layers.GetName(report.Layer) : std::string();
		if (const auto owned = state.PairsByOwner.find(report.Owner); owned != state.PairsByOwner.end())
		{
			for (const PhysicsPairKey& key : owned->second)
			{
				const auto found = state.Pairs.find(key);
				ENGINE_CORE_VERIFY(found != state.Pairs.end(), "PhysicsSystem: an owner's pair does not exist (PairsByOwner and Pairs out of step)");
				// The body's pairs: a collider pair is its collider's (M20), not the body's.
				if (found == state.Pairs.end() || !found->second.Active || key.IsColliderPair())
					continue;
				const PhysicsPair& pair = found->second;
				const bool low = key.Low == report.Owner;
				report.Contacts.push_back(PhysicsContactInfo{ .Other = low ? key.High : key.Low,
					.Collider = low ? pair.LowCollider : pair.HighCollider,
					.OtherCollider = low ? pair.HighCollider : pair.LowCollider,
					.IsTrigger = key.Kind == PhysicsPairKind::Trigger,
					.SinceTick = pair.SinceTick });
			}
		}
		std::sort(report.Contacts.begin(), report.Contacts.end(), [](const PhysicsContactInfo& a, const PhysicsContactInfo& b)
		{
			return std::tie(a.Other, a.IsTrigger) < std::tie(b.Other, b.IsTrigger);
		});
		return report;
	}

	std::span<const PhysicsDiagnostic> PhysicsSystem::GetDiagnostics() const
	{
		return m_State->Diagnostics;
	}

	PhysicsSystemStats PhysicsSystem::GetStats() const
	{
		const State& state = *m_State;
		const PhysicsWorldStats world = state.World->GetStats();
		PhysicsSystemStats stats;
		stats.BodyCount = world.BodyCount;
		stats.ActiveBodyCount = world.ActiveBodyCount;
		stats.CharacterCount = static_cast<uint32_t>(state.Characters.size());
		stats.ContactPairCount = static_cast<uint32_t>(std::count_if(state.Pairs.begin(), state.Pairs.end(), [](const auto& pair)
		{
			return pair.second.Active && !pair.first.IsColliderPair();
		}));
		return stats;
	}

	void PhysicsSystem::AppendStateHash(XXH64Hasher& hasher) const
	{
		const State& state = *m_State;
		if (state.Bodies.empty() && state.Characters.empty())
			return;

		const Scene& scene = *state.Specification.RuntimeScene;
		std::vector<std::byte> bytes;
		const auto append = [&bytes](UUID owner, const PhysicsPose& pose, const glm::vec3& linear, const glm::vec3& angular, bool sleeping)
		{
			const uint64_t value = owner.GetValue();
			for (uint32_t shift = 0; shift < 64; shift += 8)
				bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
			Utils::AppendVector(bytes, pose.Position);
			Utils::AppendFloat(bytes, pose.Rotation.x);
			Utils::AppendFloat(bytes, pose.Rotation.y);
			Utils::AppendFloat(bytes, pose.Rotation.z);
			Utils::AppendFloat(bytes, pose.Rotation.w);
			Utils::AppendVector(bytes, linear);
			Utils::AppendVector(bytes, angular);
			bytes.push_back(static_cast<std::byte>(sleeping ? 1 : 0));
		};
		for (const UUID id : scene.GetCanonicalOrder())
		{
			for (const PhysicsBodyOrigin origin : Utils::BodyOrigins)
			{
				const PhysicsBodyEntry* entry = state.FindOwnedBody(id, origin);
				if (entry == nullptr)
					continue;
				append(id, state.World->GetPose(entry->Handle), state.World->GetLinearVelocity(entry->Handle), state.World->GetAngularVelocity(entry->Handle),
					state.World->IsSleeping(entry->Handle));
			}
			if (const auto character = state.Characters.find(id); character != state.Characters.end())
			{
				const CharacterController& controller = *character->second.Controller;
				append(id, controller.GetPose(), controller.GetVelocity(), glm::vec3(0.0f), false);
			}
		}
		hasher.Update(bytes);
	}

	PhysicsEventOrderKey GetPhysicsEventOrderKey(const PhysicsEvent& event)
	{
		const UUID selfBody = event.SelfBody.IsValid() ? event.SelfBody : event.Self;
		const bool selfIsLow = selfBody < event.Other;
		return PhysicsEventOrderKey{ .Low = selfIsLow ? selfBody : event.Other,
			.High = selfIsLow ? event.Other : selfBody,
			.Type = event.Type,
			.Collider = event.IsColliderEvent() ? event.Self : UUID() };
	}

	std::string_view PhysicsEventTypeToString(PhysicsEventType type)
	{
		switch (type)
		{
			case PhysicsEventType::CollisionEnter: return "CollisionEnter";
			case PhysicsEventType::CollisionExit:  return "CollisionExit";
			case PhysicsEventType::TriggerEnter:   return "TriggerEnter";
			case PhysicsEventType::TriggerExit:    return "TriggerExit";
		}

		ENGINE_CORE_ASSERT(false, "Unknown PhysicsEventType {}", std::to_underlying(type));
		return "Unknown";
	}

}
