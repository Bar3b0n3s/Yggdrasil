#include "EnginePCH.h"
#include "Engine/Scene/Private/Physics2DComposition.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/TilemapCollision.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/PolygonCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TilemapColliderComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DValidation.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// The 2D composition in one canonical walk, as ComposePhysicsBodies (PhysicsComposition.cpp): every entity inherits from
// its parent what the rules need (its nearest RigidBody2D, the layer implicit bodies take, the owner of an implicit static
// compound, whether an ancestor moves), so each plan is complete when its owner is visited. A TilemapCollider's boxes are
// laid by DescribePhysics2DBodyShape, the only place that loads its tilemap.

namespace Engine {

	namespace {

		// What the walk carries from an entity to its children.
		struct Composition2DEntity
		{
			// The nearest entity at or above with a RigidBody2D; invalid for none.
			UUID Carrier{};
			// The layer of the nearest RigidBody2D at or above (implicit bodies take it); 0 without one.
			uint32_t InheritedLayer = 0;
			// Without a carrier: the topmost entity at or above with solid 2D colliders, the owner of their implicit static body.
			UUID ImplicitStaticOwner{};
			// A strict ancestor owns a Kinematic or Dynamic RigidBody2D.
			bool MovingAncestor = false;
			// The entity itself owns a Kinematic or Dynamic RigidBody2D.
			bool Moving = false;
		};

	}

	namespace Utils {

		// The entity's 2D collider components in BuiltinComponents order (the order of sub-shape user data).
		static void AppendOwn2DColliders(ConstEntity entity, std::vector<Physics2DColliderPlan>& colliders)
		{
			const UUID id = entity.GetUUID();
			if (const BoxCollider2DComponent* box = entity.TryGetComponent<BoxCollider2DComponent>())
				colliders.push_back(Physics2DColliderPlan{ .Entity = id, .Type = Physics2DColliderType::Box, .IsTrigger = box->IsTrigger });
			if (const CircleCollider2DComponent* circle = entity.TryGetComponent<CircleCollider2DComponent>())
				colliders.push_back(Physics2DColliderPlan{ .Entity = id, .Type = Physics2DColliderType::Circle, .IsTrigger = circle->IsTrigger });
			if (const PolygonCollider2DComponent* polygon = entity.TryGetComponent<PolygonCollider2DComponent>())
				colliders.push_back(Physics2DColliderPlan{ .Entity = id, .Type = Physics2DColliderType::Polygon, .IsTrigger = polygon->IsTrigger });
			if (const TilemapColliderComponent* tilemap = entity.TryGetComponent<TilemapColliderComponent>())
				colliders.push_back(Physics2DColliderPlan{ .Entity = id, .Type = Physics2DColliderType::Tilemap, .IsTrigger = tilemap->IsTrigger });
		}

		// "'Name' (<uuid>)", how diagnostics name an entity.
		static std::string Describe2DEntity(ConstEntity entity)
		{
			return std::format("'{}' ({})", entity.GetName(), entity.GetUUID());
		}

