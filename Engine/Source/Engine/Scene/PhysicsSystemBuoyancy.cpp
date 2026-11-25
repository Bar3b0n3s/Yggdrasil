#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Physics/CharacterController.h"
#include "Engine/Scene/Components/BuoyancyComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/WaterQueries.h"
#include "Engine/Scene/WaterValidation.h"

#include <algorithm>
#include <bit>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// PhysicsSystem's buoyancy (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §9.12; PhysicsSystem.h's item 4c):
// each effectively enabled Buoyancy whose owner holds a created Dynamic body takes, per collider, the plane of its water's
// surface under the collider's world bounds centre at the tick's world time, and PhysicsWorld::ApplyBuoyancy floats the body
// on them. A body whose planes and settings are bit-identical to its last tick's may sleep and is skipped while asleep; one
// whose planes or settings changed is woken, and so is one a grounded character stood on in its last update (phase 8's
// landing): item 5's ground push would wake it after this item, for a step without buoyancy. The planes are runtime state:
// nothing here is hashed, and a scene without a Buoyancy runs none of it.

namespace Engine {

	namespace Utils {

		// The owners holding a Buoyancy that are not pending destruction, in canonical order: M43's CollectCanonical, the
		// view sorted by canonical key (phase 8's landing), never a walk of every entity; the visits are added to the scene's
		// EntitiesVisited (ADR 0060's ledger, "Rules for every pass a later lander adds").
		static std::vector<Entity> CollectBuoyancyOwners(Scene& scene)
		{
			std::vector<entt::entity> handles;
			scene.CollectCanonical<BuoyancyComponent>(handles);
			scene.AddEntitiesVisited(handles.size());
			std::vector<Entity> owners;
			owners.reserve(handles.size());
			for (const entt::entity handle : handles)
				owners.emplace_back(handle, &scene);
			return owners;
		}

		// The bodies the characters stood on in their last updates (CharacterController::GetGroundState of a grounded one),
		// sorted by handle: a membership set, the same whatever the characters' order.
		static std::vector<uint32_t> CollectRiddenBodies(const std::map<UUID, PhysicsCharacterEntry>& characters)
		{
			std::vector<uint32_t> ridden;
			for (const auto& [id, character] : characters)
			{
				static_cast<void>(id);
				if (character.Controller == nullptr)
					continue;
				const CharacterGroundState ground = character.Controller->GetGroundState();
				if (ground.IsGrounded && ground.GroundBody.IsValid())
					ridden.push_back(ground.GroundBody.GetValue());
			}
			std::ranges::sort(ridden);
			return ridden;
		}

		static bool SameBits(float a, float b)
		{
			return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
		}

		static bool SameBits(const glm::vec3& a, const glm::vec3& b)
		{
			return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z);
		}

