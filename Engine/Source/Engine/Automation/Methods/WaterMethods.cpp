#include "EnginePCH.h"
#include "Engine/Automation/Methods/WaterMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/WaterQueries.h"
#include "Engine/Session/PlaySession.h"

#include <format>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		// The water's entity as {id, name, path} (EntitySummary's registered names).
		static VariantValue SummarizeWater(const AutomationMethodContext& context, const Scene& scene, UUID id)
		{
			const ConstEntity entity = scene.FindReadableEntityByID(id);
			EntitySummary summary;
			if (entity.IsValid())
				summary = context.MakeEntitySummary(entity);
			else
				summary.Id = FormatOptionalUUID(id);
			return VariantValue(Json{ { "id", summary.Id }, { "name", summary.Name }, { "path", summary.Path } });
		}

		static WaterSamplePoint MakeWaterSamplePoint(const AutomationMethodContext& context, const Scene& scene, const SceneWaterSample& sample)
		{
			return WaterSamplePoint{
				.Water = SummarizeWater(context, scene, sample.Water),
				.Height = static_cast<float>(sample.Height),
				.Normal = glm::vec3(sample.Normal),
				.Velocity = glm::vec3(sample.Velocity),
				.Depth = static_cast<float>(sample.Depth),
				.Underwater = sample.Depth > 0.0,
			};
		}

	}

	namespace Automation {

		Result<WaterSampleResult> WaterSample(AutomationMethodContext& context, const WaterSampleParams& params)
		{
			if (params.Points.empty() || params.Points.size() > MaxWaterSamplePoints)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/points",
					std::format("water.sample takes 1 to {} points, not {}", MaxWaterSamplePoints, params.Points.size()),
					"split the points over several calls"));
			}
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));

			std::optional<ResolvedWater> own;
			if (!params.Entity.empty())
			{
				ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
				if (!entity.HasComponent<WaterComponent>())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/entity",
						std::format("'{}' has no Water component", scene->GetEntityPath(entity)),
						"name an entity with a Water, or omit entity for the water that holds each point"));
				}
				own = ResolveWater(entity);
			}

			// The edit scene at `time` or 0; the play scene at the world's time unless `time` is given.
			double time = static_cast<double>(params.Time);
			const PlaySession* session = context.GetPlaySession();
			if (!context.HasParam("time") && session != nullptr && &session->GetScene() == scene)
				time = static_cast<double>(session->GetWorldTick()) * session->GetFixedDelta();

			WaterSampleResult result;
			result.Scene = context.MakeSceneSummary(*scene);
			result.Time = static_cast<float>(time);
			result.Points.reserve(params.Points.size());
			const SceneWaters waters = own.has_value() ? SceneWaters{} : CollectSceneWaters(*scene);
			for (const glm::vec3& point : params.Points)
			{
				const glm::dvec3 position(point);
				std::optional<SceneWaterSample> sample;
				bool underwater = false;
				if (own.has_value())
				{
					sample = SampleWater(*own, position, time);
					underwater = IsUnderWater(*own, position, time);
				}
				else
				{
					sample = SampleSceneWater(waters, position, time);
					underwater = sample.has_value() && sample->Depth > 0.0;
				}
				WaterSamplePoint entry;
				if (sample.has_value())
				{
					entry = Utils::MakeWaterSamplePoint(context, *scene, *sample);
					entry.Underwater = underwater;
				}
				result.Points.push_back(std::move(entry));
			}
			return result;
		}

	}

	void RegisterWaterMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<WaterSampleParams>("WaterSampleParams", "The params of water.sample.")
			.Field("points", &WaterSampleParams::Points, "The world points to sample under, as [x, y, z], 1 to 4096 of them.")
			.Field("entity", &WaterSampleParams::Entity,
				"An entity with a Water (16 hex digits, a unique id prefix or a path): that water's own surface, other waters' masks aside; "
				"omitted: the water that holds each point.")
			.Field("time", &WaterSampleParams::Time,
				"The world time to sample at, in seconds; omitted: 0 in the edit scene, the world's time now (the world tick times the fixed "
				"delta) in the play scene.",
				{ .Min = 0.0, .Unit = "s" })
			.Field("target", &WaterSampleParams::Target, "The scene to read; omitted: the play scene while playing, else the edit scene.");

		registry.Struct<WaterSamplePoint>("WaterSamplePoint", "The water surface under one world point.")
			.Field("water", &WaterSamplePoint::Water,
				"The water's entity as {id, name, path}; null where no water holds the point (or, with entity, outside its Rectangle's "
				"footprint), and then normal is +Y, underwater false and the others 0.")
			.Field("height", &WaterSamplePoint::Height, "The surface's world height under the point, in metres.", { .Unit = "m" })
			.Field("normal", &WaterSamplePoint::Normal, "The surface's world unit normal under the point.")
			.Field("velocity", &WaterSamplePoint::Velocity,
				"The surface's velocity under the point, in metres per second: the waves' motion there plus the water's current.", { .Unit = "m/s" })
			.Field("depth", &WaterSamplePoint::Depth, "The surface's height minus the point's y, in metres: positive under water.", { .Unit = "m" })
			.Field("underwater", &WaterSamplePoint::Underwater, "Whether the point lies under the surface (and within a Rectangle's volume).");

		registry.Struct<WaterSampleResult>("WaterSampleResult", "The water surface under each point of water.sample.")
			.Field("scene", &WaterSampleResult::Scene, "The scene sampled.")
			.Field("time", &WaterSampleResult::Time, "The world time sampled at, in seconds.", { .Unit = "s" })
			.Field("points", &WaterSampleResult::Points, "The surface under each point, in the order of the params' points.");
	}

	void RegisterWaterMethods(MethodRegistry& methods)
	{
		Json example = Json::object();
		example["points"] = Json::array({ Json::array({ 0, 0, 0 }), Json::array({ 12.5, -1, 40 }) });
		Json entityExample = Json::object();
		entityExample["points"] = Json::array({ Json::array({ 3, 2, -6 }) });
		entityExample["entity"] = "/Pool";
		entityExample["time"] = 2.5;
		methods.Add(
			{
				.Name = "water.sample",
				.Description = "Samples the water surface under world points, as scripts and the physics read it: per point the water that holds it "
							   "(the Rectangle whose volume holds it with the highest surface there, else the scene's Ocean), or a given water's own "
							   "surface, with the height, normal, velocity and depth there and whether the point is under water; at a given world "
							   "time, else 0 in the edit scene and the world's time in the play scene.",
				.RequiredParams = { "points" },
				.SupportsSceneTarget = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the sea's height and depth at two points.", .Params = example },
					{ .Description = "Read the pool's own surface over a point at 2.5 s.", .Params = entityExample } },
			},
			&Automation::WaterSample);
	}

}
