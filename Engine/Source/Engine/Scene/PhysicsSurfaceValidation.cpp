#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSurfaceValidation.h"

#include "Engine/Scene/Components/ColliderSurfaceComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <format>
#include <set>
#include <string>

namespace Engine {

	std::vector<PhysicsDiagnostic> ValidateSceneSurfaces(const Scene& scene, const PhysicsLayerTable& layers)
	{
		// The entities whose solid colliders a creatable body holds (a character's capsule is no collider of a plan).
		std::set<UUID> used;
		for (const PhysicsBodyPlan& plan : ComposePhysicsBodies(scene, layers).Bodies)
		{
			if (!plan.IsCreatable || plan.IsSensor)
				continue;
			for (const PhysicsColliderPlan& collider : plan.Colliders)
			{
				if (!collider.IsTrigger)
					used.insert(collider.Entity);
			}
		}

		// Effectively enabled entities only, as the composition reads an edit scene (DisabledTag on the entity or an
		// ancestor): a disabled one's surface applies once it is enabled.
		std::vector<PhysicsDiagnostic> diagnostics;
		std::set<UUID> disabled;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			const ConstEntity parent = entity.GetParent();
			if (entity.HasComponent<DisabledTag>() || (parent.IsValid() && disabled.contains(parent.GetUUID())))
			{
				disabled.insert(id);
				continue;
			}
			if (!entity.HasComponent<ColliderSurfaceComponent>() || used.contains(id))
				continue;
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsSurfaceUnusedCode);
			diagnostic.Severity = DiagnosticSeverity::Warning;
			diagnostic.Entity = id;
			diagnostic.Component = "ColliderSurface";
			diagnostic.Message = std::format("the ColliderSurface of '{}' ({}) applies to no collider: no body the scene creates holds a solid 3D "
											 "collider of the entity (a character's capsule, triggers and 2D colliders take no surface, and a body a "
											 "physics error refuses is not created)",
				entity.GetName(), id);
			diagnostic.Hint = "give the entity a BoxCollider, SphereCollider, CapsuleCollider, MeshCollider or TerrainCollider that is not a trigger, "
							  "or remove the ColliderSurface";
			diagnostics.push_back(std::move(diagnostic));
		}
		std::sort(diagnostics.begin(), diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return a.Entity < b.Entity;
		});
		return diagnostics;
	}

}
