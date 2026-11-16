#include "EnginePCH.h"
#include "Engine/Scene/NavigationValidation.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/NavMeshData.h"
#include "Engine/Scene/Components/NavigationComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/NavigationBakeInput.h"
#include "Engine/Scene/Scene.h"

#include <format>
#include <optional>
#include <utility>

// The navigation checks of a scene (NavigationValidation.h; Docs/Decisions/0045-m30-navigation.md decision 9).

namespace Engine {

	namespace Utils {

		constexpr std::string_view NavigationBakeHint = "Bake the scene's navigation with navigation.bake (the editor's Bake Navigation) and save the scene.";

		[[nodiscard]] static NavigationSceneIssue MakeNavigationIssue(std::string_view code, ConstEntity entity, std::string message)
		{
			NavigationSceneIssue issue;
			issue.Code = std::string(code);
			issue.Severity = DiagnosticSeverity::Warning;
			issue.Entity = entity.GetUUID();
			issue.Message = std::move(message);
			issue.Hint = std::string(NavigationBakeHint);
			return issue;
		}

	}

	std::vector<NavigationSceneIssue> FindNavigationSceneIssues(const Scene& scene, AssetManager& assets, const PhysicsLayerTable* layers)
	{
		std::vector<NavigationSceneIssue> issues;
		if (scene.GetRegistry().view<NavigationComponent>().empty())
			return issues;
		std::optional<ConstEntity> owner;
		scene.ForEachCanonical([&owner](ConstEntity entity)
		{
			if (!owner.has_value() && entity.IsActive() && entity.HasComponent<NavigationComponent>())
				owner = entity;
		});
		if (!owner.has_value())
			return issues;

		const AssetHandle handle = owner->GetComponent<NavigationComponent>().NavMesh.GetHandle();
		AssetRef<NavMeshData> mesh;
		if (handle.IsValid())
		{
			const Result<AssetRef<Asset>> loaded = assets.Load(handle);
			if (loaded.has_value())
				mesh = AssetCast<NavMeshData>(*loaded);
		}
		if (mesh == nullptr)
		{
			issues.push_back(Utils::MakeNavigationIssue(NavMeshNotBakedCode, *owner,
				handle.IsValid()
					? std::format("the navigation mesh of scene '{}' ({}) does not load: every navigation query finds nothing", scene.GetName(), handle.ToString())
					: std::format("the navigation of scene '{}' is not baked: every navigation query finds nothing", scene.GetName())));
			return issues;
		}
		if (layers == nullptr)
			return issues;

		const Result<NavigationBakeInput> input = CollectNavigationBakeInput(scene, *layers, assets);
		if (!input.has_value())
		{
			issues.push_back(Utils::MakeNavigationIssue(NavMeshStaleCode, *owner,
				std::format("the navigation mesh of scene '{}' cannot be checked against the scene, whose bake input cannot be read: {}", scene.GetName(),
					input.error().GetMessageText())));
		}
		else if (HashNavigationBakeInput(*input) != mesh->InputHash)
		{
			issues.push_back(Utils::MakeNavigationIssue(NavMeshStaleCode, *owner,
				std::format("the navigation mesh of scene '{}' is stale: its static colliders, NavModifiers or Navigation settings changed after the bake",
					scene.GetName())));
		}
		return issues;
	}

}