		// Whether a body is applied with the planes and settings of its last application, bit for bit (the wake rule): a
		// sleeping body that is sleeps on; any change wakes it.
		static bool IsSameApplication(std::span<const PhysicsFluidPlane> planes, const PhysicsBuoyancySettings& settings, const PhysicsBuoyancyEntry& entry)
		{
			const bool samePlanes = std::ranges::equal(planes, entry.Planes, [](const PhysicsFluidPlane& x, const PhysicsFluidPlane& y)
			{
				return x.InWater == y.InWater && SameBits(x.Point, y.Point) && SameBits(x.Normal, y.Normal) && SameBits(x.Velocity, y.Velocity);
			});
			return samePlanes && SameBits(settings.Density, entry.Settings.Density) && SameBits(settings.BuoyancyScale, entry.Settings.BuoyancyScale)
				&& SameBits(settings.LinearDrag, entry.Settings.LinearDrag) && SameBits(settings.AngularDrag, entry.Settings.AngularDrag);
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
		const std::vector<uint32_t> ridden = Utils::CollectRiddenBodies(Characters);
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

			// A Buoyancy that cannot float is reported once (RaiseDiagnostic), on the component and field project.validate names
			// for the same cause (Scene/WaterValidation.h), and keeps a zero report.
			const auto invalid = [this, &owner, id, &applied](std::string_view component, std::string_view field, std::string_view cause, std::string_view hint)
			{
				RaiseDiagnostic(PhysicsDiagnostic{ .Code = std::string(PhysicsBuoyancyInvalidCode),
					.Severity = DiagnosticSeverity::Warning,
					.Entity = id,
					.Component = std::string(component),
					.Field = std::string(field),
					.Message = std::format("the Buoyancy of '{}' ({}) floats nothing: {}", owner.GetName(), id, cause),
					.Hint = std::string(hint) });
				applied.emplace(id, PhysicsBuoyancyEntry{});
			};

			// The water the Buoyancy names, when it names one, checked first, as project.validate checks it. A named water that
			// is effectively disabled exists for no query: it holds no point, so every plane is out of water and the body falls.
			std::optional<ResolvedWater> named;
			bool namedEnabled = false;
			if (buoyancy.Water.IsValid())
			{
				const ConstEntity water = scene.FindEntityByID(buoyancy.Water);
				if (!water.IsValid() || !water.HasComponent<WaterComponent>())
				{
					invalid("Buoyancy", "Water", "its Water names no entity with a Water",
						"set Water to an entity with a Water component, or to null for the water that holds each collider");
					continue;
				}
				named = ResolveWater(water);
				namedEnabled = !water.HasComponent<HierarchyDisabledTag>();
			}

			// The body: the owner's RigidBody's, created, Dynamic and free to translate.
			const RigidBodyComponent* rigidBody = owner.TryGetComponent<RigidBodyComponent>();
			if (rigidBody != nullptr && rigidBody->Type != BodyType::Dynamic)
			{
				invalid("RigidBody", "Type", "its RigidBody is not Dynamic", "make the RigidBody Dynamic, or remove the Buoyancy");
				continue;
			}
			const auto handle = BodiesByOwner.find({ id, PhysicsBodyOrigin::RigidBody });
			const auto body = handle != BodiesByOwner.end() ? Bodies.find(handle->second) : Bodies.end();
			if (rigidBody == nullptr || body == Bodies.end() || body->second.Plan.MotionType != PhysicsMotionType::Dynamic)
			{
				invalid("Buoyancy", "", "its RigidBody makes no body with a solid collider",
					"give the entity (or a descendant it gathers) a box, sphere, capsule or convex mesh collider that is not a trigger");
				continue;
			}
			if (glm::all(rigidBody->LockTranslation))
			{
				invalid("RigidBody", "LockTranslation", "its RigidBody locks every translation axis, so it cannot rise or sink",
					"unlock at least one axis of LockTranslation (Y to float), or remove the Buoyancy");
				continue;
			}
			const PhysicsBodyEntry& bodyEntry = body->second;

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
					if (namedEnabled && IsInWaterVolume(*named, center))
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

			// The density of the named water, else of the water of the first collider in water (a body out of water takes the
			// default and displaces nothing).
			UUID densityWater = named.has_value() ? named->Entity : UUID();
			if (!densityWater.IsValid())
			{
				if (const auto first = std::ranges::find_if(planeWaters, &UUID::IsValid); first != planeWaters.end())
					densityWater = *first;
			}
			float density = PhysicsBuoyancySettings{}.Density;
			if (const ConstEntity water = scene.FindEntityByID(densityWater); water.IsValid() && water.HasComponent<WaterComponent>())
				density = water.GetComponent<WaterComponent>().Density;
			const PhysicsBuoyancySettings settings{ .Density = density,
				.BuoyancyScale = buoyancy.BuoyancyScale,
				.LinearDrag = buoyancy.LinearDrag,
				.AngularDrag = buoyancy.AngularDrag };

			// The wake rule: a sleeping body applied as last time sleeps on with its last report; a change of its planes (waves,
			// a tide) or of its settings (a BuoyancyScale or drag write, its water's Density) wakes it, and so does a grounded
			// character standing on it (phase 8's landing): its ground push in item 5 would wake it after this item, for a step
			// without buoyancy, so it is woken and floated here, as in an awake tick, and stays awake while ridden.
			if (World->IsSleeping(bodyEntry.Handle))
			{
				if (Utils::IsSameApplication(planes, settings, entry) && !std::ranges::binary_search(ridden, bodyEntry.Handle.GetValue()))
				{
					applied.emplace(id, std::move(entry));
					continue;
				}
				World->WakeUp(bodyEntry.Handle);
			}

			const Result<PhysicsBuoyancyResult> result = World->ApplyBuoyancy(bodyEntry.Handle, planes, settings, deltaTime);
			if (!result.has_value())
			{
				invalid("Buoyancy", "", std::format("its body cannot float ({})", result.error().GetMessageText()),
					"give the entity a Dynamic RigidBody with a solid collider that can translate");
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
			entry.Settings = settings;
			entry.Report = report;
			applied.emplace(id, std::move(entry));
		}
		Buoyancy = std::move(applied);
	}

}
