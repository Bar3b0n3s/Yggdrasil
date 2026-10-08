#include "EnginePCH.h"
#include "Engine/Scene/PhysicsComposition.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

// The composition rules of PhysicsComposition.h in one canonical walk (parents before children, Architecture §5.1): every
// entity inherits from its parent what the rules need (its nearest RigidBody or CharacterController, the layer implicit
// bodies take, the owner of an implicit static compound, whether an ancestor moves), so each plan is complete when its
// owner is visited and gathered colliders are appended in canonical order. Shapes are described relative to their owner:
// the owner's world scale (TransformSystem::GetWorldScale), then the local matrices below the owner down to each collider,
// so a description does not depend on where the owner is.

namespace Engine {

	namespace {

		// What the walk carries from an entity to its children.
		struct CompositionEntity
		{
			// The nearest entity at or above with a RigidBody or a CharacterController; invalid for none.
			UUID Carrier{};
			bool CarrierIsCharacter = false;
			// The layer of the nearest RigidBody at or above (implicit bodies take it); 0 without one.
			uint32_t InheritedLayer = 0;
			// Without a carrier: the topmost entity at or above with solid colliders, the owner of their implicit static body.
			UUID ImplicitStaticOwner{};
			// A strict ancestor owns a Kinematic or Dynamic RigidBody or a CharacterController (it moves on its own).
			bool MovingAncestor = false;
			// The entity itself owns a Kinematic or Dynamic RigidBody or a CharacterController.
			bool Moving = false;
		};

		// The cache's key: the scale's bits, so two scales that differ in any bit get separate shapes.
		struct MeshShapeKey
		{
			AssetHandle Mesh{};
			uint64_t Version = 0;
			bool Convex = false;
			uint32_t ScaleX = 0;
			uint32_t ScaleY = 0;
			uint32_t ScaleZ = 0;

			auto operator<=>(const MeshShapeKey&) const = default;
		};

	}

	struct PhysicsMeshShapeCache::State
	{
		std::map<MeshShapeKey, Ref<const PhysicsShape>> Entries;
	};

	namespace Utils {

