#include "EnginePCH.h"
#include "Engine/Scene/PhysicsValidation.h"

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// The edit-time physics checks (Docs/Decisions/0014-m11-decisions.md decision 15): the composition's diagnostics, the
// shapes Jolt refuses, the Dynamic bodies a shape cannot carry (PhysicsShape::CheckDynamicBody: its Mass, an inertia too
// large) and seams between implicit static bodies. The shapes are built exactly as the play session builds them
// (DescribePhysicsBodyShape, then PhysicsShape::Create), so a body this reports as invalid is one the session refuses.

namespace Engine {

	namespace {

		// The world bounds of one implicit static body, with its owner's position in canonical order (the composition's).
		struct StaticBodyBounds
		{
			UUID Owner{};
			size_t Order = 0;
			Aabb Bounds{};
		};

	}

	namespace Utils {

		// The entity's path for messages, or its id when the scene does not hold it.
		static std::string DescribeEntity(const Scene& scene, UUID id)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			return entity.IsValid() ? scene.GetEntityPath(entity) : id.ToString();
		}

		// PHYSICS_INVALID_SHAPE (or the physics code the error carries) for a body whose shape cannot be built: on the
		// collider entity the error names (its location's entity, or the collider index of PhysicsShape::Create's message),
		// with that collider's component when it is known, else on the owner.
		static PhysicsDiagnostic MakeShapeDiagnostic(const Scene& scene, const PhysicsBodyPlan& plan, const Error& error)
		{
			const std::string_view carried = GetPhysicsDiagnosticCode(error);
			const std::string_view code = carried.empty() ? PhysicsInvalidShapeCode : carried;
			const std::string_view text = error.GetMessageText();
			const std::string_view reason = carried.empty() ? text : text.substr(carried.size() + 2);

			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(code);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Entity = plan.Owner;
			const UUID located = error.GetLocation().Entity;
			const std::optional<uint32_t> index = ParseColliderIndex(reason);
			if (index.has_value() && *index < plan.Colliders.size())
			{
				diagnostic.Entity = plan.Colliders[*index].Entity;
				diagnostic.Component = std::string(GetColliderComponentName(plan.Colliders[*index].Type));
			}
			else if (located.IsValid())
			{
				diagnostic.Entity = located;
				// The component, when the entity holds colliders of one type in this body.
				std::set<PhysicsColliderType> types;
				for (const PhysicsColliderPlan& collider : plan.Colliders)
				{
					if (collider.Entity == located)
						types.insert(collider.Type);
				}
				if (types.size() == 1)
					diagnostic.Component = std::string(GetColliderComponentName(*types.begin()));
			}
			diagnostic.Message = std::format("the shape of the body of '{}' cannot be built: {}", DescribeEntity(scene, plan.Owner), reason);
			diagnostic.Hint = !error.GetHint().empty()
				? error.GetHint()
				: std::string("correct the collider's dimensions, scale or mesh; the play session does not create the body while its shape is refused");
			return diagnostic;
		}

		// The check of a Dynamic RigidBody against its built shape (PhysicsShape::CheckDynamicBody, which PhysicsWorld applies
		// when it creates the body): a Mass the world refuses, or an inertia too large for Jolt, on the RigidBody's Mass (its
		// LockTranslation when the body cannot move).
		static std::optional<PhysicsDiagnostic> CheckDynamicBody(const Scene& scene, const PhysicsBodyPlan& plan, const PhysicsShape& shape)
		{
			if (plan.Origin != PhysicsBodyOrigin::RigidBody || plan.MotionType != PhysicsMotionType::Dynamic)
				return std::nullopt;
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			const RigidBodyComponent* body = owner.IsValid() ? owner.TryGetComponent<RigidBodyComponent>() : nullptr;
			if (body == nullptr)
				return std::nullopt;
			const Status checked = shape.CheckDynamicBody(body->Mass, GetRigidBodyDofs(*body));
			if (checked.has_value())
				return std::nullopt;

			const std::string_view carried = GetPhysicsDiagnosticCode(checked.error());
			const std::string_view text = checked.error().GetMessageText();
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(carried.empty() ? PhysicsInvalidShapeCode : carried);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Entity = plan.Owner;
			diagnostic.Component = "RigidBody";
			diagnostic.Field = std::string(GetDynamicBodyField(checked.error()));
			diagnostic.Message = std::format("the Dynamic body of '{}' cannot be created: {}", scene.GetEntityPath(owner),
				carried.empty() ? text : text.substr(carried.size() + 2));
			diagnostic.Hint = carried == PhysicsAllDofsLockedCode
				? std::string("unlock a translation axis, or give the body colliders that can rotate it; the play session does not create the body otherwise")
				: std::format("lower Mass (at most {} kg) or make the colliders smaller; the play session does not create the body otherwise", MaxPhysicsMass);
			return diagnostic;
		}

