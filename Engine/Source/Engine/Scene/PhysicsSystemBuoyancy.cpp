#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Scene/Components/BuoyancyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/WaterQueries.h"
#include "Engine/Scene/WaterValidation.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// PhysicsSystem's buoyancy (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §9.12; PhysicsSystem.h's item 4c):
// each effectively enabled Buoyancy whose owner holds a created Dynamic body takes, per collider, the plane of its water's
// surface under the collider's world bounds centre at the tick's world time, and PhysicsWorld::ApplyBuoyancy floats the body
// on them. A body whose planes are bit-identical to its last tick's may sleep and is skipped while asleep; one whose planes
// changed is woken. The planes are runtime state: nothing here is hashed, and a scene without a Buoyancy runs none of it.

namespace Engine {

	namespace Utils {

		// The owners holding a Buoyancy that are not pending destruction, in canonical order (the registry's view, sorted by
		// its entities' canonical positions when it holds two or more; M43's CollectCanonical replaces this at the merge).
		static std::vector<Entity> CollectBuoyancyOwners(Scene& scene)
		{
			std::vector<Entity> owners;
			for (const entt::entity handle : scene.GetRegistry().view<BuoyancyComponent>())
			{
				const Entity entity(handle, &scene);
				if (scene.FindEntityByID(entity.GetUUID()).IsValid())
					owners.push_back(entity);
			}
			if (owners.size() < 2)
				return owners;
			std::map<UUID, size_t> positions;
			for (const Entity& owner : owners)
				positions.emplace(owner.GetUUID(), 0);
			const std::span<const UUID> order = scene.GetCanonicalOrder();
			for (size_t position = 0; position < order.size(); ++position)
			{
				if (const auto found = positions.find(order[position]); found != positions.end())
					found->second = position;
			}
			std::ranges::sort(owners, {}, [&positions](const Entity& owner)
			{
				return positions.at(owner.GetUUID());
			});
			return owners;
		}

		// Whether two planes are bit-identical (the wake rule).
		static bool AreSamePlanes(std::span<const PhysicsFluidPlane> a, std::span<const PhysicsFluidPlane> b)
		{
			return std::ranges::equal(a, b, [](const PhysicsFluidPlane& x, const PhysicsFluidPlane& y)
			{
				return x.InWater == y.InWater && x.Point == y.Point && x.Normal == y.Normal && x.Velocity == y.Velocity;
			});
		}

	}