		// The entity's collider components in BuiltinComponents order (§9.2: the order of sub-shape user data).
		static void AppendOwnColliders(ConstEntity entity, std::vector<PhysicsColliderPlan>& colliders)
		{
			const UUID id = entity.GetUUID();
			if (const BoxColliderComponent* box = entity.TryGetComponent<BoxColliderComponent>())
				colliders.push_back(PhysicsColliderPlan{ .Entity = id, .Type = PhysicsColliderType::Box, .IsTrigger = box->IsTrigger });
			if (const SphereColliderComponent* sphere = entity.TryGetComponent<SphereColliderComponent>())
				colliders.push_back(PhysicsColliderPlan{ .Entity = id, .Type = PhysicsColliderType::Sphere, .IsTrigger = sphere->IsTrigger });
			if (const CapsuleColliderComponent* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
				colliders.push_back(PhysicsColliderPlan{ .Entity = id, .Type = PhysicsColliderType::Capsule, .IsTrigger = capsule->IsTrigger });
			if (const MeshColliderComponent* mesh = entity.TryGetComponent<MeshColliderComponent>())
				colliders.push_back(PhysicsColliderPlan{ .Entity = id, .Type = PhysicsColliderType::Mesh, .IsTrigger = mesh->IsTrigger });
		}

		// "'Name' (<uuid>)", how diagnostics name an entity.
		static std::string DescribeEntity(ConstEntity entity)
		{
			return std::format("'{}' ({})", entity.GetName(), entity.GetUUID());
		}

		static PhysicsDiagnostic MakeDiagnostic(std::string_view code, DiagnosticSeverity severity, UUID entity, std::string_view component,
			std::string_view field, std::string message, std::string hint)
		{
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(code);
			diagnostic.Severity = severity;
			diagnostic.Entity = entity;
			diagnostic.Component = std::string(component);
			diagnostic.Field = std::string(field);
			diagnostic.Message = std::move(message);
			diagnostic.Hint = std::move(hint);
			return diagnostic;
		}

		// The layer named `name`, or layer 0 with PHYSICS_UNKNOWN_LAYER on `entity`'s `component`.
		static uint32_t ResolveLayer(const PhysicsLayerTable& layers, ConstEntity entity, std::string_view component, const std::string& name,
			std::vector<PhysicsDiagnostic>& diagnostics)
		{
			if (const std::optional<uint32_t> index = layers.FindLayer(name); index.has_value())
				return *index;

			const std::vector<std::string> suggestions = FuzzySuggest(name, layers.GetNames());
			std::string hint = MakeDidYouMeanHint(suggestions);
			if (hint.empty())
				hint = "declare the layer in the project's physics settings (PhysicsSettings.Layers), or use one of its layers";
			diagnostics.push_back(MakeDiagnostic(PhysicsUnknownLayerCode, DiagnosticSeverity::Error, entity.GetUUID(), component, "Layer",
				std::format("the {} of {} names the layer '{}', which the project's physics settings do not declare; it uses layer '{}'", component,
					DescribeEntity(entity), name, DefaultPhysicsLayerName),
				std::move(hint)));
			return 0;
		}

		// Whether the three components have the same magnitude, up to the rounding of a scale read back from a rotated world
		// matrix (TransformSystem::GetWorldScale takes column lengths).
		static bool IsUniformScale(const glm::vec3& scale)
		{
			constexpr float RelativeTolerance = 1.0e-5f;
			const glm::vec3 magnitude = glm::abs(scale);
			const float largest = std::max({ magnitude.x, magnitude.y, magnitude.z });
			const float smallest = std::min({ magnitude.x, magnitude.y, magnitude.z });
			return largest - smallest <= largest * RelativeTolerance;
		}

		// The checks of a RigidBody plan once its colliders are known (see ComposePhysicsBodies).
		static void FinishRigidBodyPlan(const Scene& scene, PhysicsBodyPlan& plan, std::vector<PhysicsDiagnostic>& diagnostics,
			const std::unordered_map<UUID, CompositionEntity>& entities)
		{
			if (plan.Colliders.empty())
			{
				plan.IsCreatable = false;
				return;
			}

			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			const RigidBodyComponent& body = owner.GetComponent<RigidBodyComponent>();
			const auto triggers = static_cast<size_t>(std::count_if(plan.Colliders.begin(), plan.Colliders.end(), [](const PhysicsColliderPlan& collider)
			{
				return collider.IsTrigger;
			}));
			if (triggers != 0 && triggers != plan.Colliders.size())
			{
				diagnostics.push_back(MakeDiagnostic(PhysicsMixedTriggerCode, DiagnosticSeverity::Error, plan.Owner, "", "IsTrigger",
					std::format("the {} colliders of the body of {} disagree on IsTrigger ({} triggers, {} solid); the body is not created",
						plan.Colliders.size(), DescribeEntity(owner), triggers, plan.Colliders.size() - triggers),
					"make every collider of the body a trigger or none of them; a trigger on a child entity without a RigidBody gets a sensor body "
					"of its own"));
				plan.IsCreatable = false;
				return;
			}

			plan.IsSensor = triggers != 0;
			if (plan.IsSensor && body.Type == BodyType::Dynamic)
			{
				diagnostics.push_back(MakeDiagnostic(PhysicsDynamicTriggerCode, DiagnosticSeverity::Error, plan.Owner, "RigidBody", "Type",
					std::format("the RigidBody of {} is Dynamic but its colliders are triggers, and a trigger cannot be simulated; the body is not "
								"created",
						DescribeEntity(owner)),
					"set RigidBody.Type to Kinematic or Static, or clear IsTrigger on its colliders"));
				plan.IsCreatable = false;
			}
			if (plan.IsSensor && body.Type == BodyType::Static)
				plan.MotionType = PhysicsMotionType::Kinematic; // §9.2: triggers are Kinematic and kept active

			if (body.Type != BodyType::Dynamic)
				return;

			for (const PhysicsColliderPlan& collider : plan.Colliders)
			{
				if (collider.Type != PhysicsColliderType::Mesh)
					continue;
				const ConstEntity entity = scene.FindEntityByID(collider.Entity);
				if (entity.GetComponent<MeshColliderComponent>().Convex)
					continue;
				diagnostics.push_back(MakeDiagnostic(PhysicsNonconvexDynamicCode, DiagnosticSeverity::Error, collider.Entity, "MeshCollider", "Convex",
					std::format("the MeshCollider of {} is not convex, which the Dynamic body of {} cannot use; the body is not created",
						DescribeEntity(entity), DescribeEntity(owner)),
					"set MeshCollider.Convex to true, or make the RigidBody Static or Kinematic"));
				plan.IsCreatable = false;
			}
			if (GetRigidBodyDofs(body) == PhysicsDofs::None)
			{
				diagnostics.push_back(MakeDiagnostic(PhysicsAllDofsLockedCode, DiagnosticSeverity::Error, plan.Owner, "RigidBody", "LockRotation",
					std::format("the Dynamic RigidBody of {} locks every translation and rotation axis, so it cannot move; the body is not created",
						DescribeEntity(owner)),
					"unlock at least one axis of LockTranslation or LockRotation, or make the body Kinematic or Static"));
				plan.IsCreatable = false;
			}
			const auto ownerInfo = entities.find(plan.Owner);
			ENGINE_CORE_VERIFY(ownerInfo != entities.end(), "PhysicsComposition: the owner of a RigidBody plan was not visited");
			if (ownerInfo != entities.end() && ownerInfo->second.MovingAncestor)
			{
				diagnostics.push_back(MakeDiagnostic(PhysicsDynamicUnderMovingParentCode, DiagnosticSeverity::Warning, plan.Owner, "RigidBody", "Type",
					std::format("the Dynamic body of {} is below an entity that moves on its own; it simulates in world space and does not follow "
								"its parent",
						DescribeEntity(owner)),
					"move the entity out from under the moving body, or make it Kinematic to have it carried"));
			}
		}

		// PHYSICS_NONUNIFORM_SCALE for each entity with a sphere or capsule collider in a body under a non-uniform world scale.
		static void CheckColliderScales(const Scene& scene, const std::vector<PhysicsBodyPlan>& bodies, std::vector<PhysicsDiagnostic>& diagnostics)
		{
			std::map<UUID, PhysicsColliderType> reported;
			for (const PhysicsBodyPlan& plan : bodies)
			{
				for (const PhysicsColliderPlan& collider : plan.Colliders)
				{
					if (collider.Type != PhysicsColliderType::Sphere && collider.Type != PhysicsColliderType::Capsule)
						continue;
					if (reported.contains(collider.Entity))
						continue;
					const ConstEntity entity = scene.FindEntityByID(collider.Entity);
					const glm::vec3 scale = TransformSystem::GetWorldScale(entity);
					if (IsUniformScale(scale))
						continue;
					reported.emplace(collider.Entity, collider.Type);
					const std::string_view component = GetColliderComponentName(collider.Type);
					diagnostics.push_back(MakeDiagnostic(PhysicsNonuniformScaleCode, DiagnosticSeverity::Warning, collider.Entity, component, "",
						std::format("the {} of {} is under the non-uniform world scale ({}, {}, {}); it uses the largest axis", component,
							DescribeEntity(entity), scale.x, scale.y, scale.z),
						"give the entity and its ancestors a uniform scale, or use a BoxCollider or MeshCollider"));
				}
			}
		}

		// Marks the creatable bodies beyond `maxBodies` (canonical order) as refused, with one diagnostic per refused owner
		// (§9.1: every refused entity gets one; an owner's two bodies are adjacent in the list).
		static void ApplyBodyLimit(const Scene& scene, std::vector<PhysicsBodyPlan>& bodies, uint32_t maxBodies, std::vector<PhysicsDiagnostic>& diagnostics)
		{
			uint32_t creatable = 0;
			UUID reported{};
			for (PhysicsBodyPlan& plan : bodies)
			{
				if (!plan.IsCreatable)
					continue;
				if (creatable < maxBodies)
				{
					++creatable;
					continue;
				}
				plan.IsCreatable = false;
				if (plan.Owner == reported)
					continue;
				reported = plan.Owner;
				PhysicsDiagnostic diagnostic = MakeDiagnostic(PhysicsLimitExceededCode, DiagnosticSeverity::Error, plan.Owner, "", "",
					std::format("the {} of {} is not created: the scene makes more than {} physics bodies, the most a world holds, and the bodies "
								"beyond them in canonical order are refused",
						plan.Origin == PhysicsBodyOrigin::Character ? "character" : "body", DescribeEntity(scene.FindEntityByID(plan.Owner)), maxBodies),
					"merge colliders into compounds (one RigidBody on a parent gathers its collider-only children) or remove bodies");
				diagnostic.Subject = "bodies";
				diagnostics.push_back(std::move(diagnostic));
			}
		}

		// The location of an error about `entity` (DescribePhysicsBodyShape's errors name the collider entity at fault).
		static ErrorLocation AtEntity(UUID entity)
		{
			ErrorLocation location;
			location.Entity = entity;
			return location;
		}

		static std::unexpected<Error> MakeShapeError(UUID entity, std::string message)
		{
			return std::unexpected(Error(ErrorCode::Validation, std::format("{}: {}", PhysicsInvalidShapeCode, std::move(message))).WithLocation(AtEntity(entity)));
		}

		// The product of the local matrices from the owner's child down to `entity` (the identity for the owner itself); nullopt
		// when `entity` is not `owner` or one of its descendants.
		static std::optional<glm::mat4> GetMatrixBelowOwner(ConstEntity owner, ConstEntity entity)
		{
			std::vector<ConstEntity> chain;
			ConstEntity current = entity;
			while (current.IsValid() && current != owner)
			{
				chain.push_back(current);
				current = current.GetParent();
			}
			if (!current.IsValid())
				return std::nullopt;

			glm::mat4 matrix(1.0f);
			for (auto link = chain.rbegin(); link != chain.rend(); ++link)
				matrix = matrix * TransformSystem::ComputeLocalMatrix(link->GetComponent<TransformComponent>());
			return matrix;
		}

		// A collider's placement in its body as translation, rotation and scale along the collider's axes (the lengths of the
		// matrix's columns). The rotation is TransformSystem::DecomposeMatrix's of the matrix with unit columns: its minimum
		// scale is for one Transform, and the scales of nested entities multiply below it (0.01 under 0.01), which is valid
		// content; whether a shape takes a tiny scale is PhysicsShape::Create's question (Shape::IsValidScale).
		static Result<TransformDecomposition> DecomposePlacement(const glm::mat4& placement)
		{
			glm::mat4 unit = placement;
			glm::vec3 lengths(1.0f);
			for (glm::length_t axis = 0; axis < 3; ++axis)
			{
				lengths[axis] = glm::length(glm::vec3(placement[axis]));
				if (!std::isfinite(lengths[axis]) || lengths[axis] <= 0.0f)
					return MakeError(ErrorCode::InvalidArgument, "axis {} of the placement has the scale {}", axis, lengths[axis]);
				unit[axis] = glm::vec4(glm::vec3(placement[axis]) / lengths[axis], 0.0f);
			}
			ENGINE_TRY_ASSIGN(TransformDecomposition decomposed, TransformSystem::DecomposeMatrix(unit));
			// A mirroring placement comes back as a negative X scale.
			decomposed.Scale = glm::vec3(std::copysign(lengths.x, decomposed.Scale.x), lengths.y, lengths.z);
			return decomposed;
		}

		// The collider's frame within the entity: its Offset, then its Rotation (spheres have none; meshes are in entity space).
		static glm::mat4 GetColliderLocalMatrix(ConstEntity entity, PhysicsColliderType type)
		{
			glm::vec3 offset(0.0f);
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			switch (type)
			{
				case PhysicsColliderType::Box:
				{
					const BoxColliderComponent& box = entity.GetComponent<BoxColliderComponent>();
					offset = box.Offset;
					rotation = box.Rotation;
					break;
				}
				case PhysicsColliderType::Sphere:
					offset = entity.GetComponent<SphereColliderComponent>().Offset;
					break;
				case PhysicsColliderType::Capsule:
				{
					const CapsuleColliderComponent& capsule = entity.GetComponent<CapsuleColliderComponent>();
					offset = capsule.Offset;
					rotation = capsule.Rotation;
					break;
				}
				case PhysicsColliderType::Mesh:
					break;
			}
			glm::mat4 matrix = glm::mat4_cast(rotation);
			matrix[3] = glm::vec4(offset, 1.0f);
			return matrix;
		}

		// The mesh a MeshCollider uses: its own, else the entity's MeshRenderer mesh; null when neither is set.
		static AssetHandle ResolveColliderMesh(ConstEntity entity)
		{
			const MeshColliderComponent& collider = entity.GetComponent<MeshColliderComponent>();
			if (collider.Mesh.IsValid())
				return collider.Mesh.GetHandle();
			if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>())
				return renderer->Mesh.GetHandle();
			return AssetHandle();
		}

