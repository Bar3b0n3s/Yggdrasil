#include "EnginePCH.h"
#include "Engine/Automation/Methods/Physics2DMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <format>
#include <optional>
#include <string_view>

// physics2d.raycast and physics2d.overlap (Docs/Decisions/0032-m21-contract.md decision 15): reads of the play session's
// Physics2DSystem, shared by the Editor and the Runtime. Params are checked here, located at their members, before the system
// sees them; the system's own refusals (an end point beyond the physics range) keep its message.

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
	}

}
