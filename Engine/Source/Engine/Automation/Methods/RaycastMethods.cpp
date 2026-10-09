#include "EnginePCH.h"
#include "Engine/Automation/Methods/RaycastMethods.h"

#include "Engine/Automation/Methods/Private/RenderQuerySupport.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <cmath>

namespace Engine {

	Result<SceneRaycastResult> Automation::SceneRaycast(AutomationMethodContext& context, const SceneRaycastParams& params)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!std::isfinite(params.Origin[axis]))
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/origin", "ray origin must be finite"));
			if (!std::isfinite(params.Direction[axis]))
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/direction", "ray direction must be finite"));
		}
		if (params.Direction == glm::vec3(0))
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/direction", "ray direction must be nonzero"));
		if (!std::isfinite(params.MaxDistance) || params.MaxDistance <= 0)
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/maxDistance", "ray distance must be positive and finite"));
		ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
		AssetManager* assets = context.GetAssets();
		if (assets == nullptr)
			return MakeError(ErrorCode::Unsupported, "this host has no asset manager for visual queries");
		ENGINE_TRY_ASSIGN(const PhysicsLayerTable layers, Utils::GetQueryLayers(context));
		ENGINE_TRY_ASSIGN(const RenderSnapshot view, Utils::ExtractQueryView(context, *scene, {}));
		ENGINE_TRY_ASSIGN(const auto hit, RaycastScene(*scene, *assets, layers, { .Ray = { params.Origin, params.Direction }, .MaxDistance = params.MaxDistance, .LayerMask = params.LayerMask, .Alpha = view.Alpha }));
		return Utils::SummarizeVisualHit(context, *scene, hit);
	}

	void RegisterRaycastMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<SceneRaycastParams>("SceneRaycastParams", "A CPU query of visual triangles, independent of physics bodies.")
			.Field("origin", &SceneRaycastParams::Origin, "Finite world-space ray origin in metres.")
			.Field("direction", &SceneRaycastParams::Direction, "Finite nonzero world direction; normalized by the query.")
			.Field("maxDistance", &SceneRaycastParams::MaxDistance, "Positive finite maximum distance in metres, inclusive.", { .Min = 0 })
			.Field("layerMask", &SceneRaycastParams::LayerMask, "Physics-layer bits inherited from the nearest rigid-body ancestor, or the entity's character, else Default.")
			.Field("target", &SceneRaycastParams::Target, "Scene to query; defaults to play while playing and edit otherwise.");
		registry.Struct<SceneRaycastResult>("SceneRaycastResult", "The nearest visual triangle, or an explicit miss with empty entity and zero values.")
			.Field("hit", &SceneRaycastResult::Hit, "Whether a triangle intersects the ray interval.")
			.Field("entity", &SceneRaycastResult::Entity, "The hit entity's id, name and path.")
			.Field("position", &SceneRaycastResult::Position, "World-space hit position in metres.")
			.Field("normal", &SceneRaycastResult::Normal, "Unit geometric normal facing against the ray.")
			.Field("barycentric", &SceneRaycastResult::Barycentric, "Weights of the triangle's three indexed vertices.")
			.Field("distance", &SceneRaycastResult::Distance, "World distance along the normalized ray.")
			.Field("submesh", &SceneRaycastResult::Submesh, "Zero-based submesh index.")
			.Field("triangle", &SceneRaycastResult::Triangle, "Zero-based triangle index within the submesh.");
	}

	void RegisterRaycastMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "scene.raycast",
						.Description = "Finds the nearest visible mesh triangle on a CPU ray, including both faces and Mask/Blend geometry without texture alpha testing. Does not step physics or change selection.",
						.RequiredParams = { "origin", "direction", "maxDistance" },
						.SupportsDryRun = true,
						.AvailableInRuntime = true,
						.AllowedInBatch = true,
						.Examples = { { .Description = "Query forward from the camera.", .Params = Json{ { "origin", { 0, 0, 10 } }, { "direction", { 0, 0, -1 } }, { "maxDistance", 100 } } } } },
			&Automation::SceneRaycast);
	}

}
