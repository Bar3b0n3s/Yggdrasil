#include "EnginePCH.h"
#include "Engine/Automation/Methods/Physics2DMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Assert.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>

// physics2d.raycast and physics2d.overlap (Docs/Decisions/0032-m21-contract.md decision 15) and physics2d.bodyInfo (decision
// 27): reads of the play session's Physics2DSystem, shared by the Editor and the Runtime. Params are checked here, located at
// their members, before the system sees them; the system's own refusals (an end point beyond the physics range) keep its
// message.

namespace Engine {

	namespace Utils {

		// The InvalidState of a 2D query without a play session (physics.bodyInfo's).
		static std::unexpected<Error> MakeNot2DPlayingError()
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "not playing: 2D physics bodies exist only in a play session")
					.WithHint("start one with play.start {mode: \"simulate\"} (physics without scripts and audio) or play.start, then step it"));
		}

		static bool IsFinite2D(const glm::vec2& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		// {id, name, path} of the entity `id` of `scene`, or just its id when the scene no longer holds it.
		static EntitySummary Summarize2DEntity(const AutomationMethodContext& context, const Scene& scene, UUID id)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (entity.IsValid())
				return context.MakeEntitySummary(entity);
			EntitySummary summary;
			summary.Id = FormatOptionalUUID(id);
			return summary;
		}

		// InvalidArgument at `pointer` unless `value` is a query size the 2D system accepts (a collider's size bounds).
		static Status Check2DQuerySize(float value, std::string_view pointer)
		{
			constexpr float Smallest = PhysicsShape::MinColliderSize / 2.0f;
			constexpr float Largest = PhysicsShape::MaxColliderSize / 2.0f;
			if (!std::isfinite(value) || !(value >= Smallest) || value > Largest)
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("{} is not from {} to {} m", value, Smallest, Largest),
					"give the query shape a collider's size"));
			}
			return {};
		}

		// The registry's BodyType of a 2D body's motion type as simulated.
		static BodyType ToBody2DType(PhysicsMotionType type)
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

		// Radians about +Z to degrees, the unit of every angle the 2D methods take and report.
		static float DegreesFromRadians2D(float radians)
		{
			return radians * (180.0f / std::numbers::pi_v<float>);
		}

	}

	namespace Automation {

		Result<Physics2DRaycastResult> Physics2DRaycast(AutomationMethodContext& context, const Physics2DRaycastParams& params)
		{
			PlaySession* session = context.GetPlaySession();
			if (session == nullptr || session->GetPhysics2D() == nullptr)
				return Utils::MakeNot2DPlayingError();
			if (!Utils::IsFinite2D(params.Origin))
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/origin", "the origin is not finite"));
			if (!Utils::IsFinite2D(params.Direction) || (params.Direction.x == 0.0f && params.Direction.y == 0.0f))
			{
				return std::unexpected(
					Utils::MakeParamError(ErrorCode::InvalidArgument, "/direction", "the direction is zero or not finite", "give a direction in the XY plane"));
			}
			if (!std::isfinite(params.MaxDistance) || !(params.MaxDistance > 0.0f))
			{
				return std::unexpected(
					Utils::MakeParamError(ErrorCode::InvalidArgument, "/maxDistance", std::format("{} is not a positive finite distance", params.MaxDistance)));
			}

			const Physics2DSystem& physics = *session->GetPhysics2D();
			std::vector<Physics2DRaycastHit> hits;
			if (params.All)
			{
				ENGINE_TRY_ASSIGN(hits, physics.RaycastAll(params.Origin, params.Direction, params.MaxDistance, params.LayerMask));
			}
			else
			{
				ENGINE_TRY_ASSIGN(const std::optional<Physics2DRaycastHit> hit,
					physics.Raycast(params.Origin, params.Direction, params.MaxDistance, params.LayerMask));
				if (hit.has_value())
					hits.push_back(*hit);
			}

			const Scene& scene = session->GetScene();
			Physics2DRaycastResult result;
			result.Hits.reserve(hits.size());
			for (const Physics2DRaycastHit& hit : hits)
			{
				result.Hits.push_back(Physics2DRaycastHitSummary{ .Entity = Utils::Summarize2DEntity(context, scene, hit.Entity),
					.Body = Utils::Summarize2DEntity(context, scene, hit.Body),
					.Point = hit.Point,
					.Normal = hit.Normal,
					.Distance = hit.Distance });
			}
			result.Tick = ToAutomationCounter(session->GetTick());
			return result;
		}

		Result<Physics2DOverlapResult> Physics2DOverlap(AutomationMethodContext& context, const Physics2DOverlapParams& params)
		{
			PlaySession* session = context.GetPlaySession();
			if (session == nullptr || session->GetPhysics2D() == nullptr)
				return Utils::MakeNot2DPlayingError();
			if (!Utils::IsFinite2D(params.Center))
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/center", "the centre is not finite"));

			const Physics2DSystem& physics = *session->GetPhysics2D();
			std::vector<PhysicsOverlapHit> hits;
			if (params.Shape == Physics2DQueryShape::Circle)
			{
				ENGINE_TRY(Utils::Check2DQuerySize(params.Radius, "/radius"));
				ENGINE_TRY_ASSIGN(hits, physics.OverlapCircle(params.Center, params.Radius, params.LayerMask));
			}
			else
			{
				ENGINE_TRY(Utils::Check2DQuerySize(params.HalfExtents.x, "/halfExtents/0"));
				ENGINE_TRY(Utils::Check2DQuerySize(params.HalfExtents.y, "/halfExtents/1"));
				if (!std::isfinite(params.Angle))
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/angle", "the angle is not finite"));
				ENGINE_TRY_ASSIGN(hits, physics.OverlapBox(params.Center, params.HalfExtents, params.Angle, params.LayerMask));
			}

			// Each collider entity once, sorted by id (the system sorts by collider, then body).
			const Scene& scene = session->GetScene();
			Physics2DOverlapResult result;
			std::optional<UUID> previous;
			for (const PhysicsOverlapHit& hit : hits)
			{
				if (previous == hit.Entity)
					continue;
				previous = hit.Entity;
				result.Entities.push_back(Utils::Summarize2DEntity(context, scene, hit.Entity));
			}
			result.Tick = ToAutomationCounter(session->GetTick());
			return result;
		}

		Result<Physics2DBodyInfoResult> Physics2DBodyInfo(AutomationMethodContext& context, const Physics2DBodyInfoParams& params)
		{
			PlaySession* session = context.GetPlaySession();
			if (session == nullptr || session->GetPhysics2D() == nullptr)
				return Utils::MakeNot2DPlayingError();
			Scene& scene = session->GetScene();
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(scene, params.Entity, "/entity"));
			const std::optional<Physics2DBodyReport> report = session->GetPhysics2D()->GetBodyInfo(entity.GetUUID());
			if (!report.has_value())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/entity",
					std::format("entity '{}' ({}) has no 2D physics body", entity.GetName(), entity.GetUUID().ToString()),
					"2D bodies come from a RigidBody2D or 2D colliders on an active entity; project.validate lists the bodies a diagnostic "
					"refuses"));
			}

			Physics2DBodyInfoResult result;
			result.Entity = context.MakeEntitySummary(entity);
			result.Body = Utils::Summarize2DEntity(context, scene, report->Owner);
			result.Origin = report->Origin;
			result.Type = Utils::ToBody2DType(report->MotionType);
			result.IsSensor = report->IsSensor;
			result.Layer = report->LayerName;
			result.Colliders.reserve(report->Colliders.size());
			for (const UUID collider : report->Colliders)
				result.Colliders.push_back(Utils::Summarize2DEntity(context, scene, collider));
			result.Position = report->Pose.Position;
			result.Angle = Utils::DegreesFromRadians2D(report->Pose.Angle);
			result.LinearVelocity = report->LinearVelocity;
			result.AngularVelocity = report->AngularVelocity;
			result.Sleeping = report->IsSleeping;
			result.BoundsMin = report->BoundsMin;
			result.BoundsMax = report->BoundsMax;
			result.Contacts.reserve(report->Contacts.size());
			for (const PhysicsContactInfo& contact : report->Contacts)
			{
				result.Contacts.push_back(PhysicsContactSummary{
					.Other = Utils::Summarize2DEntity(context, scene, contact.Other),
					.Collider = Utils::Summarize2DEntity(context, scene, contact.Collider),
					.OtherCollider = Utils::Summarize2DEntity(context, scene, contact.OtherCollider),
					.IsTrigger = contact.IsTrigger,
					.SinceTick = ToAutomationCounter(contact.SinceTick),
				});
			}
			result.Tick = ToAutomationCounter(session->GetTick());
			return result;
		}

	}

	void RegisterPhysics2DMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<Physics2DQueryShape>("Physics2DQueryShape", "The shape of a 2D overlap query.")
			.Entry(Physics2DQueryShape::Circle, "Circle", "A circle of radius `radius` around `center`.")
			.Entry(Physics2DQueryShape::Box, "Box", "A rectangle of half extents `halfExtents` around `center`, rotated by `angle` degrees.");

		registry.Struct<Physics2DRaycastParams>("Physics2DRaycastParams", "The params of physics2d.raycast.")
			.Field("origin", &Physics2DRaycastParams::Origin, "Where the ray starts, in the XY plane, in metres.")
			.Field("direction", &Physics2DRaycastParams::Direction, "The ray's direction in the XY plane; normalized, not zero.")
			.Field("maxDistance", &Physics2DRaycastParams::MaxDistance, "How far the ray reaches, in metres; positive.")
			.Field("layerMask", &Physics2DRaycastParams::LayerMask, "The physics layers hit: bit i is the project's Physics.Layers[i]; all by default.")
			.Field("all", &Physics2DRaycastParams::All, "Whether every hit is reported, sorted by distance then collider id, instead of the closest.");

		registry.Struct<Physics2DRaycastHitSummary>("Physics2DRaycastHitSummary", "One 2D ray hit.")
			.Field("entity", &Physics2DRaycastHitSummary::Entity, "The collider entity that was hit.")
			.Field("body", &Physics2DRaycastHitSummary::Body, "The owner of the 2D body the collider belongs to.")
			.Field("point", &Physics2DRaycastHitSummary::Point, "Where the ray hits, in the XY plane, in metres.")
			.Field("normal", &Physics2DRaycastHitSummary::Normal, "The surface normal at the hit, unit, against the ray.")
			.Field("distance", &Physics2DRaycastHitSummary::Distance, "How far along the ray the hit is, in metres.");

		registry.Struct<Physics2DRaycastResult>("Physics2DRaycastResult", "The hits of a 2D ray in the play scene.")
			.Field("hits", &Physics2DRaycastResult::Hits, "The hits: none for a miss, the closest one without `all`.")
			.Field("tick", &Physics2DRaycastResult::Tick, "The play session's tick when the query ran.");

		registry.Struct<Physics2DOverlapParams>("Physics2DOverlapParams", "The params of physics2d.overlap.")
			.Field("shape", &Physics2DOverlapParams::Shape, "The query shape.")
			.Field("center", &Physics2DOverlapParams::Center, "The shape's centre, in the XY plane, in metres.")
			.Field("radius", &Physics2DOverlapParams::Radius, "Circle: the radius in metres.")
			.Field("halfExtents", &Physics2DOverlapParams::HalfExtents, "Box: half the rectangle's size along X and Y before its rotation, in metres.")
			.Field("angle", &Physics2DOverlapParams::Angle, "Box: the rectangle's rotation about +Z, counter-clockwise, in degrees.")
			.Field("layerMask", &Physics2DOverlapParams::LayerMask, "The physics layers found: bit i is the project's Physics.Layers[i]; all by default.");

		registry.Struct<Physics2DOverlapResult>("Physics2DOverlapResult", "The 2D colliders a shape overlaps in the play scene.")
			.Field("entities", &Physics2DOverlapResult::Entities, "The overlapping collider entities, each once, sorted by id.")
			.Field("tick", &Physics2DOverlapResult::Tick, "The play session's tick when the query ran.");

		registry.Struct<Physics2DBodyInfoParams>("Physics2DBodyInfoParams", "The params of physics2d.bodyInfo.")
			.Field("entity", &Physics2DBodyInfoParams::Entity, "An entity of the play scene: 16 hex digits, a unique id prefix or a path.");

		registry.Struct<Physics2DBodyInfoResult>("Physics2DBodyInfoResult", "The 2D physics body of an entity of the play scene.")
			.Field("entity", &Physics2DBodyInfoResult::Entity, "The entity asked about.")
			.Field("body", &Physics2DBodyInfoResult::Body, "The body's owner: the entity itself, or the ancestor whose compound holds its colliders.")
			.Field("origin", &Physics2DBodyInfoResult::Origin,
				"Why the 2D body exists, the entries meaning the 2D components: RigidBody is the owner's RigidBody2D, ImplicitStatic the solid "
				"2D colliders without a RigidBody2D above them, ImplicitSensor the trigger 2D colliders of an entity without its own RigidBody2D; "
				"never Character.")
			.Field("type", &Physics2DBodyInfoResult::Type, "How the body moves; a Static RigidBody2D whose colliders are triggers is Kinematic.")
			.Field("isSensor", &Physics2DBodyInfoResult::IsSensor, "Whether the body is a trigger (detects overlaps without colliding).")
			.Field("layer", &Physics2DBodyInfoResult::Layer, "The body's physics layer (a name of the project's PhysicsSettings.Layers).")
			.Field("colliders", &Physics2DBodyInfoResult::Colliders,
				"The collider entities of the body, in sub-shape order, each once (a tilemap collider's entity for all its boxes).")
			.Field("position", &Physics2DBodyInfoResult::Position, "The body origin's position in the XY plane, in metres.")
			.Field("angle", &Physics2DBodyInfoResult::Angle, "The body's rotation about +Z, counter-clockwise, in degrees.")
			.Field("linearVelocity", &Physics2DBodyInfoResult::LinearVelocity, "The linear velocity in the XY plane, in m/s.")
			.Field("angularVelocity", &Physics2DBodyInfoResult::AngularVelocity, "The angular velocity about +Z, in rad/s.")
			.Field("sleeping", &Physics2DBodyInfoResult::Sleeping, "Whether the body is asleep (at rest, not simulated until something wakes it).")
			.Field("boundsMin", &Physics2DBodyInfoResult::BoundsMin, "The minimum corner of the body's world rectangle in the XY plane, in metres.")
			.Field("boundsMax", &Physics2DBodyInfoResult::BoundsMax, "The maximum corner of the body's world rectangle in the XY plane, in metres.")
			.Field("contacts", &Physics2DBodyInfoResult::Contacts, "The body's active contact pairs in the 2D world, sorted by the partner's id.")
			.Field("tick", &Physics2DBodyInfoResult::Tick, "The play session's tick when the body was read.");
	}

	void RegisterPhysics2DMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "physics2d.raycast",
				.Description = "Casts a ray in the play scene's 2D physics world (the XY plane) and reports the closest hit, or every hit with "
							   "`all`: the collider entity, its body's owner, the point, the normal and the distance. Needs a play session.",
				.RequiredParams = { "origin", "direction", "maxDistance" },
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Look for the ground below the player.",
					.Params = Json{ { "origin", { 0, 2 } }, { "direction", { 0, -1 } }, { "maxDistance", 10 } } } },
			},
			&Automation::Physics2DRaycast);
		methods.Add(
			{
				.Name = "physics2d.overlap",
				.Description = "Lists the 2D colliders of the play scene that a circle or a rotated rectangle in the XY plane overlaps, each "
							   "once, sorted by id. Needs a play session.",
				.RequiredParams = { "shape", "center" },
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Find what touches a point.", .Params = Json{ { "shape", "Circle" }, { "center", { 1, 0 } }, { "radius", 0.1 } } } },
			},
			&Automation::Physics2DOverlap);
		methods.Add(
			{
				.Name = "physics2d.bodyInfo",
				.Description = "Reports the 2D physics body of an entity of the play scene (the body it owns, or the compound its colliders belong "
							   "to): type, layer, colliders, position and angle in the XY plane, velocities, sleeping state, bounds and active "
							   "contacts. Needs a play session.",
				.RequiredParams = { "entity" },
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Check whether the crate rests on the ground.", .Params = Json{ { "entity", "/Crate" } } } },
			},
			&Automation::Physics2DBodyInfo);
	}

}