		static PhysicsDiagnostic Make2DDiagnostic(std::string_view code, DiagnosticSeverity severity, UUID entity, std::string_view component,
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

		// The layer named `name`, or layer 0 with PHYSICS_UNKNOWN_LAYER on the entity's RigidBody2D.
		static uint32_t Resolve2DLayer(const PhysicsLayerTable& layers, ConstEntity entity, const std::string& name, std::vector<PhysicsDiagnostic>& diagnostics)
		{
			if (const std::optional<uint32_t> index = layers.FindLayer(name); index.has_value())
				return *index;

			const std::vector<std::string> suggestions = FuzzySuggest(name, layers.GetNames());
			std::string hint = MakeDidYouMeanHint(suggestions);
			if (hint.empty())
				hint = "declare the layer in the project's physics settings (PhysicsSettings.Layers), which the 2D world shares, or use one of its layers";
			diagnostics.push_back(Make2DDiagnostic(PhysicsUnknownLayerCode, DiagnosticSeverity::Error, entity.GetUUID(), "RigidBody2D", "Layer",
				std::format("the RigidBody2D of {} names the layer '{}', which the project's physics settings do not declare; it uses layer '{}'",
					Describe2DEntity(entity), name, DefaultPhysicsLayerName),
				std::move(hint)));
			return 0;
		}

		// The checks of a RigidBody2D plan once its colliders are known.
		static void Finish2DRigidBodyPlan(const Scene& scene, Physics2DBodyPlan& plan, std::vector<PhysicsDiagnostic>& diagnostics,
			const std::unordered_map<UUID, Composition2DEntity>& entities)
		{
			if (plan.Colliders.empty())
			{
				plan.IsCreatable = false;
				return;
			}

			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			const RigidBody2DComponent& body = owner.GetComponent<RigidBody2DComponent>();
			const auto triggers = static_cast<size_t>(std::count_if(plan.Colliders.begin(), plan.Colliders.end(), [](const Physics2DColliderPlan& collider)
			{
				return collider.IsTrigger;
			}));
			if (triggers != 0 && triggers != plan.Colliders.size())
			{
				diagnostics.push_back(Make2DDiagnostic(PhysicsMixedTriggerCode, DiagnosticSeverity::Error, plan.Owner, "", "IsTrigger",
					std::format("the {} 2D colliders of the body of {} disagree on IsTrigger ({} triggers, {} solid); the body is not created",
						plan.Colliders.size(), Describe2DEntity(owner), triggers, plan.Colliders.size() - triggers),
					"make every 2D collider of the body a trigger or none of them; a trigger on a child entity without a RigidBody2D gets a sensor "
					"body of its own"));
				plan.IsCreatable = false;
				return;
			}

			plan.IsSensor = triggers != 0;
			if (plan.IsSensor && body.Type == BodyType::Dynamic)
			{
				diagnostics.push_back(Make2DDiagnostic(PhysicsDynamicTriggerCode, DiagnosticSeverity::Error, plan.Owner, "RigidBody2D", "Type",
					std::format("the RigidBody2D of {} is Dynamic but its colliders are triggers, and a trigger cannot be simulated; the body is not "
								"created",
						Describe2DEntity(owner)),
					"set RigidBody2D.Type to Kinematic or Static, or clear IsTrigger on its colliders"));
				plan.IsCreatable = false;
			}
			if (plan.IsSensor && body.Type == BodyType::Static)
				plan.MotionType = PhysicsMotionType::Kinematic; // as the 3D world: triggers are Kinematic and kept active

			if (body.Type != BodyType::Dynamic)
				return;
			const auto ownerInfo = entities.find(plan.Owner);
			ENGINE_CORE_VERIFY(ownerInfo != entities.end(), "Physics2DComposition: the owner of a RigidBody2D plan was not visited");
			if (ownerInfo != entities.end() && ownerInfo->second.MovingAncestor)
			{
				diagnostics.push_back(Make2DDiagnostic(PhysicsDynamicUnderMovingParentCode, DiagnosticSeverity::Warning, plan.Owner, "RigidBody2D", "Type",
					std::format("the Dynamic 2D body of {} is below an entity that moves on its own; it simulates in world space and does not "
								"follow its parent",
						Describe2DEntity(owner)),
					"move the entity out from under the moving body, or make it Kinematic to have it carried"));
			}
		}

		// PHYSICS_NONUNIFORM_SCALE for each entity with a circle collider in a body under a world scale whose X and Y differ.
		static void Check2DColliderScales(const Scene& scene, const std::vector<Physics2DBodyPlan>& bodies, std::vector<PhysicsDiagnostic>& diagnostics)
		{
			constexpr float RelativeTolerance = 1.0e-5f;
			std::set<UUID> reported;
			for (const Physics2DBodyPlan& plan : bodies)
			{
				for (const Physics2DColliderPlan& collider : plan.Colliders)
				{
					if (collider.Type != Physics2DColliderType::Circle || reported.contains(collider.Entity))
						continue;
					const ConstEntity entity = scene.FindEntityByID(collider.Entity);
					const glm::vec3 scale = TransformSystem::GetWorldScale(entity);
					const float largest = std::max(std::abs(scale.x), std::abs(scale.y));
					const float smallest = std::min(std::abs(scale.x), std::abs(scale.y));
					if (largest - smallest <= largest * RelativeTolerance)
						continue;
					reported.insert(collider.Entity);
					diagnostics.push_back(Make2DDiagnostic(PhysicsNonuniformScaleCode, DiagnosticSeverity::Warning, collider.Entity, "CircleCollider2D", "",
						std::format("the CircleCollider2D of {} is under the world scale ({}, {}) in the plane, which is not uniform; it uses the larger "
									"of the two",
							Describe2DEntity(entity), scale.x, scale.y),
						"give the entity and its ancestors the same scale in X and Y, or use a BoxCollider2D or PolygonCollider2D"));
				}
			}
		}

		// PHYSICS2D_OFF_PLANE for each creatable body's owner, and each entity whose colliders it gathers, rotated out of the
		// plane.
		static void CheckPlane(const Scene& scene, const std::vector<Physics2DBodyPlan>& bodies, std::vector<PhysicsDiagnostic>& diagnostics)
		{
			std::set<UUID> checked;
			const auto check = [&scene, &diagnostics, &checked](UUID id)
			{
				if (!checked.insert(id).second)
					return;
				const ConstEntity entity = scene.FindEntityByID(id);
				if (!IsOffPlaneRotation(TransformSystem::GetWorldRotation(entity)))
					return;
				diagnostics.push_back(Make2DDiagnostic(Physics2DOffPlaneCode, DiagnosticSeverity::Warning, id, "Transform", "Rotation",
					std::format("the 2D physics of {} is rotated out of the XY plane (a rotation about X or Y); its body takes only the rotation "
								"about Z",
						Describe2DEntity(entity)),
					"rotate the entity and its ancestors about Z only"));
			};
			for (const Physics2DBodyPlan& plan : bodies)
			{
				if (!plan.IsCreatable)
					continue;
				check(plan.Owner);
				for (const Physics2DColliderPlan& collider : plan.Colliders)
					check(collider.Entity);
			}
		}

		// Marks the creatable bodies beyond `maxBodies` (canonical order) as refused, one diagnostic per refused owner.
		static void Apply2DBodyLimit(const Scene& scene, std::vector<Physics2DBodyPlan>& bodies, uint32_t maxBodies, std::vector<PhysicsDiagnostic>& diagnostics)
		{
			uint32_t creatable = 0;
			UUID reported{};
			for (Physics2DBodyPlan& plan : bodies)
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
				PhysicsDiagnostic diagnostic = Make2DDiagnostic(PhysicsLimitExceededCode, DiagnosticSeverity::Error, plan.Owner, "", "",
					std::format("the 2D body of {} is not created: the scene makes more than {} 2D physics bodies, the most a world holds, and the "
								"bodies beyond them in canonical order are refused",
						Describe2DEntity(scene.FindEntityByID(plan.Owner)), maxBodies),
					"merge 2D colliders into compounds (one RigidBody2D on a parent gathers its collider-only children) or remove bodies");
				diagnostic.Subject = "bodies";
				diagnostics.push_back(std::move(diagnostic));
			}
		}

