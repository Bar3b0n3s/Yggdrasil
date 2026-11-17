#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/ColliderSurfaceComponent.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <vector>

// The collider surfaces of the play session (M36, Docs/Decisions/0053-m36-contact-surfaces.md decision 3; Architecture §9.2
// "Collider surfaces"): a ColliderSurface gives the solid colliders on its entity their own friction, restitution and surface
// velocity in whatever body gathers them. Its signals mark the entity in DirtySurfaces and never set ComponentsChanged, so a
// write (a belt's velocity set every tick) never recomposes the scene, rebuilds a shape or changes a body handle; PreStep
// sends the surfaces of the bodies holding a marked entity's colliders after its rebuilds, CreateBody and ReplaceShape send
// them for every new or rebuilt shape. The world wakes what lies on a collider only when its values really changed, so an
// unchanged write wakes nothing. Kept in its own file so that the composition, sync and step paths of PhysicsSystem.cpp only
// call it.

namespace Engine {

	void PhysicsSystem::State::OnSurfaceChanged(entt::registry& registry, entt::entity entity)
	{
		// An entity being destroyed may have lost its IDComponent already; its body's rebuild sends the surfaces then.
		if (const IDComponent* id = registry.try_get<IDComponent>(entity))
			DirtySurfaces.insert(id->ID);
	}

	std::vector<PhysicsColliderSurface> PhysicsSystem::State::CollectSurfaces(const PhysicsBodyEntry& entry) const
	{
		std::vector<PhysicsColliderSurface> surfaces;
		if (entry.Plan.IsSensor)
			return surfaces;
		const Scene& scene = *Specification.RuntimeScene;
		const ConstEntity owner = scene.FindEntityByID(entry.Owner);
		if (!owner.IsValid())
			return surfaces;
		std::optional<glm::quat> toBody;
		for (size_t index = 0; index < entry.Plan.Colliders.size(); ++index)
		{
			const PhysicsColliderPlan& collider = entry.Plan.Colliders[index];
			if (collider.IsTrigger)
				continue;
			const ConstEntity entity = scene.FindEntityByID(collider.Entity);
			const ColliderSurfaceComponent* surface = entity.IsValid() ? entity.TryGetComponent<ColliderSurfaceComponent>() : nullptr;
			if (surface == nullptr)
				continue;
			// The entity's axes into the body's: the owner's own colliders share its axes; a gathered collider's turn by its
			// rotation relative to the owner (a change of which rebuilds the shape, which sends the surfaces again).
			glm::vec3 velocity = surface->SurfaceVelocity;
			if (collider.Entity != entry.Owner)
			{
				if (!toBody.has_value())
					toBody = glm::conjugate(TransformSystem::GetWorldRotation(owner));
				velocity = (*toBody * TransformSystem::GetWorldRotation(entity)) * velocity;
			}
			surfaces.push_back(PhysicsColliderSurface{ .Collider = static_cast<uint32_t>(index),
				.Friction = surface->Friction,
				.Restitution = surface->Restitution,
				.SurfaceVelocity = velocity });
		}
		return surfaces;
	}

	void PhysicsSystem::State::SendSurfaces(PhysicsBodyEntry& entry)
	{
		const std::vector<PhysicsColliderSurface> surfaces = CollectSurfaces(entry);
		if (surfaces.empty() && !entry.HasSurfaces)
			return;
		if (const Status sent = World->SetColliderSurfaces(entry.Handle, surfaces); !sent)
		{
			// The component's field bounds keep every value in the world's range, so this is not content the world can refuse;
			// the body keeps the surfaces it had.
			ENGINE_CORE_ERROR("PhysicsSystem: the collider surfaces of the body of entity {} were refused: {}", entry.Owner, sent.error().ToString());
			return;
		}
		entry.HasSurfaces = !surfaces.empty();
	}

	void PhysicsSystem::State::RefreshSurfaces()
	{
		if (DirtySurfaces.empty())
			return;
		std::set<uint32_t> bodies;
		for (const UUID entity : DirtySurfaces)
		{
			if (const auto gathered = GatheredColliders.find(entity); gathered != GatheredColliders.end())
				bodies.insert(gathered->second);
			for (const PhysicsBodyOrigin origin : std::array{ PhysicsBodyOrigin::RigidBody, PhysicsBodyOrigin::ImplicitStatic })
			{
				if (const PhysicsBodyEntry* entry = FindOwnedBody(entity, origin))
					bodies.insert(entry->Handle.GetValue());
			}
		}
		DirtySurfaces.clear();
		for (const uint32_t handle : bodies)
		{
			if (PhysicsBodyEntry* entry = FindBody(handle))
				SendSurfaces(*entry);
		}
	}

	const PhysicsBodyEntry* PhysicsSystem::State::FindColliderBody(UUID entity) const
	{
		const auto holds = [entity](const PhysicsBodyEntry& entry)
		{
			return !entry.Plan.IsSensor && std::any_of(entry.Plan.Colliders.begin(), entry.Plan.Colliders.end(), [entity](const PhysicsColliderPlan& collider)
			{
				return collider.Entity == entity && !collider.IsTrigger;
			});
		};
		for (const PhysicsBodyOrigin origin : std::array{ PhysicsBodyOrigin::RigidBody, PhysicsBodyOrigin::ImplicitStatic })
		{
			if (const PhysicsBodyEntry* entry = FindOwnedBody(entity, origin); entry != nullptr && holds(*entry))
				return entry;
		}
		if (const auto gathered = GatheredColliders.find(entity); gathered != GatheredColliders.end())
		{
			if (const PhysicsBodyEntry* entry = FindBody(gathered->second); entry != nullptr && holds(*entry))
				return entry;
		}
		return nullptr;
	}

}
