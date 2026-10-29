#include "EnginePCH.h"
#include "Engine/Automation/Methods/Physics2DMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// physics2d.raycast and physics2d.overlap (Docs/Decisions/0032-m21-contract.md decision 15): reads of the play session's
// Physics2DSystem, shared by the Editor and the Runtime. The contract writes the wire format and the registrations; stream P2
// implements the handlers and has RegisterSharedMethods call the registrations.

namespace Engine {

	namespace Automation {

		Result<Physics2DRaycastResult> Physics2DRaycast(AutomationMethodContext& /*context*/, const Physics2DRaycastParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "physics2d.raycast (Docs/Decisions/0032-m21-contract.md decision 15, stream P2)");
		}

		Result<Physics2DOverlapResult> Physics2DOverlap(AutomationMethodContext& /*context*/, const Physics2DOverlapParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "physics2d.overlap (Docs/Decisions/0032-m21-contract.md decision 15, stream P2)");
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