		// The capsule check of a character (Height > 2 * Radius, then Jolt's own through PhysicsShape::Create).
		static std::optional<PhysicsDiagnostic> CheckCharacterShape(const Scene& scene, const PhysicsBodyPlan& plan)
		{
			const ConstEntity entity = scene.FindEntityByID(plan.Owner);
			const CharacterControllerComponent* controller = entity.IsValid() ? entity.TryGetComponent<CharacterControllerComponent>() : nullptr;
			if (controller == nullptr)
				return std::nullopt;

			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsInvalidShapeCode);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Entity = plan.Owner;
			diagnostic.Component = "CharacterController";
			if (!(controller->Height > 2.0f * controller->Radius))
			{
				diagnostic.Field = "Height";
				diagnostic.Message = std::format("the CharacterController of '{}' is {} m tall with a radius of {} m; its capsule needs Height > 2 * Radius",
					scene.GetEntityPath(entity), controller->Height, controller->Radius);
				diagnostic.Hint = std::format("raise Height above {} m, or lower Radius below {} m", 2.0f * controller->Radius, controller->Height * 0.5f);
				return diagnostic;
			}

			BodyShapeDescription capsule;
			capsule.Colliders.push_back(ColliderShapeDescription{
				.Geometry = CapsuleShapeGeometry{ .HalfHeight = controller->Height * 0.5f - controller->Radius, .Radius = controller->Radius } });
			const Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(capsule);
			if (shape.has_value())
				return std::nullopt;
			const std::string_view carried = GetPhysicsDiagnosticCode(shape.error());
			const std::string_view text = shape.error().GetMessageText();
			diagnostic.Message = std::format("the capsule of the CharacterController of '{}' cannot be built: {}", scene.GetEntityPath(entity),
				carried.empty() ? text : text.substr(carried.size() + 2));
			diagnostic.Hint = "correct Height and Radius; the play session does not create the character while its capsule is refused";
			return diagnostic;
		}

		// The pose of a body owned by `owner`: its world position and rotation (the world matrix decomposed, scale dropped:
		// it is baked into the shape). nullopt when the matrix does not decompose, which DescribePhysicsBodyShape refuses.
		static std::optional<glm::mat4> ComputeBodyPose(ConstEntity owner)
		{
			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(TransformSystem::ComputeWorldMatrix(owner));
			if (!decomposed)
				return std::nullopt;
			glm::mat4 pose = glm::mat4_cast(decomposed->Rotation);
			pose[3] = glm::vec4(decomposed->Translation, 1.0f);
			return pose;
		}

		// Builds every creatable body's shape: PHYSICS_INVALID_SHAPE for those that cannot be built, and the world bounds of
		// the implicit static bodies that can (the adjacency check's input, in canonical order).
		static std::vector<StaticBodyBounds> CheckShapes(const Scene& scene, const std::vector<PhysicsBodyPlan>& bodies, AssetManager* assets,
			std::vector<PhysicsDiagnostic>& diagnostics)
		{
			std::vector<StaticBodyBounds> statics;
			for (size_t order = 0; order < bodies.size(); ++order)
			{
				const PhysicsBodyPlan& plan = bodies[order];
				if (!plan.IsCreatable)
					continue;
				if (plan.Origin == PhysicsBodyOrigin::Character)
				{
					if (std::optional<PhysicsDiagnostic> diagnostic = CheckCharacterShape(scene, plan))
						diagnostics.push_back(std::move(*diagnostic));
					continue;
				}

				const Result<BodyShapeDescription> description = DescribePhysicsBodyShape(scene, plan, assets);
				if (!description)
				{
					diagnostics.push_back(MakeShapeDiagnostic(scene, plan, description.error()));
					continue;
				}
				const Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(*description);
				if (!shape)
				{
					diagnostics.push_back(MakeShapeDiagnostic(scene, plan, shape.error()));
					continue;
				}
				if (std::optional<PhysicsDiagnostic> diagnostic = CheckDynamicBody(scene, plan, **shape))
				{
					diagnostics.push_back(std::move(*diagnostic));
					continue;
				}
				if (plan.Origin != PhysicsBodyOrigin::ImplicitStatic)
					continue;
				const ConstEntity owner = scene.FindEntityByID(plan.Owner);
				const std::optional<glm::mat4> pose = owner.IsValid() ? ComputeBodyPose(owner) : std::nullopt;
				if (pose.has_value())
					statics.push_back({ .Owner = plan.Owner, .Order = order, .Bounds = (*shape)->GetLocalBounds().Transformed(*pose) });
			}
			return statics;
		}

		// The squared distance between two boxes (0 when they touch or overlap).
		static float GetSquaredDistance(const Aabb& a, const Aabb& b)
		{
			float squared = 0.0f;
			for (glm::length_t axis = 0; axis < 3; ++axis)
			{
				const float gap = std::max({ a.Min[axis] - b.Max[axis], b.Min[axis] - a.Max[axis], 0.0f });
				squared += gap * gap;
			}
			return squared;
		}

		// The nearest entity that is an ancestor of both `a` and `b` (never one of them); invalid when they share none.
		static ConstEntity FindCommonAncestor(ConstEntity a, ConstEntity b)
		{
			std::set<UUID> ancestors;
			for (ConstEntity current = a.GetParent(); current.IsValid(); current = current.GetParent())
				ancestors.insert(current.GetUUID());
			for (ConstEntity current = b.GetParent(); current.IsValid(); current = current.GetParent())
			{
				if (ancestors.contains(current.GetUUID()))
					return current;
			}
			return {};
		}

		static bool HasOwnTriggerCollider(ConstEntity entity)
		{
			const BoxColliderComponent* box = entity.TryGetComponent<BoxColliderComponent>();
			const SphereColliderComponent* sphere = entity.TryGetComponent<SphereColliderComponent>();
			const CapsuleColliderComponent* capsule = entity.TryGetComponent<CapsuleColliderComponent>();
			const MeshColliderComponent* mesh = entity.TryGetComponent<MeshColliderComponent>();
			return (box != nullptr && box->IsTrigger) || (sphere != nullptr && sphere->IsTrigger) || (capsule != nullptr && capsule->IsTrigger)
				|| (mesh != nullptr && mesh->IsTrigger);
		}

		static PhysicsDiagnostic MakeAdjacentDiagnostic(const Scene& scene, const StaticBodyBounds& first, const StaticBodyBounds& second)
		{
			const ConstEntity a = scene.FindEntityByID(first.Owner);
			const ConstEntity b = scene.FindEntityByID(second.Owner);
			const ConstEntity ancestor = FindCommonAncestor(a, b);

			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsAdjacentStaticBodiesCode);
			diagnostic.Severity = DiagnosticSeverity::Warning;
			diagnostic.Entity = first.Owner;
			diagnostic.Subject = second.Owner.ToString();
			diagnostic.Message = std::format("'{}' and '{}' are separate static bodies within {} mm of each other: a rolling body bumps at their seam",
				scene.GetEntityPath(a), scene.GetEntityPath(b), AdjacentStaticBodiesDistance * 1000.0f);
			if (!ancestor.IsValid())
			{
				diagnostic.Hint = "put both under one parent with a Static RigidBody (entity.reparent), so they form one static compound";
				return diagnostic;
			}
			diagnostic.FixTarget = ancestor.GetUUID();
			const std::string path = scene.GetEntityPath(ancestor);
			if (HasOwnTriggerCollider(ancestor))
			{
				diagnostic.Hint = std::format("move the trigger colliders of '{}' to a child entity, then add a Static RigidBody to '{}': a RigidBody there "
											  "would gather its trigger colliders with the solid pieces (PHYSICS_MIXED_TRIGGER)",
					path, path);
				return diagnostic;
			}
			diagnostic.AutoFixable = true;
			diagnostic.Hint =
				std::format("add a Static RigidBody to '{}', which joins both into one static compound (project.validate {{fix}} does it in the open scene)", path);
			return diagnostic;
		}

		// PHYSICS_ADJACENT_STATIC_BODIES for every pair of `statics` whose bounds are within AdjacentStaticBodiesDistance:
		// a sweep along x, then the distance between the boxes.
		static void CheckAdjacentStaticBodies(const Scene& scene, std::vector<StaticBodyBounds> statics, std::vector<PhysicsDiagnostic>& diagnostics)
		{
			std::sort(statics.begin(), statics.end(), [](const StaticBodyBounds& a, const StaticBodyBounds& b)
			{
				return std::tie(a.Bounds.Min.x, a.Order) < std::tie(b.Bounds.Min.x, b.Order);
			});
			constexpr float Distance = AdjacentStaticBodiesDistance;
			for (size_t i = 0; i < statics.size(); ++i)
			{
				for (size_t j = i + 1; j < statics.size() && statics[j].Bounds.Min.x <= statics[i].Bounds.Max.x + Distance; ++j)
				{
					if (GetSquaredDistance(statics[i].Bounds, statics[j].Bounds) > Distance * Distance)
						continue;
					const bool iFirst = statics[i].Order < statics[j].Order;
					diagnostics.push_back(MakeAdjacentDiagnostic(scene, iFirst ? statics[i] : statics[j], iFirst ? statics[j] : statics[i]));
				}
			}
		}

	}

	std::vector<PhysicsDiagnostic> ValidateScenePhysics(const Scene& scene, const PhysicsLayerTable& layers, AssetManager* assets)
	{
		PhysicsComposition composition = ComposePhysicsBodies(scene, layers);
		std::vector<PhysicsDiagnostic> diagnostics = std::move(composition.Diagnostics);
		if (PhysicsEngine::IsInitialized())
		{
			std::vector<StaticBodyBounds> statics = Utils::CheckShapes(scene, composition.Bodies, assets, diagnostics);
			Utils::CheckAdjacentStaticBodies(scene, std::move(statics), diagnostics);
		}
		else
		{
			ENGINE_CORE_WARN("Physics validation of scene '{}' skips the shape checks ({} and {}): the physics engine is not initialized", scene.GetName(),
				PhysicsInvalidShapeCode, PhysicsAdjacentStaticBodiesCode);
		}

		std::sort(diagnostics.begin(), diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return std::tie(a.Entity, a.Code, a.Subject, a.Component, a.Field, a.Message) < std::tie(b.Entity, b.Code, b.Subject, b.Component, b.Field, b.Message);
		});
		return diagnostics;
	}

}