	void PhysicsSystem::State::ApplyBuoyancyItem(const SimStep& step)
	{
		Scene& scene = *Specification.RuntimeScene;
		if (scene.GetRegistry().view<BuoyancyComponent>().empty())
		{
			Buoyancy.clear();
			return;
		}
		const SceneWaters waters = CollectSceneWaters(scene);
		const auto deltaTime = static_cast<float>(step.FixedDelta);
		std::map<UUID, PhysicsBuoyancyEntry> applied;
		for (const Entity& owner : Utils::CollectBuoyancyOwners(scene))
		{
			const UUID id = owner.GetUUID();
			if (owner.HasComponent<HierarchyDisabledTag>())
				continue;
			const BuoyancyComponent& buoyancy = owner.GetComponent<BuoyancyComponent>();
			PhysicsBuoyancyEntry entry;
			if (const auto previous = Buoyancy.find(id); previous != Buoyancy.end())
				entry = std::move(previous->second);

			// The body: the owner's RigidBody's, created and Dynamic.
			const auto handle = BodiesByOwner.find({ id, PhysicsBodyOrigin::RigidBody });
			const auto body = handle != BodiesByOwner.end() ? Bodies.find(handle->second) : Bodies.end();
			const auto invalid = [this, &owner, id, &applied](std::string message)
			{
				RaiseDiagnostic(PhysicsDiagnostic{ .Code = std::string(PhysicsBuoyancyInvalidCode),
					.Severity = DiagnosticSeverity::Warning,
					.Entity = id,
					.Component = "Buoyancy",
					.Message = std::format("the Buoyancy of '{}' ({}) floats nothing: {}", owner.GetName(), id, message),
					.Hint = "give the entity a Dynamic RigidBody with a solid collider, and set Water to an entity with a Water or to null" });
				applied.emplace(id, PhysicsBuoyancyEntry{});
			};
			if (body == Bodies.end() || body->second.Plan.MotionType != PhysicsMotionType::Dynamic)
			{
				invalid("its RigidBody makes no Dynamic body");
				continue;
			}
			const PhysicsBodyEntry& bodyEntry = body->second;

			// The water the Buoyancy names, when it names one.
			std::optional<ResolvedWater> named;
			if (buoyancy.Water.IsValid())
			{
				const ConstEntity water = scene.FindEntityByID(buoyancy.Water);
				if (!water.IsValid() || !water.HasComponent<WaterComponent>())
				{
					invalid("its Water names no entity with a Water");
					continue;
				}
				named = ResolveWater(water);
			}

			// Each collider's plane, at its world bounds' centre, and the water it came from.
			std::vector<PhysicsFluidPlane> planes(bodyEntry.Colliders.size());
			std::vector<UUID> planeWaters(bodyEntry.Colliders.size());
			for (uint32_t collider = 0; collider < planes.size(); ++collider)
			{
				const std::optional<Aabb> bounds = World->GetSubShapeBounds(bodyEntry.Handle, collider);
				if (!bounds.has_value())
					continue;
				const glm::dvec3 center = (glm::dvec3(bounds->Min) + glm::dvec3(bounds->Max)) * 0.5;
				std::optional<SceneWaterSample> sample;
				if (named.has_value())
				{
					if (IsInWaterVolume(*named, center))
						sample = SampleWater(*named, center, step.Time);
				}
				else
					sample = SampleSceneWater(waters, center, step.Time);
				if (!sample.has_value())
					continue;
				planes[collider] = PhysicsFluidPlane{ .InWater = true,
					.Point = glm::vec3(static_cast<float>(center.x), static_cast<float>(sample->Height), static_cast<float>(center.z)),
					.Normal = glm::vec3(sample->Normal),
					.Velocity = glm::vec3(sample->Velocity) };
				planeWaters[collider] = sample->Water;
			}

			// The wake rule: unchanged planes let a sleeping body sleep on with its last report; changed ones wake it.
			const bool unchanged = Utils::AreSamePlanes(planes, entry.Planes);
			if (World->IsSleeping(bodyEntry.Handle))
			{
				if (unchanged)
				{
					applied.emplace(id, std::move(entry));
					continue;
				}
				World->WakeUp(bodyEntry.Handle);
			}

			// The density of the named water, else of the water of the first collider in water (a body out of water takes the
			// component's default and displaces nothing).
			UUID densityWater = named.has_value() ? named->Entity : UUID();
			if (!densityWater.IsValid())
			{
				if (const auto first = std::ranges::find_if(planeWaters, &UUID::IsValid); first != planeWaters.end())
					densityWater = *first;
			}
			float density = WaterComponent{}.Density;
			if (const ConstEntity water = scene.FindEntityByID(densityWater); water.IsValid() && water.HasComponent<WaterComponent>())
				density = water.GetComponent<WaterComponent>().Density;

			const PhysicsBuoyancySettings settings{ .Density = density,
				.BuoyancyScale = buoyancy.BuoyancyScale,
				.LinearDrag = buoyancy.LinearDrag,
				.AngularDrag = buoyancy.AngularDrag };
			const Result<PhysicsBuoyancyResult> result = World->ApplyBuoyancy(bodyEntry.Handle, planes, settings, deltaTime);
			if (!result.has_value())
			{
				invalid(std::format("its body cannot float ({})", result.error().GetMessageText()));
				continue;
			}

			// The report: the named water, else the water whose planes held the most of the submerged volume, the waters listed
			// in collider order so a tie keeps the one of the lowest collider index.
			PhysicsBuoyancyReport report{ .SubmergedVolume = result->SubmergedVolume, .TotalVolume = result->TotalVolume };
			if (named.has_value())
				report.Water = named->Entity;
			else if (result->SubmergedVolume > 0.0f)
			{
				std::vector<std::pair<UUID, float>> volumes;
				for (size_t index = 0; index < planeWaters.size() && index < result->PlaneSubmergedVolumes.size(); ++index)
				{
					if (!planeWaters[index].IsValid())
						continue;
					const auto found = std::ranges::find(volumes, planeWaters[index], &std::pair<UUID, float>::first);
					if (found == volumes.end())
						volumes.emplace_back(planeWaters[index], result->PlaneSubmergedVolumes[index]);
					else
						found->second += result->PlaneSubmergedVolumes[index];
				}
				float most = 0.0f;
				for (const auto& [water, volume] : volumes)
				{
					if (volume > most)
					{
						most = volume;
						report.Water = water;
					}
				}
			}
			entry.Planes = std::move(planes);
			entry.Report = report;
			applied.emplace(id, std::move(entry));
		}
		Buoyancy = std::move(applied);
	}

}