		// The vertex positions of `mesh`.
		static std::vector<glm::vec3> GetMeshPositions(const MeshData& mesh)
		{
			std::vector<glm::vec3> positions;
			positions.reserve(mesh.Vertices.size());
			for (const MeshVertex& vertex : mesh.Vertices)
				positions.push_back(vertex.Position);
			return positions;
		}

		// A mesh's collider geometry: its convex hull or its triangles.
		static PhysicsShapeGeometry MakeMeshGeometry(const MeshData& mesh, bool convex)
		{
			if (convex)
				return ConvexHullShapeGeometry{ .Points = GetMeshPositions(mesh) };
			return MeshShapeGeometry{ .Vertices = GetMeshPositions(mesh), .Indices = mesh.Indices };
		}

	}

	PhysicsMeshShapeCache::PhysicsMeshShapeCache()
		: m_State(CreateScope<State>())
	{
	}

	PhysicsMeshShapeCache::~PhysicsMeshShapeCache() = default;

	Result<Ref<const PhysicsShape>> PhysicsMeshShapeCache::GetOrCreate(AssetManager& assets, AssetHandle mesh, bool convex, const glm::vec3& scale)
	{
		// Loading first: the version is 0 until the first load.
		const AssetRef<MeshData> data = assets.GetOrPlaceholder<MeshData>(mesh);
		const MeshShapeKey key{ .Mesh = mesh,
			.Version = assets.GetVersion(mesh),
			.Convex = convex,
			.ScaleX = std::bit_cast<uint32_t>(scale.x),
			.ScaleY = std::bit_cast<uint32_t>(scale.y),
			.ScaleZ = std::bit_cast<uint32_t>(scale.z) };
		if (const auto found = m_State->Entries.find(key); found != m_State->Entries.end())
			return found->second;

		BodyShapeDescription description;
		description.Colliders.push_back(ColliderShapeDescription{ .Geometry = Utils::MakeMeshGeometry(*data, convex), .Scale = scale });
		ENGINE_TRY_ASSIGN(Ref<const PhysicsShape> shape, PhysicsShape::Create(description));
		m_State->Entries.emplace(key, shape);
		return shape;
	}

