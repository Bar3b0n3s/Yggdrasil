#include "EnginePCH.h"
#include "Engine/Scene/ColliderDebugDraw.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <tuple>
#include <utility>

// The collider visualization (Docs/Decisions/0014-m11-decisions.md decision 16): the composition decides which bodies
// exist and which colliders each holds, the components give the shapes, and each collider's world matrix gives its frame,
// so the records show what DescribePhysicsBodyShape bakes into the bodies (scale along the collider's axes, the largest
// component for spheres and capsules). A body the composition accepts but the session refuses is Invalid too: in play,
// one the session has not created; in edit, one whose shape cannot be built as the session builds it.

namespace Engine {

	namespace {

		// A collider's frame in world space, decomposed: where the record is drawn and the scale baked into its dimensions.
		struct ColliderFrame
		{
			glm::mat4 Matrix = glm::mat4(1.0f); // the entity's world matrix, then the collider's Offset and Rotation
			glm::vec3 Position = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Scale = glm::vec3(1.0f); // the magnitude of the scale along each of the collider's axes
		};

	}

	namespace Utils {

		// The frame of a collider whose Offset and Rotation are given in its entity's space; nullopt when the matrix does not
		// decompose (a scale product below the transform minimum), in which case the body is refused too
		// (DescribePhysicsBodyShape reports PHYSICS_INVALID_SHAPE) and nothing is drawn.
		static std::optional<ColliderFrame> ComputeColliderFrame(ConstEntity entity, const glm::vec3& offset, const glm::quat& rotation)
		{
			glm::mat4 local = glm::mat4_cast(rotation);
			local[3] = glm::vec4(offset, 1.0f);
			ColliderFrame frame;
			frame.Matrix = TransformSystem::ComputeWorldMatrix(entity) * local;
			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(frame.Matrix);
			if (!decomposed)
				return std::nullopt;
			frame.Position = decomposed->Translation;
			frame.Rotation = decomposed->Rotation;
			frame.Scale = glm::abs(decomposed->Scale);
			return frame;
		}

		static float GetLargestComponent(const glm::vec3& value)
		{
			return std::max({ value.x, value.y, value.z });
		}

		static ColliderDebugShape MakeShape(ColliderDebugShapeType type, ColliderDebugCategory category, UUID collider, UUID body)
		{
			ColliderDebugShape shape;
			shape.Type = type;
			shape.Category = category;
			shape.Color = GetColliderDebugColor(category);
			shape.Collider = collider;
			shape.Body = body;
			return shape;
		}

		static void SetFrame(ColliderDebugShape& shape, const ColliderFrame& frame)
		{
			shape.Position = frame.Position;
			shape.Rotation = frame.Rotation;
		}

		// Whether the body `plan` describes can be built as the play session builds it: its shape (DescribePhysicsBodyShape,
		// then PhysicsShape::Create) and, for a Dynamic RigidBody, PhysicsShape::CheckDynamicBody; a character's capsule needs
		// Height > 2 * Radius and Jolt's acceptance. True without the physics engine, which nothing can be built without.
		static bool CanBuildBody(const Scene& scene, const PhysicsBodyPlan& plan, AssetManager* assets)
		{
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			if (!owner.IsValid())
				return false;
			BodyShapeDescription description;
			if (plan.Origin == PhysicsBodyOrigin::Character)
			{
				const CharacterControllerComponent* controller = owner.TryGetComponent<CharacterControllerComponent>();
				if (controller == nullptr || !(controller->Height > 2.0f * controller->Radius))
					return false;
				description.Colliders.push_back(ColliderShapeDescription{
					.Geometry = CapsuleShapeGeometry{ .HalfHeight = controller->Height * 0.5f - controller->Radius, .Radius = controller->Radius } });
			}
			if (!PhysicsEngine::IsInitialized())
				return true;
			if (plan.Origin != PhysicsBodyOrigin::Character)
			{
				Result<BodyShapeDescription> described = DescribePhysicsBodyShape(scene, plan, assets);
				if (!described)
					return false;
				description = std::move(*described);
			}
			const Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(description);
			if (!shape)
				return false;
			const RigidBodyComponent* body = owner.TryGetComponent<RigidBodyComponent>();
			if (plan.Origin == PhysicsBodyOrigin::RigidBody && plan.MotionType == PhysicsMotionType::Dynamic && body != nullptr)
				return (*shape)->CheckDynamicBody(body->Mass, GetRigidBodyDofs(*body)).has_value();
			return true;
		}

