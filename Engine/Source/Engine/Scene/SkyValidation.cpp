#include "EnginePCH.h"
#include "Engine/Scene/SkyValidation.h"

#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <format>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static SkySceneIssue MakeSkyLightIssue(UUID sky, std::string field, std::string message, std::string hint)
		{
			return SkySceneIssue{ .Code = std::string(SkyLightInvalidCode),
				.Severity = DiagnosticSeverity::Warning,
				.Entity = sky,
				.Component = "Sky",
				.Field = std::move(field),
				.Message = std::move(message),
				.Hint = std::move(hint) };
		}

	}

	std::vector<SkySceneIssue> FindSkyIssues(const Scene& scene)
	{
		std::vector<SkySceneIssue> issues;
		if (scene.GetRegistry().view<SkyComponent>().empty())
			return issues;
		std::optional<ConstEntity> sky;
		std::optional<ConstEntity> firstLight;
		std::vector<ConstEntity> environments;
		scene.ForEachCanonical([&sky, &firstLight, &environments](ConstEntity entity)
		{
			if (entity.HasComponent<HierarchyDisabledTag>())
				return;
			if (!sky.has_value() && entity.HasComponent<SkyComponent>())
				sky = entity;
			if (!firstLight.has_value() && entity.HasComponent<DirectionalLightComponent>())
				firstLight = entity;
			if (entity.HasComponent<EnvironmentComponent>())
				environments.push_back(entity);
		});
		if (!sky.has_value())
			return issues;

		const SkyComponent& component = sky->GetComponent<SkyComponent>();
		const UUID skyId = sky->GetUUID();
		// The Sun's entity, as the renderer resolves it; empty when there is none.
		std::optional<UUID> sunEntity;
		if (component.Sun.IsValid())
		{
			const ConstEntity sun = scene.FindEntityByID(component.Sun);
			if (sun.IsValid())
			{
				if (!sun.HasComponent<DirectionalLightComponent>())
					issues.push_back(Utils::MakeSkyLightIssue(skyId, "Sun", std::format("the Sky's Sun names '{}', which has no DirectionalLight", sun.GetName()),
						"Set Sun to the entity of the scene's sun DirectionalLight, or to null for the scene's first DirectionalLight."));
				else if (sun.HasComponent<HierarchyDisabledTag>())
					issues.push_back(Utils::MakeSkyLightIssue(skyId, "Sun", std::format("the Sky's Sun names '{}', which is disabled", sun.GetName()),
						"Enable the Sun's entity, or set Sun to an enabled DirectionalLight's entity."));
				else
					sunEntity = component.Sun;
			}
		}
		else if (!firstLight.has_value())
			issues.push_back(Utils::MakeSkyLightIssue(skyId, "Sun", "the Sky has no Sun: its Sun is null and the scene has no enabled DirectionalLight",
				"Add a DirectionalLight for the sun (entity.create {components: {DirectionalLight: {}}}) and name it in the Sky's Sun."));
		else
			sunEntity = firstLight->GetUUID();
		if (component.Moon.IsValid() && sunEntity.has_value() && component.Moon == *sunEntity)
			issues.push_back(Utils::MakeSkyLightIssue(skyId, "Moon", "the Sky's Moon is its Sun's entity, so no moon is drawn",
				"Set Moon to a second entity, normally a dim DirectionalLight turned toward the moon, or to null for no moon."));

		for (const ConstEntity environment : environments)
			issues.push_back(SkySceneIssue{ .Code = std::string(SkyEnvironmentIgnoredCode),
				.Severity = DiagnosticSeverity::Warning,
				.Entity = environment.GetUUID(),
				.Component = "Environment",
				.Field = {},
				.Message = std::format("the Environment on '{}' is unused: the scene's Sky lights it and draws its sky", environment.GetName()),
				.Hint = "Remove the Environment (or disable its entity), or remove the Sky to light the scene with the Environment's map." });
		return issues;
	}

}
