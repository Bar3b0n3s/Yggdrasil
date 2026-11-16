#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <string>
#include <vector>

namespace Engine {

	// One kind of agent a scene's navigation mesh is baked for (M30, Docs/Decisions/0045-m30-navigation.md decision 5): the
	// clearance its paths keep from walls (Radius), the headroom it needs (Height), the steepest slope it walks up and the
	// highest ledge it steps onto. Scripts name it in their queries (Navigation.FindPath's agent).
	struct NavAgentType
	{
		std::string Name = "Default"; // 1 to MaxNavAgentNameLength bytes, unique in its component
		float Radius = 0.4f;          // metres, [MinNavAgentRadius, MaxNavAgentRadius]
		float Height = 1.8f;          // metres, [MinNavAgentHeight, MaxNavAgentHeight]
		float MaxSlope = 45.0f;       // degrees, [0, MaxNavAgentSlope]
		float MaxStep = 0.4f;         // metres, [0, MaxNavAgentStep]
	};

	// Registry name "Navigation", version 1, category Navigation, unique per scene (M30, Docs/Decisions/0045-m30-navigation.md
	// decision 5): the scene's navigation mesh and how it is baked.
	//   - NavMesh is the baked mesh, which navigation.bake (EditorCore/Navigation/NavigationBake.h) writes as
	//     `Assets/Navigation/<scene path>.navmesh` and assigns here; null is not baked (NAV_MESH_NOT_BAKED), and every query
	//     then finds nothing.
	//   - AgentTypes: one to MaxNavAgentTypes agent types, each baked into a mesh of its own.
	//   - The bake reads the static, solid, creatable bodies (PhysicsComposition.h) on the physics layers Layers names (empty:
	//     every layer), voxelized in cells of CellSize by CellHeight metres; walkable regions smaller than MinRegionArea
	//     square metres are dropped (an island on a table top, a ledge), and every NavModifier marks its box.
	//   - Not script-visible: the fields are the editor's bake's input, and a play session reads only the baked mesh (run-time
	//     rebakes are out of scope, ADR 0042 decision 1), so the script API's `Navigation` is the query namespace alone.
	struct NavigationComponent
	{
		TypedAssetHandle<AssetType::NavMesh> NavMesh{};
		std::vector<NavAgentType> AgentTypes = { NavAgentType{} };
		std::vector<std::string> Layers{};
		float CellSize = 0.2f;      // metres, [MinNavCellSize, MaxNavCellSize]
		float CellHeight = 0.1f;    // metres, [MinNavCellSize, MaxNavCellSize]
		float MinRegionArea = 1.0f; // square metres, [0, 10000]
	};

}
