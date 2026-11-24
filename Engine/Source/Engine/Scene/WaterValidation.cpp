#include "EnginePCH.h"
#include "Engine/Scene/WaterValidation.h"

#include "Engine/Physics/WaterWaves.h"
#include "Engine/Scene/Components/BuoyancyComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		// "'Name' (<uuid>)", how the findings name an entity.
		static std::string DescribeWaterEntity(ConstEntity entity)
		{
			return std::format("'{}' ({})", entity.GetName(), entity.GetUUID());
		}

		// Whether the owner's body is made, Dynamic, with a solid collider: a body plan of `composition` owned by `owner`, from
		// its RigidBody, creatable, with a collider that is not a trigger.
		static bool HasSolidDynamicBody(const PhysicsComposition& composition, UUID owner)
		{
			const auto plan = std::ranges::find_if(composition.Bodies, [owner](const PhysicsBodyPlan& body)
			{
				return body.Owner == owner && body.Origin == PhysicsBodyOrigin::RigidBody;
			});
			if (plan == composition.Bodies.end() || !plan->IsCreatable || plan->MotionType != PhysicsMotionType::Dynamic)
				return false;
			return std::ranges::any_of(plan->Colliders, [](const PhysicsColliderPlan& collider)
			{
				return !collider.IsTrigger;
			});
		}

	}

	bool IsLevelWaterFrame(const glm::mat4& world)
	{
		const glm::dvec3 axisX(world[0]);
		const glm::dvec3 axisY(world[1]);
		const glm::dvec3 axisZ(world[2]);
		const double lengthX = std::sqrt(glm::dot(axisX, axisX));
		const double lengthY = std::sqrt(glm::dot(axisY, axisY));
		const double lengthZ = std::sqrt(glm::dot(axisZ, axisZ));
		if (!(lengthX > 0.0) || !(lengthY > 0.0) || !(lengthZ > 0.0) || !std::isfinite(lengthX * lengthY * lengthZ))
			return false;
		const double tolerance = WaterFrameTolerance;
		const bool levelX = std::abs(axisX.y) <= tolerance * lengthX;
		const bool levelZ = std::abs(axisZ.y) <= tolerance * lengthZ;
		const bool verticalY = std::abs(axisY.x) <= tolerance * lengthY && std::abs(axisY.z) <= tolerance * lengthY;
		const bool perpendicular = std::abs(glm::dot(axisX, axisZ)) <= tolerance * lengthX * lengthZ;
		// Right-handed about +Y: local +Z a quarter turn from local +X, as a yaw turns them.
		const bool rightHanded = axisX.x * axisZ.z - axisX.z * axisZ.x > 0.0;
		return levelX && levelZ && verticalY && perpendicular && rightHanded;
	}

	std::vector<WaterSceneIssue> FindWaterIssues(const Scene& scene, const PhysicsLayerTable* layers)
	{
		std::vector<WaterSceneIssue> issues;
		const bool hasWater = !scene.GetRegistry().view<WaterComponent>().empty();
		const bool hasBuoyancy = !scene.GetRegistry().view<BuoyancyComponent>().empty();
		if (!hasWater && !hasBuoyancy)
			return issues;

		std::optional<PhysicsComposition> composition;
		if (hasBuoyancy && layers != nullptr)
			composition = ComposePhysicsBodies(scene, *layers);
		std::optional<ConstEntity> firstOcean;
		scene.ForEachCanonical([&scene, &issues, &composition, &firstOcean](ConstEntity entity)
		{
			if (entity.HasComponent<HierarchyDisabledTag>())
				return;
			const UUID id = entity.GetUUID();
			if (const WaterComponent* water = entity.TryGetComponent<WaterComponent>())
			{
				if (!IsLevelWaterFrame(TransformSystem::ComputeWorldMatrix(entity)))
				{
					issues.push_back(WaterSceneIssue{
						.Code = std::string(WaterInvalidCode),
						.Entity = id,
						.Component = "Water",
						.Message = std::format("Water {} is pitched, rolled, sheared or mirrored, so it draws and samples level at its yaw",
							Utils::DescribeWaterEntity(entity)),
						.Hint = "turn the water only about +Y (its own and its ancestors' rotations), and give it a positive X and Z scale",
					});
				}
				if (water->Shape == WaterShape::Ocean)
				{
					if (firstOcean.has_value())
					{
						issues.push_back(WaterSceneIssue{
							.Code = std::string(WaterInvalidCode),
							.Entity = id,
							.Component = "Water",
							.Field = "Shape",
							.Message = std::format("Water {} is a second Ocean, which is neither drawn nor sampled: the scene's Ocean is {}",
								Utils::DescribeWaterEntity(entity), Utils::DescribeWaterEntity(*firstOcean)),
							.Hint = "keep one Ocean a scene; make the other water a Rectangle, or remove it",
						});
					}
					else
						firstOcean = entity;
				}
				const double sum = ComputeWaterSteepnessSum(water->Waves, static_cast<double>(water->WaveScale));
				if (sum > MaxWaterSteepnessSum)
				{
					issues.push_back(WaterSceneIssue{
						.Code = std::string(WaterWavesTooSteepCode),
						.Entity = id,
						.Component = "Water",
						.Field = "Waves",
						.Message = std::format("Water {}'s waves would fold the surface at WaveScale {} (steepness sum {:.3f}, at most {}), so every "
											   "Steepness is scaled by {:.3f}",
							Utils::DescribeWaterEntity(entity), water->WaveScale, sum, MaxWaterSteepnessSum, MaxWaterSteepnessSum / sum),
						.Hint = "lower the Steepness or Amplitude of the short waves, or the WaveScale",
					});
				}
			}
			if (const BuoyancyComponent* buoyancy = entity.TryGetComponent<BuoyancyComponent>())
			{
				if (buoyancy->Water.IsValid())
				{
					const ConstEntity target = scene.FindEntityByID(buoyancy->Water);
					if (!target.IsValid() || !target.HasComponent<WaterComponent>())
					{
						issues.push_back(WaterSceneIssue{
							.Code = std::string(PhysicsBuoyancyInvalidCode),
							.Entity = id,
							.Component = "Buoyancy",
							.Field = "Water",
							.Message = std::format("Buoyancy on {} names {} as its Water, which is no entity with a Water, so it floats nothing",
								Utils::DescribeWaterEntity(entity), buoyancy->Water),
							.Hint = "set Water to an entity with a Water component, or to null for the water that holds each collider",
						});
						return;
					}
				}
				if (!composition.has_value())
					return;
				const RigidBodyComponent* body = entity.TryGetComponent<RigidBodyComponent>();
				if (body != nullptr && body->Type != BodyType::Dynamic)
				{
					issues.push_back(WaterSceneIssue{
						.Code = std::string(PhysicsBuoyancyInvalidCode),
						.Entity = id,
						.Component = "RigidBody",
						.Field = "Type",
						.Message = std::format("Buoyancy on {} floats nothing: its RigidBody is not Dynamic", Utils::DescribeWaterEntity(entity)),
						.Hint = "make the RigidBody Dynamic, or remove the Buoyancy",
					});
				}
				else if (body != nullptr && !Utils::HasSolidDynamicBody(*composition, id))
				{
					issues.push_back(WaterSceneIssue{
						.Code = std::string(PhysicsBuoyancyInvalidCode),
						.Entity = id,
						.Component = "Buoyancy",
						.Message = std::format("Buoyancy on {} floats nothing: its RigidBody makes no body with a solid collider",
							Utils::DescribeWaterEntity(entity)),
						.Hint = "give the entity (or a descendant it gathers) a box, sphere, capsule or convex mesh collider that is not a trigger",
					});
				}
			}
		});
		return issues;
	}

}
