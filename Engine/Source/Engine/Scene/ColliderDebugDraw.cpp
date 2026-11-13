#include "EnginePCH.h"
#include "Engine/Scene/ColliderDebugDraw.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Physics/Physics2DWorld.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Private/Physics2DComposition.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>

// The collider visualization (Docs/Decisions/0014-m11-decisions.md decision 16): the composition decides which bodies
// exist and which colliders each holds, the components give the shapes, and each collider's world matrix (its rendered one
// in a play view between steps, Docs/Decisions/0016-m8-m11-m12-integration.md decision 3) gives its frame, so the records
// show what DescribePhysicsBodyShape bakes into the bodies (scale along the collider's axes, the largest component for
// spheres and capsules). A body the composition accepts but the session refuses is Invalid too: in play, one the session
// has not created; in edit, one whose shape cannot be built as the session builds it. The 2D colliders (M21,
// Docs/Decisions/0032-m21-contract.md decision 28) follow the same rules through the 2D composition: each collider's shapes
// as DescribePhysics2DBodyShape places them in its body, drawn at the body's planar pose as edges in its entity's plane.

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

		// The world matrix an entity's records are drawn at (ColliderDebugDrawOptions::Alpha): in a runtime scene below alpha
		// 1, its rendered pose (ComputeRenderedWorldMatrix over its WorldTransformComponent, §5.2), where the view's meshes
		// are extracted; otherwise its world matrix, walking the parent chain.
		static glm::mat4 GetDrawnWorldMatrix(ConstEntity entity, float alpha)
		{
			if (alpha < 1.0f && entity.GetScene()->IsRuntime())
				return ComputeRenderedWorldMatrix(entity, alpha);
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		// The frame of a collider whose Offset and Rotation are given in its entity's space, drawn at `alpha`; nullopt when
		// the matrix does not decompose (a scale product below the transform minimum), in which case the body is refused too
		// (DescribePhysicsBodyShape reports PHYSICS_INVALID_SHAPE) and nothing is drawn.
		static std::optional<ColliderFrame> ComputeColliderFrame(ConstEntity entity, const glm::vec3& offset, const glm::quat& rotation, float alpha)
		{
			glm::mat4 local = glm::mat4_cast(rotation);
			local[3] = glm::vec4(offset, 1.0f);
			ColliderFrame frame;
			frame.Matrix = GetDrawnWorldMatrix(entity, alpha) * local;
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
			const std::optional<ColliderFrame> frame = ComputeColliderFrame(entity, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), options.Alpha);
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

		// M26 (C-TERR; Docs/Decisions/0037-m26-contract.md decision 21, stream T-B): a terrain collider's record, the outline of
		// its height field: the four border rows and columns of samples (Asset/TerrainGeometry.h's placement, the one the body's
		// shape is built from) joined in order, at the entity's drawn world matrix. 4 * (Resolution - 1) segments at most, so a
		// terrain of 1025 samples a side draws 4,096 lines, where its triangle edges would be millions. nullopt without the
		// Terrain, an asset manager or a heightfield that loads, as the body is refused then.
		static std::optional<ColliderDebugShape> MakeTerrainShape(ConstEntity entity, ColliderDebugCategory category, UUID body, AssetManager* assets,
			float alpha)
		{
			const TerrainComponent* terrain = entity.TryGetComponent<TerrainComponent>();
			if (terrain == nullptr || !terrain->Heightfield.IsValid() || assets == nullptr)
				return std::nullopt;
			const Result<AssetRef<Asset>> loaded = assets->Load(terrain->Heightfield.GetHandle());
			const AssetRef<HeightfieldData> heightfield = loaded.has_value() ? AssetCast<HeightfieldData>(*loaded) : nullptr;
			if (heightfield == nullptr || !ValidateHeightfieldData(*heightfield).has_value())
				return std::nullopt;
			const std::optional<ColliderFrame> frame = ComputeColliderFrame(entity, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), alpha);
			if (!frame.has_value())
				return std::nullopt;

			const TerrainExtent extent{ .Size = terrain->Size, .HeightScale = terrain->HeightScale };
			const uint32_t last = heightfield->Resolution - 1;
			ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Lines, category, entity.GetUUID(), body);
			SetFrame(shape, *frame);
			shape.LineVertices.reserve(static_cast<size_t>(last) * 8);
			const auto addSegment = [&shape, &frame, &heightfield, &extent](uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1)
			{
				shape.LineVertices.emplace_back(frame->Matrix * glm::vec4(GetTerrainSamplePosition(*heightfield, extent, x0, z0), 1.0f));
				shape.LineVertices.emplace_back(frame->Matrix * glm::vec4(GetTerrainSamplePosition(*heightfield, extent, x1, z1), 1.0f));
			};
			// Along +X on the rows z = 0 and z = last, then along +Z on the columns x = 0 and x = last.
			for (const uint32_t z : { 0u, last })
			{
				for (uint32_t x = 0; x < last; ++x)
					addSegment(x, z, x + 1, z);
			}
			for (const uint32_t x : { 0u, last })
			{
				for (uint32_t z = 0; z < last; ++z)
					addSegment(x, z, x, z + 1);
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
					const std::optional<ColliderFrame> frame = box != nullptr ? ComputeColliderFrame(entity, box->Offset, box->Rotation, options.Alpha) : std::nullopt;
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
						? ComputeColliderFrame(entity, sphere->Offset, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), options.Alpha)
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
					const std::optional<ColliderFrame> frame =
						capsule != nullptr ? ComputeColliderFrame(entity, capsule->Offset, capsule->Rotation, options.Alpha) : std::nullopt;
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
				case PhysicsColliderType::Terrain:
					return MakeTerrainShape(entity, category, body, assets, options.Alpha);
			}

			ENGINE_CORE_ASSERT(false, "Unknown PhysicsColliderType {}", std::to_underlying(collider.Type));
			return std::nullopt;
		}

		// A character's capsule (§9.6): upright, its base at the entity's world position drawn at `alpha` (the capsule's
		// bottom), in metres (the controller's size takes no scale), and symmetric about the up axis, so only the base matters.
		static std::optional<ColliderDebugShape> MakeCharacterShape(const Scene& scene, const PhysicsBodyPlan& plan, ColliderDebugCategory category, float alpha)
		{
			const ConstEntity entity = scene.FindEntityByID(plan.Owner);
			if (!entity.IsValid())
				return std::nullopt;
			const CharacterControllerComponent* controller = entity.TryGetComponent<CharacterControllerComponent>();
			if (controller == nullptr)
				return std::nullopt;
			ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Capsule, category, plan.Owner, plan.Owner);
			shape.Position = glm::vec3(GetDrawnWorldMatrix(entity, alpha)[3]) + glm::vec3(0.0f, controller->Height * 0.5f, 0.0f);
			shape.Radius = controller->Radius;
			// A controller without a cylinder (Height <= 2 * Radius, PHYSICS_INVALID_SHAPE, drawn Invalid) draws its sphere.
			shape.HalfHeight = std::max(controller->Height * 0.5f - controller->Radius, 0.0f);
			return shape;
		}

	}

	std::vector<ColliderDebugShape> BuildColliderDebugDraw(const Scene& scene, const PhysicsLayerTable& layers, const PhysicsSystem* physics,
		AssetManager* assets, const ColliderDebugDrawOptions& options)
	{
		ENGINE_CORE_ASSERT(std::isfinite(options.Alpha) && options.Alpha >= 0.0f && options.Alpha <= 1.0f,
			"BuildColliderDebugDraw needs an alpha in [0, 1], got {}", options.Alpha);
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
				if (std::optional<ColliderDebugShape> shape = Utils::MakeCharacterShape(scene, plan, category, options.Alpha))
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

	void AppendColliderDebugDraw(std::span<const ColliderDebugShape> shapes, DebugDrawList& list)
	{
		for (const ColliderDebugShape& shape : shapes)
		{
			switch (shape.Type)
			{
				case ColliderDebugShapeType::Box:
					list.AddBox(shape.Position, shape.HalfExtents, shape.Rotation, shape.Color);
					break;
				case ColliderDebugShapeType::Sphere:
					list.AddSphere(shape.Position, shape.Radius, shape.Color);
					break;
				case ColliderDebugShapeType::Capsule:
				{
					const glm::vec3 axis = shape.Rotation * glm::vec3(0.0f, shape.HalfHeight, 0.0f);
					list.AddCapsule(shape.Position - axis, shape.Position + axis, shape.Radius, shape.Color);
					break;
				}
				case ColliderDebugShapeType::Lines:
					for (size_t index = 0; index + 1 < shape.LineVertices.size(); index += 2)
						list.AddLine(shape.LineVertices[index], shape.LineVertices[index + 1], shape.Color);
					break;
			}
		}
	}

	// --- 2D colliders (Docs/Decisions/0032-m21-contract.md decision 28) --------------------------------------------------

	namespace Utils {

		// The sides of a 2D circle's outline.
		constexpr uint32_t Collider2DCircleSides = 32;

		// Whether the body `plan` describes can be built as the play session builds it (Physics2DSystem's BuildShape and
		// CreateBody): its shape (DescribePhysics2DBodyShape, MakeBody2DShape, PhysicsShape::Create) and, for a Dynamic
		// RigidBody2D, PhysicsShape::CheckDynamicBody with the plane's degrees of freedom. True without the physics engine,
		// which nothing can be built without.
		static bool CanBuild2DBody(const Scene& scene, const Physics2DBodyPlan& plan, AssetManager* assets)
		{
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			if (!owner.IsValid())
				return false;
			if (!PhysicsEngine::IsInitialized())
				return true;
			const Result<Body2DShapeDescription> described = DescribePhysics2DBodyShape(scene, plan, assets);
			if (!described)
				return false;
			const Result<BodyShapeDescription> laid = MakeBody2DShape(*described);
			if (!laid)
				return false;
			const Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(*laid);
			if (!shape)
				return false;
			const RigidBody2DComponent* body = owner.TryGetComponent<RigidBody2DComponent>();
			if (plan.Origin == PhysicsBodyOrigin::RigidBody && plan.MotionType == PhysicsMotionType::Dynamic && body != nullptr)
				return (*shape)->CheckDynamicBody(body->Mass, body->FixedRotation ? Physics2DTranslationDofs : Physics2DDofs).has_value();
			return true;
		}

		// Whether the play session created the 2D body `plan` describes, as HasSessionBody for the 3D world: GetBodyInfo
		// reports an owner's RigidBody2D or implicit static body before its implicit sensor body, so an implicit sensor body
		// whose owner reports another one is decided by its shape.
		static bool HasSession2DBody(const Scene& scene, const Physics2DSystem& physics2D, const Physics2DBodyPlan& plan, AssetManager* assets)
		{
			const std::optional<Physics2DBodyReport> report = physics2D.GetBodyInfo(plan.Owner);
			if (!report.has_value() || report->Owner != plan.Owner)
				return false;
			return report->Origin == plan.Origin || CanBuild2DBody(scene, plan, assets);
		}

		// The category of a 2D body's colliders, as GetCategory's for the 3D ones (2D bodies have no characters).
		static ColliderDebugCategory Get2DCategory(const Scene& scene, const Physics2DBodyPlan& plan, const Physics2DSystem* physics2D, AssetManager* assets)
		{
			if (!plan.IsCreatable)
				return ColliderDebugCategory::Invalid;
			if (physics2D != nullptr ? !HasSession2DBody(scene, *physics2D, plan, assets) : !CanBuild2DBody(scene, plan, assets))
				return ColliderDebugCategory::Invalid;
			if (plan.IsSensor)
				return ColliderDebugCategory::Trigger;
			switch (plan.MotionType)
			{
				case PhysicsMotionType::Static:    return ColliderDebugCategory::Static;
				case PhysicsMotionType::Kinematic: return ColliderDebugCategory::Kinematic;
				case PhysicsMotionType::Dynamic:   break;
			}
			if (physics2D != nullptr)
			{
				const Result<bool> sleeping = physics2D->IsSleeping(plan.Owner);
				if (sleeping.has_value() && *sleeping)
					return ColliderDebugCategory::Sleeping;
			}
			return ColliderDebugCategory::Dynamic;
		}

		// The planar pose a 2D body's records are drawn at: its owner's world X and Y and the twist about Z of its world
		// rotation, the pose the play session gives the body (ToPhysics2DPose drops the swing), taken from the owner's
		// rendered pose in a runtime scene below alpha 1 (ColliderDebugDrawOptions::Alpha). nullopt when that rendered matrix
		// does not decompose.
		static std::optional<Physics2DPose> GetDrawn2DPose(ConstEntity owner, float alpha)
		{
			if (alpha < 1.0f && owner.GetScene()->IsRuntime())
			{
				const glm::mat4 rendered = ComputeRenderedWorldMatrix(owner, alpha);
				const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(rendered);
				if (!decomposed)
					return std::nullopt;
				return ToPhysics2DPose(PhysicsPose{ .Position = glm::vec3(rendered[3]), .Rotation = decomposed->Rotation });
			}
			return ToPhysics2DPose(
				PhysicsPose{ .Position = glm::vec3(TransformSystem::ComputeWorldMatrix(owner)[3]), .Rotation = TransformSystem::GetWorldRotation(owner) });
		}

		static glm::vec2 Rotate2D(const glm::vec2& value, const SinCosResult<float>& turn)
		{
			return glm::vec2(turn.Cos * value.x - turn.Sin * value.y, turn.Sin * value.x + turn.Cos * value.y);
		}

		// The convex hull of `points` counter-clockwise (Andrew's monotone chain, collinear points dropped): what Jolt builds
		// a PolygonShape2D's hull of. Two points for collinear input, fewer for fewer distinct points. The points are finite.
		static std::vector<glm::vec2> ComputeConvexHull2D(std::vector<glm::vec2> points)
		{
			std::ranges::sort(points, [](const glm::vec2& a, const glm::vec2& b)
			{
				return std::tie(a.x, a.y) < std::tie(b.x, b.y);
			});
			points.erase(std::unique(points.begin(), points.end()), points.end());
			if (points.size() < 3)
				return points;
			const auto turnsLeft = [](const glm::vec2& origin, const glm::vec2& a, const glm::vec2& b)
			{
				return (a.x - origin.x) * (b.y - origin.y) - (a.y - origin.y) * (b.x - origin.x) > 0.0f;
			};
			std::vector<glm::vec2> hull;
			hull.reserve(points.size() * 2);
			for (const glm::vec2& point : points)
			{
				while (hull.size() >= 2 && !turnsLeft(hull[hull.size() - 2], hull.back(), point))
					hull.pop_back();
				hull.push_back(point);
			}
			const size_t lower = hull.size() + 1;
			for (size_t index = points.size() - 1; index-- > 0;)
			{
				while (hull.size() >= lower && !turnsLeft(hull[hull.size() - 2], hull.back(), points[index]))
					hull.pop_back();
				hull.push_back(points[index]);
			}
			hull.pop_back(); // the first point, closing the chain
			return hull;
		}

		// The outline of one collider of a 2D body in its geometry's own space: a box's corners, a circle's polygon, a
		// polygon's convex hull, in order around it. Empty for a polygon with a point that is not finite (the body is refused).
		static std::vector<glm::vec2> GetCollider2DOutline(const Shape2DGeometry& geometry)
		{
			if (const BoxShape2D* box = std::get_if<BoxShape2D>(&geometry))
			{
				const glm::vec2 half = box->HalfExtents;
				return { glm::vec2(-half.x, -half.y), glm::vec2(half.x, -half.y), glm::vec2(half.x, half.y), glm::vec2(-half.x, half.y) };
			}
			if (const CircleShape2D* circle = std::get_if<CircleShape2D>(&geometry))
			{
				std::vector<glm::vec2> corners;
				corners.reserve(Collider2DCircleSides);
				constexpr float Step = 2.0f * std::numbers::pi_v<float> / static_cast<float>(Collider2DCircleSides);
				for (uint32_t side = 0; side < Collider2DCircleSides; ++side)
				{
					const SinCosResult<float> turn = DetMath::SinCos(static_cast<float>(side) * Step);
					corners.emplace_back(circle->Radius * turn.Cos, circle->Radius * turn.Sin);
				}
				return corners;
			}
			const PolygonShape2D* polygon = std::get_if<PolygonShape2D>(&geometry);
			ENGINE_CORE_ASSERT(polygon != nullptr, "Unknown Shape2DGeometry alternative {}", geometry.index());
			const auto isFinite = [](const glm::vec2& point)
			{
				return std::isfinite(point.x) && std::isfinite(point.y);
			};
			if (polygon == nullptr || !std::ranges::all_of(polygon->Points, isFinite))
				return {};
			return ComputeConvexHull2D(polygon->Points);
		}

		// Appends the edges of `collider`'s outline, placed in its body (its Scale, Angle and Position) and by the body's pose,
		// to `vertices` in the plane z = `z`, two vertices per edge; nothing when a placed vertex is not finite.
		static void AppendCollider2DEdges(const Collider2DShapeDescription& collider, const Physics2DPose& body, float z, std::vector<glm::vec3>& vertices)
		{
			const SinCosResult<float> colliderTurn = DetMath::SinCos(collider.Angle);
			const SinCosResult<float> bodyTurn = DetMath::SinCos(body.Angle);
			std::vector<glm::vec3> corners;
			for (const glm::vec2& point : GetCollider2DOutline(collider.Geometry))
			{
				const glm::vec2 inBody = collider.Position + Rotate2D(point * collider.Scale, colliderTurn);
				const glm::vec3 world(body.Position + Rotate2D(inBody, bodyTurn), z);
				if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
					return;
				corners.push_back(world);
			}
			// A segment for two corners (a degenerate hull), a closed loop for more.
			const size_t edges = corners.size() == 2 ? 1 : (corners.size() >= 3 ? corners.size() : 0);
			for (size_t edge = 0; edge < edges; ++edge)
			{
				vertices.push_back(corners[edge]);
				vertices.push_back(corners[(edge + 1) % corners.size()]);
			}
		}

		// The record of collider `collider` of the 2D body `plan`, drawn at the body's pose `body`: one Lines record of the
		// edges of every shape the collider adds to the body (DescribePhysics2DBodyShape of it alone, so a collider that
		// cannot be placed costs only its own record) in the plane of its entity's world Z. Its frame is the body's pose at
		// that Z. nullopt when its entity is gone, it cannot be described or it has nothing to draw.
		static std::optional<ColliderDebugShape> MakeCollider2DShape(const Scene& scene, const Physics2DBodyPlan& plan, const Physics2DColliderPlan& collider,
			ColliderDebugCategory category, const Physics2DPose& body, float alpha, AssetManager* assets)
		{
			const ConstEntity entity = scene.FindEntityByID(collider.Entity);
			if (!entity.IsValid())
				return std::nullopt;
			const Physics2DBodyPlan alone{ .Owner = plan.Owner,
				.Origin = plan.Origin,
				.MotionType = plan.MotionType,
				.IsSensor = plan.IsSensor,
				.Layer = plan.Layer,
				.CollisionGroup = plan.CollisionGroup,
				.Colliders = { collider },
				.IsCreatable = plan.IsCreatable };
			const Result<Body2DShapeDescription> described = DescribePhysics2DBodyShape(scene, alone, assets);
			if (!described)
				return std::nullopt;
			const float z = GetDrawnWorldMatrix(entity, alpha)[3].z;
			ColliderDebugShape shape = MakeShape(ColliderDebugShapeType::Lines, category, collider.Entity, plan.Owner);
			shape.Position = glm::vec3(body.Position, z);
			shape.Rotation = ToPhysicsPose(body).Rotation;
			for (const Collider2DShapeDescription& placed : described->Colliders)
				AppendCollider2DEdges(placed, body, z, shape.LineVertices);
			if (shape.LineVertices.empty())
				return std::nullopt;
			return shape;
		}

	}

	std::vector<ColliderDebugShape> BuildCollider2DDebugDraw(const Scene& scene, const PhysicsLayerTable& layers, const Physics2DSystem* physics2D,
		AssetManager* assets, const ColliderDebugDrawOptions& options)
	{
		ENGINE_CORE_ASSERT(std::isfinite(options.Alpha) && options.Alpha >= 0.0f && options.Alpha <= 1.0f,
			"BuildCollider2DDebugDraw needs an alpha in [0, 1], got {}", options.Alpha);
		const Physics2DComposition composition = ComposePhysics2DBodies(scene, layers);
		std::vector<ColliderDebugShape> shapes;
		for (const Physics2DBodyPlan& plan : composition.Bodies)
		{
			if (plan.IsSensor && !options.Triggers)
				continue;
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			if (!owner.IsValid())
				continue;
			const std::optional<Physics2DPose> body = Utils::GetDrawn2DPose(owner, options.Alpha);
			if (!body.has_value())
				continue;
			const ColliderDebugCategory category = Utils::Get2DCategory(scene, plan, physics2D, assets);
			for (const Physics2DColliderPlan& collider : plan.Colliders)
			{
				if (std::optional<ColliderDebugShape> shape = Utils::MakeCollider2DShape(scene, plan, collider, category, *body, options.Alpha, assets))
					shapes.push_back(std::move(*shape));
			}
		}
		return shapes;
	}

}
