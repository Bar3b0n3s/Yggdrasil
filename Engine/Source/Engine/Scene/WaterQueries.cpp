#include "EnginePCH.h"
#include "Engine/Scene/WaterQueries.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace Engine {

	namespace Utils {

		// The length of a world matrix column, the scale along that local axis.
		static double ColumnLength(const glm::vec4& column)
		{
			const glm::dvec3 axis(column);
			return std::sqrt(glm::dot(axis, axis));
		}

		// The point's offset from the water's origin in the water's frame: (along local X, along local Z).
		static glm::dvec2 ToWaterLocal(const ResolvedWater& water, const glm::dvec3& point)
		{
			const WaterFrame& frame = water.Surface.Frame;
			const glm::dvec2 relative(point.x - frame.Origin.x, point.z - frame.Origin.z);
			return glm::dvec2(glm::dot(relative, frame.AxisX), glm::dot(relative, frame.AxisZ));
		}

		// The entities holding a Water that are not pending destruction, in canonical order.
		static std::vector<ConstEntity> CollectWaterEntities(const Scene& scene)
		{
			std::vector<ConstEntity> entities;
			const auto view = scene.GetRegistry().view<WaterComponent>();
			for (const entt::entity handle : view)
			{
				const ConstEntity entity(handle, &scene);
				if (scene.FindEntityByID(entity.GetUUID()).IsValid())
					entities.push_back(entity);
			}
			if (entities.size() < 2)
				return entities;
			// Several: their positions in the canonical order.
			std::vector<std::pair<size_t, ConstEntity>> positioned;
			positioned.reserve(entities.size());
			const std::span<const UUID> order = scene.GetCanonicalOrder();
			for (size_t position = 0; position < order.size() && positioned.size() < entities.size(); ++position)
			{
				const auto found = std::ranges::find_if(entities, [id = order[position]](const ConstEntity& entity)
				{
					return entity.GetUUID() == id;
				});
				if (found != entities.end())
					positioned.emplace_back(position, *found);
			}
			std::ranges::sort(positioned, {}, &std::pair<size_t, ConstEntity>::first);
			entities.clear();
			for (const auto& [position, entity] : positioned)
				entities.push_back(entity);
			return entities;
		}

		// The surface of `water` at the point's X and Z, as a sample for the point.
		static SceneWaterSample MakeSample(const ResolvedWater& water, const glm::dvec3& point, double time)
		{
			const WaterSurfaceSample surface = EvaluateWaterSurface(water.Surface, glm::dvec2(point.x, point.z), time);
			return SceneWaterSample{
				.Water = water.Entity,
				.Height = surface.Height,
				.Normal = surface.Normal,
				.Velocity = surface.Velocity,
				.Depth = surface.Height - point.y,
			};
		}

		// The water the rule chooses and its surface at the point.
		struct ChosenWater
		{
			const ResolvedWater* Water = nullptr;
			SceneWaterSample Sample{};
		};

		static std::optional<ChosenWater> ChooseWater(const SceneWaters& waters, const glm::dvec3& point, double time)
		{
			std::optional<ChosenWater> chosen;
			for (const ResolvedWater& rectangle : waters.Rectangles)
			{
				if (!IsInWaterVolume(rectangle, point))
					continue;
				const SceneWaterSample sample = MakeSample(rectangle, point, time);
				// Strictly higher only, so a tie keeps the earlier water in canonical order.
				if (!chosen.has_value() || sample.Height > chosen->Sample.Height)
					chosen = ChosenWater{ .Water = &rectangle, .Sample = sample };
			}
			if (!chosen.has_value() && waters.Ocean.has_value())
				chosen = ChosenWater{ .Water = &*waters.Ocean, .Sample = MakeSample(*waters.Ocean, point, time) };
			return chosen;
		}

	}

	ResolvedWater ResolveWater(const WaterComponent& water, const glm::mat4& world, UUID id)
	{
		ResolvedWater resolved;
		resolved.Entity = id;
		resolved.Shape = water.Shape;
		resolved.Surface = ResolveWaterSurface(water.Waves, static_cast<double>(water.WaveScale), glm::dvec2(water.Current), MakeWaterFrame(glm::dmat4(world)));
		if (water.Shape == WaterShape::Rectangle)
		{
			resolved.HalfExtents = glm::dvec2(static_cast<double>(water.Size.x) * 0.5 * Utils::ColumnLength(world[0]),
				static_cast<double>(water.Size.y) * 0.5 * Utils::ColumnLength(world[2]));
			resolved.Depth = static_cast<double>(water.Depth);
			resolved.Headroom = static_cast<double>(water.Headroom);
		}
		return resolved;
	}

	ResolvedWater ResolveWater(ConstEntity entity)
	{
		ENGINE_CORE_ASSERT(entity.IsReadable() && entity.HasComponent<WaterComponent>(), "ResolveWater needs a readable entity with a Water");
		return ResolveWater(entity.GetComponent<WaterComponent>(), TransformSystem::ComputeWorldMatrix(entity), entity.GetUUID());
	}

	SceneWaters CollectSceneWaters(const Scene& scene)
	{
		SceneWaters waters;
		for (const ConstEntity entity : Utils::CollectWaterEntities(scene))
		{
			if (entity.HasComponent<HierarchyDisabledTag>())
				continue;
			const WaterComponent& water = entity.GetComponent<WaterComponent>();
			if (water.Shape == WaterShape::Rectangle)
				waters.Rectangles.push_back(ResolveWater(entity));
			else if (!waters.Ocean.has_value())
				waters.Ocean = ResolveWater(entity);
		}
		return waters;
	}

	bool IsInWaterFootprint(const ResolvedWater& water, const glm::dvec3& point)
	{
		if (water.Shape != WaterShape::Rectangle)
			return true;
		const glm::dvec2 local = Utils::ToWaterLocal(water, point);
		return std::abs(local.x) <= water.HalfExtents.x && std::abs(local.y) <= water.HalfExtents.y;
	}

	bool IsInWaterVolume(const ResolvedWater& water, const glm::dvec3& point)
	{
		if (water.Shape != WaterShape::Rectangle)
			return true;
		const double base = water.Surface.Frame.Origin.y;
		return IsInWaterFootprint(water, point) && point.y >= base - water.Depth && point.y <= base + water.Headroom;
	}

	std::optional<SceneWaterSample> SampleWater(const ResolvedWater& water, const glm::dvec3& point, double time)
	{
		if (!IsInWaterFootprint(water, point))
			return std::nullopt;
		return Utils::MakeSample(water, point, time);
	}

	bool IsUnderWater(const ResolvedWater& water, const glm::dvec3& point, double time)
	{
		return IsInWaterVolume(water, point) && Utils::MakeSample(water, point, time).Depth > 0.0;
	}

	const ResolvedWater* FindWaterAt(const SceneWaters& waters, const glm::dvec3& point, double time)
	{
		const std::optional<Utils::ChosenWater> chosen = Utils::ChooseWater(waters, point, time);
		return chosen.has_value() ? chosen->Water : nullptr;
	}

	std::optional<SceneWaterSample> SampleSceneWater(const SceneWaters& waters, const glm::dvec3& point, double time)
	{
		const std::optional<Utils::ChosenWater> chosen = Utils::ChooseWater(waters, point, time);
		if (!chosen.has_value())
			return std::nullopt;
		return chosen->Sample;
	}

	std::optional<SceneWaterSample> SampleSceneWater(const Scene& scene, const glm::dvec3& point, double time)
	{
		return SampleSceneWater(CollectSceneWaters(scene), point, time);
	}

}