		static std::unexpected<Error> Make2DShapeError(UUID entity, std::string message)
		{
			ErrorLocation location;
			location.Entity = entity;
			return std::unexpected(Error(ErrorCode::Validation, std::format("{}: {}", PhysicsInvalidShapeCode, std::move(message))).WithLocation(location));
		}

		// The product of the local matrices from the owner's child down to `entity` (the identity for the owner itself); nullopt
		// when `entity` is not `owner` or one of its descendants.
		static std::optional<glm::mat4> GetMatrixBelow2DOwner(ConstEntity owner, ConstEntity entity)
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

		// The collider's frame within its entity: its Offset in the XY plane, then its Angle (degrees) about Z (circles have
		// none; a tilemap's cells lie in the entity's own plane).
		static glm::mat4 GetCollider2DLocalMatrix(ConstEntity entity, Physics2DColliderType type)
		{
			glm::vec2 offset(0.0f);
			float angle = 0.0f;
			switch (type)
			{
				case Physics2DColliderType::Box:
				{
					const BoxCollider2DComponent& box = entity.GetComponent<BoxCollider2DComponent>();
					offset = box.Offset;
					angle = box.Angle;
					break;
				}
				case Physics2DColliderType::Circle:
					offset = entity.GetComponent<CircleCollider2DComponent>().Offset;
					break;
				case Physics2DColliderType::Polygon:
				{
					const PolygonCollider2DComponent& polygon = entity.GetComponent<PolygonCollider2DComponent>();
					offset = polygon.Offset;
					angle = polygon.Angle;
					break;
				}
				case Physics2DColliderType::Tilemap:
					return glm::mat4(1.0f);
			}
			glm::mat4 matrix = glm::mat4_cast(TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 0.0f, angle)));
			matrix[3] = glm::vec4(offset, 0.0f, 1.0f);
			return matrix;
		}

		// The PHYSICS_INVALID_SHAPE error of the TilemapCollider at `index` in its plan, named by that index (Utils::
		// ParseColliderIndex reads it back) and located at its entity.
		static std::unexpected<Error> MakeTilemapShapeError(ConstEntity entity, size_t index, std::string reason, std::string hint)
		{
			ErrorLocation location;
			location.Entity = entity.GetUUID();
			return std::unexpected(Error(ErrorCode::Validation,
				std::format("{}: collider {}: the TilemapCollider of {} {}", PhysicsInvalidShapeCode, index, Describe2DEntity(entity), std::move(reason)))
					.WithLocation(location)
					.WithHint(std::move(hint)));
		}

		// The merged boxes of the TilemapCollider at `index` of its plan, on `entity`: its tilemap loaded through `assets`.
		static Result<std::pair<AssetRef<TilemapData>, std::vector<TilemapBox>>> LoadTilemapColliderBoxes(ConstEntity entity, size_t index,
			AssetManager* assets)
		{
			const AssetHandle handle = GetTilemapColliderTilemap(entity);
			if (!handle.IsValid())
			{
				return MakeTilemapShapeError(entity, index, "has no tilemap: neither its Tilemap nor a TilemapRenderer on its entity names one",
					"set the TilemapCollider's Tilemap, or give its entity a TilemapRenderer that draws the tilemap");
			}
			if (assets == nullptr)
			{
				return MakeTilemapShapeError(entity, index, std::format("cannot load its tilemap {}: no asset manager loads tilemaps here", handle),
					"tilemap colliders need the session's or the editor's asset manager");
			}
			Result<AssetRef<Asset>> loaded = assets->Load(handle);
			if (!loaded)
			{
				return MakeTilemapShapeError(entity, index, std::format("cannot load its tilemap {}: {}", handle, loaded.error().GetMessageText()),
					"name an existing .tilemap that imports, or fix the one it names (project.validate reports why it fails)");
			}
			AssetRef<TilemapData> tilemap = AssetCast<TilemapData>(*loaded);
			if (tilemap == nullptr)
			{
				return MakeTilemapShapeError(entity, index, std::format("names {}, which is a {}, not a Tilemap", handle, AssetTypeToString((*loaded)->GetAssetType())),
					"name a .tilemap asset");
			}
			std::vector<TilemapBox> boxes = MergeTilemapBoxes(*tilemap);
			if (boxes.size() > MaxTilemapColliderBoxes)
			{
				return MakeTilemapShapeError(entity, index,
					std::format("makes {} boxes of its tilemap {}, more than the {} one collider holds", boxes.size(), handle, MaxTilemapColliderBoxes),
					"split the tilemap's solid cells into several tilemaps, or make large solid areas rectangular so they merge into fewer boxes");
			}
			return std::make_pair(std::move(tilemap), std::move(boxes));
		}

		// A placement as translation, rotation and the scale along its axes (the lengths of its columns), as the 3D
		// composition decomposes a collider's placement.
		static Result<TransformDecomposition> Decompose2DPlacement(const glm::mat4& placement)
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

	}

	bool HasPhysics2DComponent(ConstEntity entity)
	{
		return entity.HasComponent<RigidBody2DComponent>() || entity.HasComponent<BoxCollider2DComponent>() || entity.HasComponent<CircleCollider2DComponent>()
			|| entity.HasComponent<PolygonCollider2DComponent>() || entity.HasComponent<TilemapColliderComponent>();
	}

	bool HasPhysics3DComponent(ConstEntity entity)
	{
		return entity.HasComponent<RigidBodyComponent>() || entity.HasComponent<BoxColliderComponent>() || entity.HasComponent<SphereColliderComponent>()
			|| entity.HasComponent<CapsuleColliderComponent>() || entity.HasComponent<MeshColliderComponent>() || entity.HasComponent<CharacterControllerComponent>();
	}

	bool IsOffPlaneRotation(const glm::quat& rotation)
	{
		const float swing = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y);
		return !(swing <= PhysicsUnitRotationTolerance);
	}

	Physics2DComposition ComposePhysics2DBodies(const Scene& scene, const PhysicsLayerTable& layers, uint32_t maxBodies)
	{
		Physics2DComposition composition;
		// Lookups only: output order comes from the canonical walk.
		std::unordered_map<UUID, Composition2DEntity> entities;
		std::unordered_map<UUID, size_t> solidPlans; // owner -> its RigidBody2D or implicit static plan
		std::vector<size_t> rigidBodyPlans;

		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			const ConstEntity parent = entity.GetParent();
			const auto parentInfo = parent.IsValid() ? entities.find(parent.GetUUID()) : entities.end();
			// A disabled entity and its subtree make nothing; its children find no parent entry and skip too.
			if (entity.HasComponent<DisabledTag>() || (parent.IsValid() && parentInfo == entities.end()))
				continue;

			Composition2DEntity info;
			if (parentInfo != entities.end())
			{
				info = parentInfo->second;
				info.MovingAncestor = parentInfo->second.MovingAncestor || parentInfo->second.Moving;
				info.Moving = false;
			}

			// One world per entity: a mixed entity counts as holding no 2D component.
			const bool has2D = HasPhysics2DComponent(entity);
			if (has2D && HasPhysics3DComponent(entity))
			{
				composition.Diagnostics.push_back(Utils::Make2DDiagnostic(Physics2DMixedWorldsCode, DiagnosticSeverity::Error, id, "", "",
					std::format("{} holds both 2D and 3D physics components; it stays in the 3D world and its 2D components make no 2D body",
						Utils::Describe2DEntity(entity)),
					"remove the components of the world it does not belong to, or split it into two entities"));
				entities.emplace(id, info);
				continue;
			}
			if (!has2D)
			{
				entities.emplace(id, info);
				continue;
			}

			std::vector<Physics2DColliderPlan> colliders;
			Utils::AppendOwn2DColliders(entity, colliders);
			const bool hasSolid = std::ranges::any_of(colliders, [](const Physics2DColliderPlan& collider)
			{
				return !collider.IsTrigger;
			});
			const bool hasTrigger = std::ranges::any_of(colliders, [](const Physics2DColliderPlan& collider)
			{
				return collider.IsTrigger;
			});
			const RigidBody2DComponent* rigidBody = entity.TryGetComponent<RigidBody2DComponent>();

			if (rigidBody != nullptr)
			{
				info.Carrier = id;
				info.Moving = rigidBody->Type != BodyType::Static;
				info.InheritedLayer = Utils::Resolve2DLayer(layers, entity, rigidBody->Layer, composition.Diagnostics);

				Physics2DBodyPlan plan;
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
			else if (hasSolid)
			{
				std::optional<size_t> target;
				if (!info.Carrier.IsValid() && !info.ImplicitStaticOwner.IsValid())
				{
					info.ImplicitStaticOwner = id;
					Physics2DBodyPlan plan;
					plan.Owner = id;
					plan.Origin = PhysicsBodyOrigin::ImplicitStatic;
					plan.MotionType = PhysicsMotionType::Static;
					solidPlans.emplace(id, composition.Bodies.size());
					composition.Bodies.push_back(std::move(plan));
				}
				const UUID owner = info.Carrier.IsValid() ? info.Carrier : info.ImplicitStaticOwner;
				const auto found = solidPlans.find(owner);
				ENGINE_CORE_VERIFY(found != solidPlans.end(), "Physics2DComposition: a 2D collider's body owner has no plan");
				if (found != solidPlans.end())
					target = found->second;
				if (target.has_value())
				{
					std::vector<Physics2DColliderPlan>& bodyColliders = composition.Bodies[*target].Colliders;
					std::ranges::copy_if(colliders, std::back_inserter(bodyColliders), [](const Physics2DColliderPlan& collider)
					{
						return !collider.IsTrigger;
					});
				}
			}

			// Trigger colliders of an entity without its own RigidBody2D: its implicit sensor body.
			if (rigidBody == nullptr && hasTrigger)
			{
				Physics2DBodyPlan plan;
				plan.Owner = id;
				plan.Origin = PhysicsBodyOrigin::ImplicitSensor;
				plan.MotionType = PhysicsMotionType::Kinematic;
				plan.IsSensor = true;
				plan.Layer = info.InheritedLayer;
				plan.CollisionGroup = info.Carrier;
				std::ranges::copy_if(colliders, std::back_inserter(plan.Colliders), [](const Physics2DColliderPlan& collider)
				{
					return collider.IsTrigger;
				});
				composition.Bodies.push_back(std::move(plan));
			}

			entities.emplace(id, info);
		}

		for (const size_t index : rigidBodyPlans)
			Utils::Finish2DRigidBodyPlan(scene, composition.Bodies[index], composition.Diagnostics, entities);
		Utils::Check2DColliderScales(scene, composition.Bodies, composition.Diagnostics);
		Utils::Apply2DBodyLimit(scene, composition.Bodies, maxBodies, composition.Diagnostics);
		Utils::CheckPlane(scene, composition.Bodies, composition.Diagnostics);

		std::sort(composition.Diagnostics.begin(), composition.Diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return std::tie(a.Entity, a.Code, a.Subject) < std::tie(b.Entity, b.Code, b.Subject);
		});
		return composition;
	}

	AssetHandle GetTilemapColliderTilemap(ConstEntity entity)
	{
		const TilemapColliderComponent* collider = entity.TryGetComponent<TilemapColliderComponent>();
		if (collider == nullptr)
			return AssetHandle();
		if (collider->Tilemap.IsValid())
			return collider->Tilemap.GetHandle();
		const TilemapRendererComponent* renderer = entity.TryGetComponent<TilemapRendererComponent>();
		return renderer != nullptr ? renderer->Tilemap.GetHandle() : AssetHandle();
	}

	Result<Body2DShapeDescription> DescribePhysics2DBodyShape(const Scene& scene, const Physics2DBodyPlan& plan, AssetManager* assets)
	{
		const ConstEntity owner = scene.FindEntityByID(plan.Owner);
		if (!owner.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "the 2D body owner {} is not an entity of scene '{}'", plan.Owner, scene.GetName());

		const glm::mat4 bakedScale = glm::scale(glm::mat4(1.0f), TransformSystem::GetWorldScale(owner));
		Body2DShapeDescription description;
		description.Colliders.reserve(plan.Colliders.size());
		for (size_t index = 0; index < plan.Colliders.size(); ++index)
		{
			const Physics2DColliderPlan& collider = plan.Colliders[index];
			const ConstEntity entity = scene.FindEntityByID(collider.Entity);
			if (!entity.IsValid())
				return MakeError(ErrorCode::InvalidArgument, "collider {} of the 2D body of {} is not an entity of scene '{}'", collider.Entity, plan.Owner,
					scene.GetName());
			const std::optional<glm::mat4> below = Utils::GetMatrixBelow2DOwner(owner, entity);
			if (!below.has_value())
			{
				return MakeError(ErrorCode::InvalidArgument, "collider {} of the 2D body of {} is not below its owner in scene '{}'", collider.Entity,
					plan.Owner, scene.GetName());
			}

			const glm::mat4 placement = bakedScale * *below * Utils::GetCollider2DLocalMatrix(entity, collider.Type);
			const Result<TransformDecomposition> decomposed = Utils::Decompose2DPlacement(placement);
			if (!decomposed)
			{
				return Utils::Make2DShapeError(collider.Entity,
					std::format("the {} of {} cannot be placed in its body: {}", GetCollider2DComponentName(collider.Type), Utils::Describe2DEntity(entity),
						decomposed.error().GetMessageText()));
			}

			// The placement in the plane: its X and Y, and its rotation's twist about Z (ToPhysics2DPose drops the swing).
			const Physics2DPose planar = ToPhysics2DPose(PhysicsPose{ .Position = decomposed->Translation, .Rotation = decomposed->Rotation });
			if (collider.Type == Physics2DColliderType::Tilemap)
			{
				// One box per merged rectangle of cells, centred on it in the entity's plane, all with the collider's index.
				ENGINE_TRY_ASSIGN(const auto tilemapBoxes, Utils::LoadTilemapColliderBoxes(entity, index, assets));
				const auto& [tilemap, boxes] = tilemapBoxes;
				const glm::vec2 cellSize = tilemap->CellSize;
				for (const TilemapBox& box : boxes)
				{
					const glm::vec2 low = glm::vec2(box.Min) * cellSize;
					const glm::vec2 high = (glm::vec2(box.Min) + glm::vec2(box.Size)) * cellSize;
					const glm::vec2 center = 0.5f * (low + high);
					Collider2DShapeDescription& shape = description.Colliders.emplace_back();
					shape.Geometry = BoxShape2D{ .HalfExtents = 0.5f * (high - low) };
					shape.Position = glm::vec2(placement * glm::vec4(center, 0.0f, 1.0f));
					shape.Angle = planar.Angle;
					shape.Scale = glm::vec2(decomposed->Scale.x, decomposed->Scale.y);
					shape.UserData = static_cast<uint32_t>(index);
				}
				continue;
			}

			Collider2DShapeDescription shape;
			shape.Position = planar.Position;
			shape.Angle = planar.Angle;
			shape.Scale = glm::vec2(decomposed->Scale.x, decomposed->Scale.y);
			shape.UserData = static_cast<uint32_t>(index);
			switch (collider.Type)
			{
				case Physics2DColliderType::Box:
					shape.Geometry = BoxShape2D{ .HalfExtents = entity.GetComponent<BoxCollider2DComponent>().HalfExtents };
					break;
				case Physics2DColliderType::Circle:
				{
					shape.Geometry = CircleShape2D{ .Radius = entity.GetComponent<CircleCollider2DComponent>().Radius };
					// A circle under a non-uniform scale uses the larger of X and Y (PHYSICS_NONUNIFORM_SCALE).
					shape.Scale = glm::vec2(std::max(std::abs(shape.Scale.x), std::abs(shape.Scale.y)));
					break;
				}
				case Physics2DColliderType::Polygon:
					shape.Geometry = PolygonShape2D{ .Points = entity.GetComponent<PolygonCollider2DComponent>().Points };
					break;
				case Physics2DColliderType::Tilemap:
					ENGINE_CORE_ASSERT(false, "DescribePhysics2DBodyShape: a TilemapCollider is laid above");
					break;
			}
			description.Colliders.push_back(std::move(shape));
		}
		return description;
	}

	Result<Ref<const PhysicsShape>> CreatePhysics2DBodyShape(const Physics2DBodyPlan& plan, const Body2DShapeDescription& description)
	{
		Result<Ref<const PhysicsShape>> shape = MakeBody2DShape(description).and_then([](const BodyShapeDescription& laid)
		{
			return PhysicsShape::Create(laid);
		});
		if (shape)
			return shape;

		// "<code>: collider <place>: <reason>" names the place in description.Colliders: renamed to the plan's collider.
		const Error& error = shape.error();
		const std::string_view code = GetPhysicsDiagnosticCode(error);
		const std::string_view message = error.GetMessageText();
		const std::string_view reason = code.empty() ? message : message.substr(code.size() + 2);
		const std::optional<uint32_t> place = Utils::ParseColliderIndex(reason);
		if (!place.has_value() || *place >= description.Colliders.size() || description.Colliders[*place].UserData >= plan.Colliders.size())
			return shape;
		const uint32_t index = description.Colliders[*place].UserData;
		const Physics2DColliderPlan& collider = plan.Colliders[index];
		const std::string_view detail = reason.substr(reason.find(':') + 1);
		std::string renamed = std::format("{}: collider {}:", code, index);
		if (collider.Type == Physics2DColliderType::Tilemap)
		{
			const auto box = static_cast<size_t>(std::count_if(description.Colliders.begin(), description.Colliders.begin() + static_cast<std::ptrdiff_t>(*place),
				[index](const Collider2DShapeDescription& candidate)
			{
				return candidate.UserData == index;
			}));
			renamed += std::format(" box {} of its tilemap:", box);
		}
		renamed += detail;
		ErrorLocation location;
		location.Entity = collider.Entity;
		return std::unexpected(Error(error.GetCode(), std::move(renamed)).WithLocation(std::move(location)).WithHint(error.GetHint()));
	}

	const Physics2DColliderPlan* FindRefusedCollider2D(const Physics2DBodyPlan& plan, const Error& error)
	{
		const std::string_view code = GetPhysicsDiagnosticCode(error);
		const std::string_view message = error.GetMessageText();
		const std::string_view reason = code.empty() ? message : message.substr(code.size() + 2);
		if (const std::optional<uint32_t> index = Utils::ParseColliderIndex(reason); index.has_value() && *index < plan.Colliders.size())
			return &plan.Colliders[*index];
		const UUID entity = error.GetLocation().Entity;
		if (!entity.IsValid())
			return nullptr;
		const auto collider = std::ranges::find_if(plan.Colliders, [entity](const Physics2DColliderPlan& candidate)
		{
			return candidate.Entity == entity;
		});
		return collider != plan.Colliders.end() ? &*collider : nullptr;
	}

	std::string_view GetCollider2DShapeField(Physics2DColliderType type)
	{
		return type == Physics2DColliderType::Tilemap ? "Tilemap" : "";
	}

	std::string_view GetCollider2DComponentName(Physics2DColliderType type)
	{
		switch (type)
		{
			case Physics2DColliderType::Box:     return "BoxCollider2D";
			case Physics2DColliderType::Circle:  return "CircleCollider2D";
			case Physics2DColliderType::Polygon: return "PolygonCollider2D";
			case Physics2DColliderType::Tilemap: return "TilemapCollider";
		}

		ENGINE_CORE_ASSERT(false, "Unknown Physics2DColliderType {}", std::to_underlying(type));
		return "BoxCollider2D";
	}

}
