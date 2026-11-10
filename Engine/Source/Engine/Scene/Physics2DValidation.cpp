#include "EnginePCH.h"
#include "Engine/Scene/Physics2DValidation.h"

#include "Engine/Core/Log.h"
#include "Engine/Physics/Physics2DWorld.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/Physics2DComposition.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <format>
#include <string>
#include <tuple>

// The 2D checks of one scene (Physics2DValidation.h): the 2D composition's diagnostics (PHYSICS2D_MIXED_WORLDS,
// PHYSICS2D_OFF_PLANE and the shared PHYSICS_* codes), then PHYSICS_INVALID_SHAPE for every creatable 2D body whose shape
// cannot be built (its TilemapColliders' tilemaps loaded through the asset manager), as the play session's Physics2DSystem
// would refuse it.

namespace Engine {

	namespace Utils {

		// The PHYSICS_INVALID_SHAPE diagnostic of a 2D body whose shape `error` refused: on the collider the error names (by
		// the collider index of its message, else its location), with that collider's component and, for a TilemapCollider,
		// the Tilemap field; else on the owner.
		static PhysicsDiagnostic MakeInvalid2DShapeDiagnostic(const Scene& scene, const Physics2DBodyPlan& plan, const Error& error)
		{
			const std::string_view code = GetPhysicsDiagnosticCode(error);
			std::string_view reason = error.GetMessageText();
			if (!code.empty() && reason.size() > code.size() + 1)
				reason = reason.substr(code.size() + 2);
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsInvalidShapeCode);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Entity = plan.Owner;
			if (const Physics2DColliderPlan* collider = FindRefusedCollider2D(plan, error))
			{
				diagnostic.Entity = collider->Entity;
				diagnostic.Component = std::string(GetCollider2DComponentName(collider->Type));
				diagnostic.Field = std::string(GetCollider2DShapeField(collider->Type));
			}
			else if (error.GetLocation().Entity.IsValid())
			{
				diagnostic.Entity = error.GetLocation().Entity;
			}
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			diagnostic.Message = std::format("the 2D body of '{}' cannot be built: {}", owner.IsValid() ? scene.GetEntityPath(owner) : plan.Owner.ToString(), reason);
			diagnostic.Hint = error.GetHint().empty()
				? std::string("correct the 2D collider's size, points or scale; the play session does not create the body while its shape is refused")
				: error.GetHint();
			return diagnostic;
		}

	}

	std::vector<PhysicsDiagnostic> ValidateScenePhysics2D(const Scene& scene, const PhysicsLayerTable& layers, AssetManager* assets)
	{
		Physics2DComposition composition = ComposePhysics2DBodies(scene, layers);
		std::vector<PhysicsDiagnostic> diagnostics = std::move(composition.Diagnostics);
		if (PhysicsEngine::IsInitialized())
		{
			for (const Physics2DBodyPlan& plan : composition.Bodies)
			{
				if (!plan.IsCreatable)
					continue;
				const Result<Body2DShapeDescription> description = DescribePhysics2DBodyShape(scene, plan, assets);
				if (!description)
				{
					diagnostics.push_back(Utils::MakeInvalid2DShapeDiagnostic(scene, plan, description.error()));
					continue;
				}
				// Tilemaps without a solid cell leave the body no sub-shape: no body, as the session, and no diagnostic.
				if (description->Colliders.empty())
					continue;
				if (const Result<Ref<const PhysicsShape>> shape = CreatePhysics2DBodyShape(plan, *description); !shape)
					diagnostics.push_back(Utils::MakeInvalid2DShapeDiagnostic(scene, plan, shape.error()));
			}
		}
		else
		{
			ENGINE_CORE_WARN("2D physics validation of scene '{}' skips the shape checks ({}): the physics engine is not initialized", scene.GetName(),
				PhysicsInvalidShapeCode);
		}

		std::sort(diagnostics.begin(), diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return std::tie(a.Entity, a.Code, a.Subject, a.Component, a.Field, a.Message) < std::tie(b.Entity, b.Code, b.Subject, b.Component, b.Field, b.Message);
		});
		return diagnostics;
	}

}
