#include "EditorPCH.h"
#include "EditorCore/Automation/NavigationMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Navigation/NavigationBake.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <format>
#include <utility>

// navigation.bake (Docs/Decisions/0045-m30-navigation.md decision 8): BakeNavigation on the request's scene, its result as the
// method's summaries.

namespace Engine {

	namespace Automation {

		Result<NavigationBakeMethodResult> NavigationBakeMethod(EditorMethodContext& context, const NoParams& /*params*/)
		{
			ENGINE_TRY_ASSIGN(const OpenSceneId scene, context.ResolveSceneTarget());
			ENGINE_TRY_ASSIGN(const NavigationBakeResult baked, BakeNavigation(context.GetEditor(), scene));
			const Scene& bakedScene = context.GetEditor().GetScene();
			NavigationBakeMethodResult result;
			result.Entity = context.MakeEntitySummary(bakedScene.FindEntityByID(baked.Entity));
			result.Path = baked.Path;
			result.Asset = AssetSummary{ .Id = baked.Asset.ToString(), .Path = baked.Path, .Type = AssetType::NavMesh };
			result.Changed = baked.Changed;
			result.InputHash = std::format("{:016x}", baked.InputHash);
			for (const NavigationBakeAgentSummary& agent : baked.Agents)
				result.Agents.push_back({ .Name = agent.Name, .Polygons = agent.Polygons, .Vertices = agent.Vertices });
			for (const UUID& owner : baked.SkippedBodies)
				result.SkippedBodies.push_back(context.MakeEntitySummary(bakedScene.FindEntityByID(owner)));
			return result;
		}

	}

	void RegisterNavigationMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<NavigationBakeAgent>("NavigationBakeAgent", "One agent type's baked navigation mesh.")
			.Field("name", &NavigationBakeAgent::Name, "The agent type's name.")
			.Field("polygons", &NavigationBakeAgent::Polygons, "The mesh's walkable polygons (0 when the agent type walks nowhere).")
			.Field("vertices", &NavigationBakeAgent::Vertices, "The mesh's polygon vertices.");
		registry.Struct<NavigationBakeMethodResult>("NavigationBakeMethodResult", "The navigation mesh navigation.bake baked.")
			.Field("entity", &NavigationBakeMethodResult::Entity, "The entity whose Navigation component now references the mesh.")
			.Field("path", &NavigationBakeMethodResult::Path,
				"The written source file, project-relative: Assets/Navigation/<the scene's path below Assets/ without .scene>.navmesh.")
			.Field("asset", &NavigationBakeMethodResult::Asset, "The baked NavMesh asset, which the Navigation component's NavMesh now references.")
			.Field("changed", &NavigationBakeMethodResult::Changed, "Whether the file's bytes changed (false when a rebake found the same input).")
			.Field("inputHash", &NavigationBakeMethodResult::InputHash,
				"The bake input's hash (16 lowercase hex digits), which project.validate compares to report NAV_MESH_STALE.")
			.Field("agents", &NavigationBakeMethodResult::Agents, "Each agent type's mesh, in the Navigation component's order.")
			.Field("skippedBodies", &NavigationBakeMethodResult::SkippedBodies,
				"Static bodies whose shape could not be described (PHYSICS_INVALID_SHAPE in project.validate), left out of the bake.");
	}

	void RegisterNavigationMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "navigation.bake",
				.Description = "Bakes an edit scene's navigation mesh: voxelizes the static colliders on the Navigation component's layers for each "
							   "of its agent types, marks its NavModifiers, writes the mesh for the saved scene "
							   "(Assets/Navigation/<the scene's path below Assets/ without .scene>.navmesh, so Assets/Scenes/Arena.scene bakes "
							   "into Assets/Navigation/Scenes/Arena.navmesh), imports it and sets the component's NavMesh in one undoable edit. "
							   "CPU only. Two bakes of one scene write byte-identical files.",
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsSceneTarget = true,
				.TimeoutSeconds = 600,
				.Examples = { { .Description = "Bake the active scene's navigation mesh.", .Params = Json::object() } },
			},
			&Automation::NavigationBakeMethod);
	}

}
