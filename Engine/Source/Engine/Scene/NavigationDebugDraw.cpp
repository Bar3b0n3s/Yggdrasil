#include "EnginePCH.h"
#include "Engine/Scene/NavigationDebugDraw.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/NavMeshData.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scene/NavigationValidation.h"

#include <array>

// The navigation view (NavigationDebugDraw.h; Docs/Decisions/0045-m30-navigation.md decision 12).

namespace Engine {

	glm::vec4 GetNavigationDebugColor(size_t agent, bool border)
	{
		constexpr std::array<glm::vec3, MaxNavAgentTypes> Colors = { glm::vec3(0.1f, 0.85f, 1.0f), glm::vec3(1.0f, 0.55f, 0.1f),
			glm::vec3(1.0f, 0.25f, 0.85f), glm::vec3(0.95f, 0.95f, 0.2f) };
		return glm::vec4(Colors[agent % Colors.size()], border ? 1.0f : 0.4f);
	}

	size_t AppendNavigationDebugDraw(const Scene& scene, AssetManager& assets, DebugDrawList& debug)
	{
		const AssetHandle handle = FindSceneNavMesh(scene);
		if (!handle.IsValid())
			return 0;
		const Result<AssetRef<Asset>> loaded = assets.Load(handle);
		if (!loaded.has_value())
			return 0;
		const AssetRef<NavMeshData> mesh = AssetCast<NavMeshData>(*loaded);
		if (mesh == nullptr)
			return 0;

		size_t lines = 0;
		const glm::vec3 lift(0.0f, NavigationDebugLift, 0.0f);
		for (size_t agentIndex = 0; agentIndex < mesh->Agents.size(); ++agentIndex)
		{
			const NavMeshAgent& agent = mesh->Agents[agentIndex];
			const size_t slots = agent.VerticesPerPolygon;
			for (size_t polygon = 0; polygon < agent.GetPolygonCount(); ++polygon)
			{
				const uint16_t* values = agent.Polygons.data() + polygon * 2 * slots;
				const uint32_t count = GetNavMeshPolygonVertexCount(agent, polygon);
				for (uint32_t edge = 0; edge < count; ++edge)
				{
					const uint16_t neighbour = values[slots + edge];
					// An edge between two polygons is drawn once, from the polygon of the lower index.
					if (neighbour != NavMeshNullIndex && neighbour < polygon)
						continue;
					const glm::vec4 color = GetNavigationDebugColor(agentIndex, neighbour == NavMeshNullIndex);
					debug.AddLine(GetNavMeshVertexPosition(agent, values[edge]) + lift, GetNavMeshVertexPosition(agent, values[(edge + 1) % count]) + lift, color);
					++lines;
				}
			}
		}
		return lines;
	}

}
