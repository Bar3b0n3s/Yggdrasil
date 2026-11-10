#include "EnginePCH.h"
#include "Engine/Scene/Physics2DValidation.h"

#include "Engine/Core/Log.h"
#include "Engine/Physics/Physics2DWorld.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/Physics2DComposition.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// The 2D checks of one scene (Physics2DValidation.h): the 2D composition's diagnostics (PHYSICS2D_MIXED_WORLDS,
// PHYSICS2D_OFF_PLANE and the shared PHYSICS_* codes), then PHYSICS_INVALID_SHAPE for every creatable 2D body whose shape
// cannot be built (its TilemapColliders' tilemaps loaded through the asset manager), and the Dynamic body check of every
// Dynamic RigidBody2D's built shape (M21's milestone review), as the play session's Physics2DSystem would refuse it.

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

		// The check of a Dynamic RigidBody2D against its built shape (PhysicsShape::CheckDynamicBody, which the play session's
		// Physics2DSystem makes before it creates the body, with Physics2DTranslationDofs under FixedRotation): a Mass the world
		// refuses, or an inertia too large for Jolt, reported as the system reports it: the carried code (PHYSICS_INVALID_SHAPE
		// without one), an Error on the owner's RigidBody2D, Field Mass.
		static std::optional<PhysicsDiagnostic> CheckDynamicBody2D(const Scene& scene, const Physics2DBodyPlan& plan, const PhysicsShape& shape)
		{
			if (plan.Origin != PhysicsBodyOrigin::RigidBody || plan.MotionType != PhysicsMotionType::Dynamic)
				return std::nullopt;
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			const RigidBody2DComponent* body = owner.IsValid() ? owner.TryGetComponent<RigidBody2DComponent>() : nullptr;
			if (body == nullptr)
				return std::nullopt;
			const Status checked = shape.CheckDynamicBody(body->Mass, body->FixedRotation ? Physics2DTranslationDofs : Physics2DDofs);
			if (checked.has_value())
				return std::nullopt;
			const std::string_view carried = GetPhysicsDiagnosticCode(checked.error());
			const std::string_view text = checked.error().GetMessageText();
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(carried.empty() ? PhysicsInvalidShapeCode : carried);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Entity = plan.Owner;
			diagnostic.Component = "RigidBody2D";
			diagnostic.Field = "Mass";
			diagnostic.Message = std::format("the Dynamic 2D body of '{}' cannot be created: {}", scene.GetEntityPath(owner),
				carried.empty() || text.size() <= carried.size() + 2 ? text : text.substr(carried.size() + 2));
			diagnostic.Hint = std::format("lower Mass (at most {} kg), or bring the colliders closer together or make them smaller; the play session does not "
										  "create the body otherwise",
				MaxPhysicsMass);
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
				const Result<Ref<const PhysicsShape>> shape = CreatePhysics2DBodyShape(plan, *description);
				if (!shape)
					diagnostics.push_back(Utils::MakeInvalid2DShapeDiagnostic(scene, plan, shape.error()));
				else if (std::optional<PhysicsDiagnostic> dynamic = Utils::CheckDynamicBody2D(scene, plan, **shape))
					diagnostics.push_back(std::move(*dynamic));
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
