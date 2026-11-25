#include "EnginePCH.h"
#include "Engine/Scene/PhysicsMassValidation.h"

#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/MassPropertiesComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <format>
#include <set>
#include <string>

namespace Engine {

	PhysicsMassOverride GetPhysicsMassOverride(const ConstEntity& owner)
	{
		const MassPropertiesComponent* mass = owner.TryGetComponent<MassPropertiesComponent>();
		if (mass == nullptr)
			return {};
		return PhysicsMassOverride{ .CenterOfMassOffset = mass->CenterOfMassOffset, .InertiaScale = mass->InertiaScale };
	}

	std::vector<PhysicsDiagnostic> ValidateSceneMassProperties(const Scene& scene, const PhysicsLayerTable& layers)
	{
		// The owners whose own RigidBody makes a creatable Dynamic body that is not a sensor.
		std::set<UUID> used;
		for (const PhysicsBodyPlan& plan : ComposePhysicsBodies(scene, layers).Bodies)
		{
			if (plan.IsCreatable && !plan.IsSensor && plan.Origin == PhysicsBodyOrigin::RigidBody && plan.MotionType == PhysicsMotionType::Dynamic)
				used.insert(plan.Owner);
		}

		// Effectively enabled entities only, as the composition reads an edit scene (DisabledTag on the entity or an
		// ancestor): a disabled one's override applies once it is enabled.
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
			if (!entity.HasComponent<MassPropertiesComponent>() || used.contains(id))
				continue;
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsMassUnusedCode);
			diagnostic.Severity = DiagnosticSeverity::Warning;
			diagnostic.Entity = id;
			diagnostic.Component = "MassProperties";
			diagnostic.Message = std::format("the MassProperties of '{}' ({}) changes no body: the entity's RigidBody makes no Dynamic body (it is "
											 "Static or Kinematic, its colliders are triggers, or the scene creates no body for it)",
				entity.GetName(), id);
			diagnostic.Hint = "set the RigidBody's Type to Dynamic and give the entity a solid collider, or remove the MassProperties";
			diagnostics.push_back(std::move(diagnostic));
		}
		std::sort(diagnostics.begin(), diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return a.Entity < b.Entity;
		});
		return diagnostics;
	}

}