	void PhysicsMeshShapeCache::Prune()
	{
		std::erase_if(m_State->Entries, [](const auto& entry)
		{
			return entry.second.use_count() == 1;
		});
	}

	void PhysicsMeshShapeCache::Clear()
	{
		m_State->Entries.clear();
	}

	size_t PhysicsMeshShapeCache::GetSize() const
	{
		return m_State->Entries.size();
	}

	PhysicsComposition ComposePhysicsBodies(const Scene& scene, const PhysicsLayerTable& layers, uint32_t maxBodies)
	{
		PhysicsComposition composition;
		// Lookups only (§5.1): output order comes from the canonical walk.
		std::unordered_map<UUID, CompositionEntity> entities;
		std::unordered_map<UUID, size_t> solidPlans; // owner -> its RigidBody or implicit static plan
		std::vector<size_t> rigidBodyPlans;

		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			const ConstEntity parent = entity.GetParent();
			const auto parentInfo = parent.IsValid() ? entities.find(parent.GetUUID()) : entities.end();
			// A disabled entity and its subtree make nothing (§5.2); its children find no parent entry and skip too.
			if (entity.HasComponent<DisabledTag>() || (parent.IsValid() && parentInfo == entities.end()))
				continue;

			CompositionEntity info;
			if (parentInfo != entities.end())
			{
				info = parentInfo->second;
				info.MovingAncestor = parentInfo->second.MovingAncestor || parentInfo->second.Moving;
				info.Moving = false;
			}

			std::vector<PhysicsColliderPlan> colliders;
			Utils::AppendOwnColliders(entity, colliders);
			const bool hasSolid = std::any_of(colliders.begin(), colliders.end(), [](const PhysicsColliderPlan& collider)
			{
				return !collider.IsTrigger;
			});
			const RigidBodyComponent* rigidBody = entity.TryGetComponent<RigidBodyComponent>();
			const CharacterControllerComponent* character = rigidBody == nullptr ? entity.TryGetComponent<CharacterControllerComponent>() : nullptr;

			if (rigidBody != nullptr)
			{
				info.Carrier = id;
				info.CarrierIsCharacter = false;
				info.Moving = rigidBody->Type != BodyType::Static;
				info.InheritedLayer = Utils::ResolveLayer(layers, entity, "RigidBody", rigidBody->Layer, composition.Diagnostics);

				PhysicsBodyPlan plan;
				plan.Owner = id;
				plan.Origin = PhysicsBodyOrigin::RigidBody;
				plan.MotionType = Utils::ToPhysicsMotionType(rigidBody->Type);
				plan.Layer = info.InheritedLayer;
				plan.CollisionGroup = id;
				plan.Colliders = colliders; // its own colliders, triggers included (they must agree)
				solidPlans.emplace(id, composition.Bodies.size());
				rigidBodyPlans.push_back(composition.Bodies.size());
				composition.Bodies.push_back(std::move(plan));
			}
			else if (character != nullptr)
			{
				info.Carrier = id;
				info.CarrierIsCharacter = true;
				info.Moving = true;

				PhysicsBodyPlan plan;
				plan.Owner = id;
				plan.Origin = PhysicsBodyOrigin::Character;
				plan.MotionType = PhysicsMotionType::Kinematic;
				plan.Layer = Utils::ResolveLayer(layers, entity, "CharacterController", character->Layer, composition.Diagnostics);
				plan.CollisionGroup = id;
				composition.Bodies.push_back(std::move(plan));
			}

			// Solid colliders of an entity without its own RigidBody.
			if (rigidBody == nullptr && hasSolid)
			{
				// The plan index of `owner`, which an ancestor (or this entity) registered before its descendants are visited.
				const auto findPlan = [&solidPlans](UUID owner) -> std::optional<size_t>
				{
					const auto found = solidPlans.find(owner);
					ENGINE_CORE_VERIFY(found != solidPlans.end(), "PhysicsComposition: a collider's body owner has no plan");
					return found != solidPlans.end() ? std::optional<size_t>(found->second) : std::nullopt;
				};
				std::optional<size_t> target;
				if (info.Carrier.IsValid() && !info.CarrierIsCharacter)
				{
					target = findPlan(info.Carrier);
				}
				else if (!info.Carrier.IsValid())
				{
					if (!info.ImplicitStaticOwner.IsValid())
					{
						info.ImplicitStaticOwner = id;
						PhysicsBodyPlan plan;
						plan.Owner = id;
						plan.Origin = PhysicsBodyOrigin::ImplicitStatic;
						plan.MotionType = PhysicsMotionType::Static;
						plan.Layer = 0;
						solidPlans.emplace(id, composition.Bodies.size());
						composition.Bodies.push_back(std::move(plan));
					}
					target = findPlan(info.ImplicitStaticOwner);
				}
				// Under a CharacterController the capsule is the shape: solid colliders there make nothing.
				if (target.has_value())
				{
					std::vector<PhysicsColliderPlan>& bodyColliders = composition.Bodies[*target].Colliders;
					std::copy_if(colliders.begin(), colliders.end(), std::back_inserter(bodyColliders), [](const PhysicsColliderPlan& collider)
					{
						return !collider.IsTrigger;
					});
				}
			}

			// Trigger colliders of an entity without its own RigidBody: its implicit sensor body.
			if (rigidBody == nullptr && std::any_of(colliders.begin(), colliders.end(), [](const PhysicsColliderPlan& collider)
			{
				return collider.IsTrigger;
			}))
			{
				PhysicsBodyPlan plan;
				plan.Owner = id;
				plan.Origin = PhysicsBodyOrigin::ImplicitSensor;
				plan.MotionType = PhysicsMotionType::Kinematic;
				plan.IsSensor = true;
				plan.Layer = info.InheritedLayer;
				plan.CollisionGroup = info.Carrier;
				std::copy_if(colliders.begin(), colliders.end(), std::back_inserter(plan.Colliders), [](const PhysicsColliderPlan& collider)
				{
					return collider.IsTrigger;
				});
				composition.Bodies.push_back(std::move(plan));
			}

			entities.emplace(id, info);
		}