		// Whether the play session created the body `plan` describes. GetBodyInfo reports an owner's RigidBody, character or
		// implicit static body before its implicit sensor body, so an implicit sensor body whose owner reports another one
		// is decided by its shape.
		static bool HasSessionBody(const Scene& scene, const PhysicsSystem& physics, const PhysicsBodyPlan& plan, AssetManager* assets)
		{
			const std::optional<PhysicsBodyReport> report = physics.GetBodyInfo(plan.Owner);
			if (!report.has_value() || report->Owner != plan.Owner)
				return false;
			return report->Origin == plan.Origin || CanBuildBody(scene, plan, assets);
		}

		// The category of `plan`'s colliders: refused bodies first (by the composition, by the session in play, by their shape
		// in edit), then sensors and characters, then the motion type; a Dynamic body of a play session is Sleeping while
		// Jolt has put it to sleep.
		static ColliderDebugCategory GetCategory(const Scene& scene, const PhysicsBodyPlan& plan, const PhysicsSystem* physics, AssetManager* assets)
		{
			if (!plan.IsCreatable)
				return ColliderDebugCategory::Invalid;
			if (physics != nullptr ? !HasSessionBody(scene, *physics, plan, assets) : !CanBuildBody(scene, plan, assets))
				return ColliderDebugCategory::Invalid;
			if (plan.IsSensor)
				return ColliderDebugCategory::Trigger;
			if (plan.Origin == PhysicsBodyOrigin::Character)
				return ColliderDebugCategory::Character;
			switch (plan.MotionType)
			{
				case PhysicsMotionType::Static:    return ColliderDebugCategory::Static;
				case PhysicsMotionType::Kinematic: return ColliderDebugCategory::Kinematic;
				case PhysicsMotionType::Dynamic:   break;
			}
			if (physics != nullptr)
			{
				const Result<bool> sleeping = physics->IsSleeping(plan.Owner);
				if (sleeping.has_value() && *sleeping)
					return ColliderDebugCategory::Sleeping;
			}
			return ColliderDebugCategory::Dynamic;
		}

		// The edges of a mesh's triangles, each once, as pairs of indices into `vertices`: vertices at bit-identical positions
		// (split for normals or texture coordinates) are welded first, so a shared edge is drawn once, and edges that weld to
		// a point are dropped. Sorted, so the order is deterministic. Triangles with an index outside `vertices` are skipped (a
		// loaded MeshData never has them).
		static std::vector<std::pair<uint32_t, uint32_t>> CollectMeshEdges(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices)
		{
			const auto key = [&vertices](uint32_t index)
			{
				const glm::vec3& position = vertices[index].Position;
				return std::make_tuple(std::bit_cast<uint32_t>(position.x), std::bit_cast<uint32_t>(position.y), std::bit_cast<uint32_t>(position.z));
			};
			std::vector<uint32_t> order(vertices.size());
			for (size_t index = 0; index < order.size(); ++index)
				order[index] = static_cast<uint32_t>(index);
			std::sort(order.begin(), order.end(), [&key](uint32_t a, uint32_t b)
			{
				return std::make_pair(key(a), a) < std::make_pair(key(b), b);
			});
			// Each vertex maps to the lowest index at its position.
			std::vector<uint32_t> weld(vertices.size());
			for (size_t index = 0; index < order.size(); ++index)
				weld[order[index]] = (index > 0 && key(order[index - 1]) == key(order[index])) ? weld[order[index - 1]] : order[index];

			std::vector<std::pair<uint32_t, uint32_t>> edges;
			edges.reserve(indices.size());
			for (size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3)
			{
				const std::array<uint32_t, 3> corners = { indices[triangle], indices[triangle + 1], indices[triangle + 2] };
				if (corners[0] >= vertices.size() || corners[1] >= vertices.size() || corners[2] >= vertices.size())
					continue;
				for (size_t side = 0; side < 3; ++side)
				{
					const uint32_t a = weld[corners[side]];
					const uint32_t b = weld[corners[(side + 1) % 3]];
					if (a != b)
						edges.emplace_back(std::min(a, b), std::max(a, b));
				}
			}
			std::sort(edges.begin(), edges.end());
			edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
			return edges;
		}

