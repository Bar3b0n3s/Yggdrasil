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

// physics.bodyInfo (Docs/Decisions/0014-m11-decisions.md decision 18) and physics.jointInfo (M37,
// Docs/Decisions/0054-m37-joints.md decision 10): reads of the play session's PhysicsSystem, mapped to the reflected results.
// Shared by the Editor and the Runtime.

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

		// SummarizeEntityId's summary as JSON ({id, name, path}, EntitySummary's registered names), or null for the invalid UUID.
		static VariantValue SummarizeNullableEntityId(const AutomationMethodContext& context, const Scene& scene, UUID id)
		{
			if (!id.IsValid())
				return VariantValue();
			const EntitySummary summary = SummarizeEntityId(context, scene, id);
			return VariantValue(Json{ { "id", summary.Id }, { "name", summary.Name }, { "path", summary.Path } });
		}

		// The registry's JointMotorMode of the world's motor.
		static JointMotorMode ToJointMotorMode(PhysicsJointMotor motor)
		{
			switch (motor)
			{
				case PhysicsJointMotor::Off:      return JointMotorMode::Off;
				case PhysicsJointMotor::Velocity: return JointMotorMode::Velocity;
				case PhysicsJointMotor::Position: return JointMotorMode::Position;
			}
			ENGINE_CORE_ASSERT(false, "Unknown PhysicsJointMotor {}", std::to_underlying(motor));
			return JointMotorMode::Off;
		}

		// The play session, or InvalidState "not playing" naming what needs one.
		static Result<PlaySession*> RequirePhysicsSession(AutomationMethodContext& context, std::string_view what)
		{
			PlaySession* session = context.GetPlaySession();
			if (session == nullptr)
			{
				return std::unexpected(Error(ErrorCode::InvalidState, std::format("not playing: {} exist only in a play session", what))
						.WithHint("start one with play.start {mode: \"simulate\"} (physics without scripts and audio) or play.start, then step it"));
			}
			return session;
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
				if (report->Character->GroundCollider.IsValid())
					result.GroundCollider = Utils::SummarizeEntityId(context, scene, report->Character->GroundCollider);
				result.Squeezed = report->Character->IsSqueezed;
				if (report->Character->SqueezedBy.IsValid())
					result.SqueezedBy = Utils::SummarizeEntityId(context, scene, report->Character->SqueezedBy);
			}
			// M47: the buoyancy item's last application.
			if (report->Buoyancy.has_value())
			{
				result.Buoyancy = VariantValue(Json{ { "water", Utils::SummarizeNullableEntityId(context, scene, report->Buoyancy->Water).Get() },
					{ "submergedVolume", report->Buoyancy->SubmergedVolume },
					{ "totalVolume", report->Buoyancy->TotalVolume } });
			}
			result.Tick = ToAutomationCounter(session->GetTick());
			return result;
		}

		Result<PhysicsJointInfoResult> PhysicsJointInfo(AutomationMethodContext& context, const PhysicsJointInfoParams& params)
		{
			ENGINE_TRY_ASSIGN(PlaySession* const session, Utils::RequirePhysicsSession(context, "joints"));
			Scene& scene = session->GetScene();
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(scene, params.Entity, "/entity"));
			const std::optional<PhysicsJointReport> report = session->GetPhysics().GetJointInfo(entity.GetUUID());
			if (!report.has_value())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/entity",
					std::format("entity '{}' ({}) has no joint", entity.GetName(), entity.GetUUID().ToString()),
					"a joint is a HingeJoint, SliderJoint, DistanceJoint or FixedJoint component; physics.bodyInfo reports bodies"));
			}

			PhysicsJointInfoResult result;
			result.Entity = context.MakeEntitySummary(entity);
			result.Type = report->Type;
			result.Body = Utils::SummarizeNullableEntityId(context, scene, report->Body);
			result.ConnectedBody = Utils::SummarizeNullableEntityId(context, scene, report->Connected);
			result.Created = report->Created;
			result.Broken = report->Broken;
			result.Angle = report->Angle;
			result.Position = report->Position;
			result.Distance = report->Distance;
			result.Force = report->Force;
			result.Torque = report->Torque;
			result.Motor = Utils::ToJointMotorMode(report->Motor);
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
			.Field("sinceTick", &PhysicsContactSummary::SinceTick, "The world's tick whose physics step reported the pair's enter (play.state's worldTick, which holds while the world is paused, so after a world pause it is behind play.state's tick).");

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
			.Field("groundCollider", &PhysicsBodyInfoResult::GroundCollider,
				"The collider entity a grounded character's last update stood on (another character's entity when it stood on one), as "
				"CharacterController:GetGroundCollider reads it; empty for other bodies, a character in the air, and a ground that is disabled or "
				"destroyed.")
			.Field("squeezed", &PhysicsBodyInfoResult::Squeezed,
				"Whether a character's last update found it squeezed (a Kinematic body, or a Dynamic one not lighter than it, pushing it while "
				"something else holds it), as CharacterController:IsSqueezed reads it; false for other bodies.")
			.Field("squeezedBy", &PhysicsBodyInfoResult::SqueezedBy,
				"The owner of the body that squeezed a character in its last update, as CharacterController:GetSqueezingBody reads it; empty "
				"when it is not squeezed, for other bodies, and when that entity is disabled or destroyed.")
			.Field("buoyancy", &PhysicsBodyInfoResult::Buoyancy,
				"For a body whose owner holds a Buoyancy, what the physics last floated it with: {water (the Buoyancy's Water when set; else "
				"the water whose planes held the most of the submerged volume, null when nothing was submerged; as {id, name, path}), "
				"submergedVolume, totalVolume (m^3)}, kept while it sleeps and 0 while it is skipped as invalid; null for any other body.")
			.Field("tick", &PhysicsBodyInfoResult::Tick, "The play session's tick when the body was read.");

		// M37 (Docs/Decisions/0054-m37-joints.md decision 10).
		registry.Struct<PhysicsJointInfoParams>("PhysicsJointInfoParams", "The params of physics.jointInfo.")
			.Field("entity", &PhysicsJointInfoParams::Entity, "An entity of the play scene holding a joint: 16 hex digits, a unique id prefix or a path.");

		registry.Struct<PhysicsJointInfoResult>("PhysicsJointInfoResult", "The joint of an entity of the play scene, as the play session holds it.")
			.Field("entity", &PhysicsJointInfoResult::Entity, "The joint's entity.")
			.Field("type", &PhysicsJointInfoResult::Type, "Which joint component the entity holds.")
			.Field("body", &PhysicsJointInfoResult::Body,
				"The owner of the joint's body, the RigidBody at or above the joint's entity, as {id, name, path}; null when the joint has none.")
			.Field("connectedBody", &PhysicsJointInfoResult::ConnectedBody,
				"The owner of the body the joint joins its body to, as {id, name, path}; null when it joins the world (or the end resolves to "
				"no body).")
			.Field("created", &PhysicsJointInfoResult::Created,
				"Whether the joint is in force; false for a joint that is refused (PHYSICS_JOINT_INVALID), broken, disabled or not made yet.")
			.Field("broken", &PhysicsJointInfoResult::Broken, "The component's Broken: the joint broke (or was written broken) and joins nothing.")
			.Field("angle", &PhysicsJointInfoResult::Angle,
				"A hinge's angle in degrees from the pose it was made in, after the last physics step; 0 for the other types and while not "
				"in force.")
			.Field("position", &PhysicsJointInfoResult::Position,
				"A slider's position in metres along its axis from the pose it was made in, after the last physics step; 0 otherwise.")
			.Field("distance", &PhysicsJointInfoResult::Distance,
				"A distance joint's distance in metres between its anchors, after the last physics step; 0 otherwise.")
			.Field("force", &PhysicsJointInfoResult::Force,
				"The force the joint applied over the last collision step, in newtons, as the break test measures it; 0 while not in force.")
			.Field("torque", &PhysicsJointInfoResult::Torque,
				"The torque the joint applied over the last collision step, in newton metres; 0 for a distance joint and while not in force.")
			.Field("motor", &PhysicsJointInfoResult::Motor, "A hinge's or slider's motor; Off for distance and fixed joints.")
			.Field("tick", &PhysicsJointInfoResult::Tick, "The play session's tick when the joint was read.");
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

		// M37 (Docs/Decisions/0054-m37-joints.md decision 10).
		Json jointExample = Json::object();
		jointExample["entity"] = "/TrapDoor";
		methods.Add(
			{
				.Name = "physics.jointInfo",
				.Description = "Reports the joint (HingeJoint, SliderJoint, DistanceJoint or FixedJoint) of an entity of the play scene: the "
							   "bodies it joins, whether it is in force or broken, its angle, position or distance, its force and torque of the "
							   "last physics step and its motor. Needs a play session.",
				.RequiredParams = { "entity" },
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read how far the trap door has opened.", .Params = jointExample } },
			},
			&Automation::PhysicsJointInfo);
	}

}