		for (const size_t index : rigidBodyPlans)
			Utils::FinishRigidBodyPlan(scene, composition.Bodies[index], composition.Diagnostics, entities);
		Utils::CheckColliderScales(scene, composition.Bodies, composition.Diagnostics);
		Utils::ApplyBodyLimit(scene, composition.Bodies, maxBodies, composition.Diagnostics);

		std::sort(composition.Diagnostics.begin(), composition.Diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return std::tie(a.Entity, a.Code, a.Subject) < std::tie(b.Entity, b.Code, b.Subject);
		});
		return composition;
	}

	Result<BodyShapeDescription> DescribePhysicsBodyShape(const Scene& scene, const PhysicsBodyPlan& plan, AssetManager* assets,
		PhysicsMeshShapeCache* meshShapes)
	{
		const ConstEntity owner = scene.FindEntityByID(plan.Owner);
		if (!owner.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "the body owner {} is not an entity of scene '{}'", plan.Owner, scene.GetName());

		const glm::mat4 bakedScale = glm::scale(glm::mat4(1.0f), TransformSystem::GetWorldScale(owner));
		BodyShapeDescription description;
		description.Colliders.reserve(plan.Colliders.size());
		for (size_t index = 0; index < plan.Colliders.size(); ++index)
		{
			const PhysicsColliderPlan& collider = plan.Colliders[index];
			const ConstEntity entity = scene.FindEntityByID(collider.Entity);
			if (!entity.IsValid())
				return MakeError(ErrorCode::InvalidArgument, "collider {} of the body of {} is not an entity of scene '{}'", collider.Entity, plan.Owner,
					scene.GetName());
			const std::optional<glm::mat4> below = Utils::GetMatrixBelowOwner(owner, entity);
			if (!below.has_value())
			{
				return MakeError(ErrorCode::InvalidArgument, "collider {} of the body of {} is not below its owner in scene '{}'", collider.Entity, plan.Owner,
					scene.GetName());
			}

			const glm::mat4 placement = bakedScale * *below * Utils::GetColliderLocalMatrix(entity, collider.Type);
			const Result<TransformDecomposition> decomposed = Utils::DecomposePlacement(placement);
			if (!decomposed)
			{
				return Utils::MakeShapeError(collider.Entity,
					std::format("the {} of {} cannot be placed in its body: {}", Utils::GetColliderComponentName(collider.Type), Utils::DescribeEntity(entity),
						decomposed.error().GetMessageText()));
			}

			ColliderShapeDescription shape;
			shape.Position = decomposed->Translation;
			shape.Rotation = decomposed->Rotation;
			shape.Scale = decomposed->Scale;
			shape.UserData = static_cast<uint32_t>(index);
			switch (collider.Type)
			{
				case PhysicsColliderType::Box:
					shape.Geometry = BoxShapeGeometry{ .HalfExtents = entity.GetComponent<BoxColliderComponent>().HalfExtents };
					break;
				case PhysicsColliderType::Sphere:
					shape.Geometry = SphereShapeGeometry{ .Radius = entity.GetComponent<SphereColliderComponent>().Radius };
					break;
				case PhysicsColliderType::Capsule:
				{
					const CapsuleColliderComponent& capsule = entity.GetComponent<CapsuleColliderComponent>();
					shape.Geometry = CapsuleShapeGeometry{ .HalfHeight = capsule.HalfHeight, .Radius = capsule.Radius };
					break;
				}
				case PhysicsColliderType::Mesh:
				{
					const AssetHandle mesh = Utils::ResolveColliderMesh(entity);
					if (!mesh.IsValid())
					{
						return Utils::MakeShapeError(collider.Entity,
							std::format("the MeshCollider of {} has no mesh: its Mesh is null and the entity has no MeshRenderer mesh",
								Utils::DescribeEntity(entity)));
					}
					if (assets == nullptr)
					{
						return Utils::MakeShapeError(collider.Entity,
							std::format("the MeshCollider of {} cannot load its mesh {} without an asset manager", Utils::DescribeEntity(entity), mesh));
					}
					const bool convex = entity.GetComponent<MeshColliderComponent>().Convex;
					if (meshShapes != nullptr)
					{
						Result<Ref<const PhysicsShape>> cached = meshShapes->GetOrCreate(*assets, mesh, convex, shape.Scale);
						if (!cached)
						{
							return std::unexpected(std::move(cached)
									.error()
									.WithLocation(Utils::AtEntity(collider.Entity))
									.WithContext(std::format("while building the MeshCollider of {}", Utils::DescribeEntity(entity))));
						}
						shape.Geometry = SharedShapeGeometry{ .Shape = std::move(*cached) };
						shape.Scale = glm::vec3(1.0f);
					}
					else
					{
						shape.Geometry = Utils::MakeMeshGeometry(*assets->GetOrPlaceholder<MeshData>(mesh), convex);
					}
					break;
				}
			}
			// §9.2: spheres and capsules under a non-uniform scale use its largest axis (PHYSICS_NONUNIFORM_SCALE).
			if (collider.Type == PhysicsColliderType::Sphere || collider.Type == PhysicsColliderType::Capsule)
			{
				const glm::vec3 magnitude = glm::abs(shape.Scale);
				shape.Scale = glm::vec3(std::max({ magnitude.x, magnitude.y, magnitude.z }));
			}
			description.Colliders.push_back(std::move(shape));
		}
		return description;
	}

	std::string_view PhysicsBodyOriginToString(PhysicsBodyOrigin origin)
	{
		switch (origin)
		{
			case PhysicsBodyOrigin::RigidBody:      return "RigidBody";
			case PhysicsBodyOrigin::ImplicitStatic: return "ImplicitStatic";
			case PhysicsBodyOrigin::ImplicitSensor: return "ImplicitSensor";
			case PhysicsBodyOrigin::Character:      return "Character";
		}

		ENGINE_CORE_ASSERT(false, "Unknown PhysicsBodyOrigin {}", std::to_underlying(origin));
		return "Unknown";
	}

}