		// A mesh collider's record: its triangle edges in world space, or its local bounding box when it has more edges than
		// `options.MaxMeshEdges`. nullopt without a mesh (no Mesh and no MeshRenderer mesh, or no asset manager), as the body
		// is refused then.
		static std::optional<ColliderDebugShape> MakeMeshShape(ConstEntity entity, const MeshColliderComponent& collider, ColliderDebugCategory category,
			UUID body, AssetManager* assets, const ColliderDebugDrawOptions& options)
		{
			AssetHandle handle = collider.Mesh.GetHandle();
			if (!handle.IsValid())
			{
				if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>())
					handle = renderer->Mesh.GetHandle();
			}
			if (!handle.IsValid() || assets == nullptr)
				return std::nullopt;
			const std::optional<ColliderFrame> frame = ComputeColliderFrame(entity, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			if (!frame.has_value())
				return std::nullopt;

			// Missing or failed meshes give the placeholder cube, as the body's shape does (DescribePhysicsBodyShape).
			const AssetRef<MeshData> mesh = assets->GetOrPlaceholder<MeshData>(handle);
			const std::vector<std::pair<uint32_t, uint32_t>> edges = CollectMeshEdges(mesh->Vertices, mesh->Indices);
			if (edges.size() > options.MaxMeshEdges)
			{
				ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Box, category, entity.GetUUID(), body);
				shape.Rotation = frame->Rotation;
				shape.Position = glm::vec3(frame->Matrix * glm::vec4(mesh->Bounds.GetCenter(), 1.0f));
				shape.HalfExtents = mesh->Bounds.GetSize() * 0.5f * frame->Scale;
				return shape;
			}

			ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Lines, category, entity.GetUUID(), body);
			SetFrame(shape, *frame);
			shape.LineVertices.reserve(edges.size() * 2);
			for (const auto& [a, b] : edges)
			{
				shape.LineVertices.emplace_back(frame->Matrix * glm::vec4(mesh->Vertices[a].Position, 1.0f));
				shape.LineVertices.emplace_back(frame->Matrix * glm::vec4(mesh->Vertices[b].Position, 1.0f));
			}
			return shape;
		}

		// The record of one collider of a body; nullopt when its component is gone or its frame or mesh is unavailable.
		static std::optional<ColliderDebugShape> MakeColliderShape(const Scene& scene, const PhysicsColliderPlan& collider, ColliderDebugCategory category,
			UUID body, AssetManager* assets, const ColliderDebugDrawOptions& options)
		{
			const ConstEntity entity = scene.FindEntityByID(collider.Entity);
			if (!entity.IsValid())
				return std::nullopt;
			switch (collider.Type)
			{
				case PhysicsColliderType::Box:
				{
					const BoxColliderComponent* box = entity.TryGetComponent<BoxColliderComponent>();
					const std::optional<ColliderFrame> frame = box != nullptr ? ComputeColliderFrame(entity, box->Offset, box->Rotation) : std::nullopt;
					if (!frame.has_value())
						return std::nullopt;
					ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Box, category, collider.Entity, body);
					SetFrame(shape, *frame);
					shape.HalfExtents = box->HalfExtents * frame->Scale;
					return shape;
				}
				case PhysicsColliderType::Sphere:
				{
					const SphereColliderComponent* sphere = entity.TryGetComponent<SphereColliderComponent>();
					const std::optional<ColliderFrame> frame = sphere != nullptr
						? ComputeColliderFrame(entity, sphere->Offset, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
						: std::nullopt;
					if (!frame.has_value())
						return std::nullopt;
					ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Sphere, category, collider.Entity, body);
					SetFrame(shape, *frame);
					shape.Radius = sphere->Radius * GetLargestComponent(frame->Scale);
					return shape;
				}
				case PhysicsColliderType::Capsule:
				{
					const CapsuleColliderComponent* capsule = entity.TryGetComponent<CapsuleColliderComponent>();
					const std::optional<ColliderFrame> frame = capsule != nullptr ? ComputeColliderFrame(entity, capsule->Offset, capsule->Rotation) : std::nullopt;
					if (!frame.has_value())
						return std::nullopt;
					ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Capsule, category, collider.Entity, body);
					SetFrame(shape, *frame);
					const float scale = GetLargestComponent(frame->Scale);
					shape.Radius = capsule->Radius * scale;
					shape.HalfHeight = capsule->HalfHeight * scale;
					return shape;
				}
				case PhysicsColliderType::Mesh:
				{
					const MeshColliderComponent* mesh = entity.TryGetComponent<MeshColliderComponent>();
					if (mesh == nullptr)
						return std::nullopt;
					return MakeMeshShape(entity, *mesh, category, body, assets, options);
				}
			}

			ENGINE_CORE_ASSERT(false, "Unknown PhysicsColliderType {}", std::to_underlying(collider.Type));
			return std::nullopt;
		}

		// A character's capsule (§9.6): upright, its base at the entity's world position (the capsule's bottom), in metres
		// (the controller's size takes no scale), and symmetric about the up axis, so only the base matters.
		static std::optional<ColliderDebugShape> MakeCharacterShape(const Scene& scene, const PhysicsBodyPlan& plan, ColliderDebugCategory category)
		{
			const ConstEntity entity = scene.FindEntityByID(plan.Owner);
			if (!entity.IsValid())
				return std::nullopt;
			const CharacterControllerComponent* controller = entity.TryGetComponent<CharacterControllerComponent>();
			if (controller == nullptr)
				return std::nullopt;
			ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Capsule, category, plan.Owner, plan.Owner);
			shape.Position = TransformSystem::GetWorldPosition(entity) + glm::vec3(0.0f, controller->Height * 0.5f, 0.0f);
			shape.Radius = controller->Radius;
			// A controller without a cylinder (Height <= 2 * Radius, PHYSICS_INVALID_SHAPE, drawn Invalid) draws its sphere.
			shape.HalfHeight = std::max(controller->Height * 0.5f - controller->Radius, 0.0f);
			return shape;
		}

	}

	std::vector<ColliderDebugShape> BuildColliderDebugDraw(const Scene& scene, const PhysicsLayerTable& layers, const PhysicsSystem* physics,
		AssetManager* assets, const ColliderDebugDrawOptions& options)
	{
		const PhysicsComposition composition = ComposePhysicsBodies(scene, layers);
		std::vector<ColliderDebugShape> shapes;
		for (const PhysicsBodyPlan& plan : composition.Bodies)
		{
			if (plan.IsSensor && !options.Triggers)
				continue;
			const ColliderDebugCategory category = Utils::GetCategory(scene, plan, physics, assets);
			if (plan.Origin == PhysicsBodyOrigin::Character)
			{
				if (!options.Characters)
					continue;
				if (std::optional<ColliderDebugShape> shape = Utils::MakeCharacterShape(scene, plan, category))
					shapes.push_back(std::move(*shape));
				continue;
			}
			for (const PhysicsColliderPlan& collider : plan.Colliders)
			{
				if (std::optional<ColliderDebugShape> shape = Utils::MakeColliderShape(scene, collider, category, plan.Owner, assets, options))
					shapes.push_back(std::move(*shape));
			}
		}
		return shapes;
	}

}
