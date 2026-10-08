#include "EnginePCH.h"
#include "Engine/Automation/Methods/PhysicsMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Assert.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <format>
#include <utility>

// physics.bodyInfo (Docs/Decisions/0014-m11-decisions.md decision 18): a read of the play session's PhysicsSystem, mapped to
// the reflected result. Shared by the Editor and the Runtime.

namespace Engine {

	namespace Utils {

		// {id, name, path} of the entity `id` of `scene`, or just its id when the scene no longer holds it (a partner whose
		// destruction is pending: its pairs close at the next flush).
		static EntitySummary SummarizeEntityId(const AutomationMethodContext& context, const Scene& scene, UUID id)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (entity.IsValid())
				return context.MakeEntitySummary(entity);
			EntitySummary summary;
			summary.Id = FormatOptionalUUID(id);
			return summary;
		}

		static BodyType ToBodyType(PhysicsMotionType type)
		{
			switch (type)
			{
				case PhysicsMotionType::Static:    return BodyType::Static;
				case PhysicsMotionType::Kinematic: return BodyType::Kinematic;
				case PhysicsMotionType::Dynamic:   return BodyType::Dynamic;
			}

			ENGINE_CORE_ASSERT(false, "Unknown PhysicsMotionType {}", std::to_underlying(type));
			return BodyType::Static;
		}

	}

	namespace Automation {

		Result<PhysicsBodyInfoResult> PhysicsBodyInfo(AutomationMethodContext& context, const PhysicsBodyInfoParams& params)
		{
			PlaySession* session = context.GetPlaySession();
			if (session == nullptr)
			{
				return std::unexpected(Error(ErrorCode::InvalidState, "not playing: physics bodies exist only in a play session")
						.WithHint("start one with play.start {mode: \"simulate\"} (physics without scripts and audio) or play.start, then step it"));
			}
			Scene& scene = session->GetScene();
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(scene, params.Entity, "/entity"));
			const std::optional<PhysicsBodyReport> report = session->GetPhysics().GetBodyInfo(entity.GetUUID());
			if (!report.has_value())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/entity",
					std::format("entity '{}' ({}) has no physics body", entity.GetName(), entity.GetUUID().ToString()),
					"bodies come from a RigidBody, a CharacterController or colliders on an active entity; project.validate lists the bodies "
					"a diagnostic refuses"));
			}

			PhysicsBodyInfoResult result;
			result.Entity = context.MakeEntitySummary(entity);
			result.Body = Utils::SummarizeEntityId(context, scene, report->Owner);
			result.Origin = report->Origin;
			result.Type = Utils::ToBodyType(report->MotionType);
			result.IsSensor = report->IsSensor;
			result.Layer = report->LayerName;
			result.Colliders.reserve(report->Colliders.size());
			for (const UUID collider : report->Colliders)
				result.Colliders.push_back(Utils::SummarizeEntityId(context, scene, collider));
			result.Position = report->Pose.Position;
			result.Rotation = report->Pose.Rotation;
			result.LinearVelocity = report->LinearVelocity;
			result.AngularVelocity = report->AngularVelocity;
			result.Sleeping = report->IsSleeping;
			if (!report->Bounds.IsEmpty())
			{
				result.BoundsMin = report->Bounds.Min;
				result.BoundsMax = report->Bounds.Max;
			}
			result.Contacts.reserve(report->Contacts.size());
			for (const PhysicsContactInfo& contact : report->Contacts)
			{
				result.Contacts.push_back(PhysicsContactSummary{
					.Other = Utils::SummarizeEntityId(context, scene, contact.Other),
					.Collider = Utils::SummarizeEntityId(context, scene, contact.Collider),
					.OtherCollider = Utils::SummarizeEntityId(context, scene, contact.OtherCollider),
					.IsTrigger = contact.IsTrigger,
					.SinceTick = ToAutomationCounter(contact.SinceTick),
				});
			}
			if (report->Character.has_value())
			{
				result.Grounded = report->Character->IsGrounded;
				result.GroundNormal = report->Character->GroundNormal;
			}
			result.Tick = ToAutomationCounter(session->GetTick());
			return result;
		}

	}

	void RegisterPhysicsMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<PhysicsBodyOrigin>("PhysicsBodyOrigin", "Why a physics body exists (§5.3 composition rules).")
			.Entry(PhysicsBodyOrigin::RigidBody, "RigidBody", "The owner's RigidBody, with its own colliders and those of collider-only descendants.")
			.Entry(PhysicsBodyOrigin::ImplicitStatic, "ImplicitStatic", "Solid colliders without a RigidBody above them: a static body.")
			.Entry(PhysicsBodyOrigin::ImplicitSensor, "ImplicitSensor",
				"Trigger colliders on an entity without its own RigidBody: a kinematic sensor that follows the entity.")
			.Entry(PhysicsBodyOrigin::Character, "Character", "The owner's CharacterController; its body is the controller's inner capsule.");

		registry.Struct<PhysicsBodyInfoParams>("PhysicsBodyInfoParams", "The params of physics.bodyInfo.")
			.Field("entity", &PhysicsBodyInfoParams::Entity, "An entity of the play scene: 16 hex digits, a unique id prefix or a path.");

		registry.Struct<PhysicsContactSummary>("PhysicsContactSummary", "One active contact pair of a body, from its side.")
			.Field("other", &PhysicsContactSummary::Other, "The partner body's owner.")
			.Field("collider", &PhysicsContactSummary::Collider, "This body's collider entity of the first sub-shape pair that touched.")
			.Field("otherCollider", &PhysicsContactSummary::OtherCollider, "The partner's collider entity of that sub-shape pair.")
			.Field("isTrigger", &PhysicsContactSummary::IsTrigger, "A sensor pair (trigger events), not a collision.")
			.Field("sinceTick", &PhysicsContactSummary::SinceTick, "The tick whose physics step reported the pair's enter.");

		registry.Struct<PhysicsBodyInfoResult>("PhysicsBodyInfoResult", "The physics body of an entity of the play scene.")
			.Field("entity", &PhysicsBodyInfoResult::Entity, "The entity asked about.")
			.Field("body", &PhysicsBodyInfoResult::Body, "The body's owner: the entity itself, or the ancestor whose compound holds its colliders.")
			.Field("origin", &PhysicsBodyInfoResult::Origin, "Why the body exists.")
			.Field("type", &PhysicsBodyInfoResult::Type, "How the body moves; a Static RigidBody whose colliders are triggers is Kinematic.")
			.Field("isSensor", &PhysicsBodyInfoResult::IsSensor, "Whether the body is a trigger (detects overlaps without colliding).")
			.Field("layer", &PhysicsBodyInfoResult::Layer, "The body's physics layer (a name of the project's PhysicsSettings.Layers).")
			.Field("colliders", &PhysicsBodyInfoResult::Colliders, "The collider entities of the body, in sub-shape order; empty for a character.")
			.Field("position", &PhysicsBodyInfoResult::Position, "The body origin's world position in metres (a character's base).")
			.Field("rotation", &PhysicsBodyInfoResult::Rotation, "The body's world rotation.")
			.Field("linearVelocity", &PhysicsBodyInfoResult::LinearVelocity, "The linear velocity in m/s.")
			.Field("angularVelocity", &PhysicsBodyInfoResult::AngularVelocity, "The angular velocity in rad/s.")
			.Field("sleeping", &PhysicsBodyInfoResult::Sleeping, "Whether the body is asleep (at rest, not simulated until something wakes it).")
			.Field("boundsMin", &PhysicsBodyInfoResult::BoundsMin, "The minimum corner of the body's world bounding box, in metres.")
			.Field("boundsMax", &PhysicsBodyInfoResult::BoundsMax, "The maximum corner of the body's world bounding box, in metres.")
			.Field("contacts", &PhysicsBodyInfoResult::Contacts, "The body's active contact pairs, sorted by the partner's id.")
			.Field("grounded", &PhysicsBodyInfoResult::Grounded, "A character standing on walkable ground; false for other bodies.")
			.Field("groundNormal", &PhysicsBodyInfoResult::GroundNormal, "The normal of a grounded character's ground; (0, 1, 0) otherwise.")
			.Field("tick", &PhysicsBodyInfoResult::Tick, "The play session's tick when the body was read.");
	}

	void RegisterPhysicsMethods(MethodRegistry& methods)
	{
		Json example = Json::object();
		example["entity"] = "/Ball";
		methods.Add(
			{
				.Name = "physics.bodyInfo",
				.Description = "Reports the physics body of an entity of the play scene (the body it owns, or the compound its colliders belong "
							   "to): type, layer, colliders, pose, velocities, sleeping state, bounds and active contacts. Needs a play session.",
				.RequiredParams = { "entity" },
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Check whether the ball rests on the ground.", .Params = example } },
			},
			&Automation::PhysicsBodyInfo);
	}

}
