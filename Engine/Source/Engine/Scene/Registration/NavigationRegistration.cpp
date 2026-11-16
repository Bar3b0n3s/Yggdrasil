#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Asset/NavMeshData.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <format>
#include <set>
#include <string>
#include <string_view>

// The Navigation category (M30, Docs/Decisions/0045-m30-navigation.md decision 5; ADR 0042's phase-5 ledger: after Terrain,
// in landing order): the scene's Navigation (its baked mesh, agent types and bake settings) and the NavModifier boxes that
// mark areas of it. Both are the editor's bake's input, so neither is script-visible: scripts reach the baked mesh through
// the Navigation namespace's queries.

namespace Engine {

	namespace Utils {

		// One to MaxNavAgentTypes agent types with unique names of 1 to MaxNavAgentNameLength bytes, and unique, non-empty
		// layer names.
		static void ValidateNavigation(const NavigationComponent& navigation, ValidationContext& context)
		{
			if (navigation.AgentTypes.empty() || navigation.AgentTypes.size() > MaxNavAgentTypes)
				context.Error("AgentTypes", std::format("must hold 1 to {} agent types, not {}", MaxNavAgentTypes, navigation.AgentTypes.size()));
			std::set<std::string_view> names;
			for (const NavAgentType& agent : navigation.AgentTypes)
			{
				if (agent.Name.empty() || agent.Name.size() > MaxNavAgentNameLength)
				{
					context.Error("AgentTypes", std::format("agent type '{}' must have a name of 1 to {} bytes", agent.Name, MaxNavAgentNameLength));
				}
				else if (!names.insert(agent.Name).second)
				{
					context.Error("AgentTypes", std::format("two agent types are named '{}'", agent.Name));
				}
			}
			std::set<std::string_view> layers;
			for (const std::string& layer : navigation.Layers)
			{
				if (layer.empty())
					context.Error("Layers", "a layer name is empty");
				else if (!layers.insert(layer).second)
					context.Error("Layers", std::format("the layer '{}' is named twice", layer));
			}
		}

		static void MakeNavigationValid(NavigationComponent& navigation, Random& /*random*/)
		{
			if (navigation.AgentTypes.size() > MaxNavAgentTypes)
				navigation.AgentTypes.resize(MaxNavAgentTypes);
			if (navigation.AgentTypes.empty())
				navigation.AgentTypes.emplace_back();
			for (size_t index = 0; index < navigation.AgentTypes.size(); ++index)
				navigation.AgentTypes[index].Name = std::format("Agent{}", index);
			std::set<std::string> seen;
			std::erase_if(navigation.Layers, [&seen](const std::string& layer)
			{
				return layer.empty() || !seen.insert(layer).second;
			});
		}

	}

	void RegisterNavigationComponents(TypeRegistry& registry)
	{
		registry.Enum<NavModifierKind>("NavModifierKind", "What a NavModifier does to the navigation mesh inside its box.")
			.Entry(NavModifierKind::NotWalkable, "NotWalkable", "No agent walks there: the box cuts a hole in the navigation mesh.")
			.Entry(NavModifierKind::Cost, "Cost", "Walkable at Cost times the distance, so paths avoid it unless no cheaper way exists.");

		registry.Struct<NavAgentType>("NavAgentType", "One kind of agent the scene's navigation mesh is baked for, which queries name.")
			.Field("Name", &NavAgentType::Name, "The name queries give (Navigation.FindPath's agent); unique in its component.")
			.Field("Radius", &NavAgentType::Radius, "The clearance the agent's paths keep from walls and edges, in metres.",
				{ .Min = static_cast<double>(MinNavAgentRadius), .Max = static_cast<double>(MaxNavAgentRadius), .Unit = "m" })
			.Field("Height", &NavAgentType::Height, "The headroom the agent needs to pass under an obstacle, in metres.",
				{ .Min = static_cast<double>(MinNavAgentHeight), .Max = static_cast<double>(MaxNavAgentHeight), .Unit = "m" })
			.Field("MaxSlope", &NavAgentType::MaxSlope, "The steepest slope the agent walks up, in degrees.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxNavAgentSlope), .Unit = "deg" })
			.Field("MaxStep", &NavAgentType::MaxStep, "The highest ledge the agent steps onto, in metres.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxNavAgentStep), .Unit = "m" });

		RegisterComponent<NavigationComponent>(registry, "Navigation",
			"The scene's navigation mesh (one per scene): its agent types and bake settings, and the mesh navigation.bake builds from the "
			"scene's static colliders for the Navigation script namespace's queries.")
			.Category("Navigation")
			.Version(1)
			.Flags(ComponentFlags::UniquePerScene)
			.RemoveFlags(ComponentFlags::ScriptVisible)
			.Field("NavMesh", &NavigationComponent::NavMesh, "The baked navigation mesh, which navigation.bake writes and assigns; null finds no path.")
			.Field("AgentTypes", &NavigationComponent::AgentTypes, "One to four agent types, each baked into a mesh of its own.")
			.Field("Layers", &NavigationComponent::Layers,
				"The physics layers whose static colliders the bake reads, by name from project settings; empty reads every layer.")
			.Field("CellSize", &NavigationComponent::CellSize,
				"The width of the bake's voxels, in metres: smaller follows walls more closely and bakes more slowly.",
				{ .Min = static_cast<double>(MinNavCellSize), .Max = static_cast<double>(MaxNavCellSize), .Unit = "m" })
			.Field("CellHeight", &NavigationComponent::CellHeight, "The height of the bake's voxels, in metres: the precision of steps and slopes.",
				{ .Min = static_cast<double>(MinNavCellSize), .Max = static_cast<double>(MaxNavCellSize), .Unit = "m" })
			.Field("MinRegionArea", &NavigationComponent::MinRegionArea,
				"Walkable islands smaller than this many square metres are left out of the mesh (a table top, a narrow ledge).",
				{ .Min = 0.0, .Max = 10000.0, .Unit = "m2" })
			.Validate(&Utils::ValidateNavigation)
			.Generate(&Utils::MakeNavigationValid);

		RegisterComponent<NavModifierComponent>(registry, "NavModifier",
			"A box around the entity that marks the navigation mesh at the next bake: not walkable, or walkable at a cost that paths avoid.")
			.Category("Navigation")
			.Version(1)
			.RemoveFlags(ComponentFlags::ScriptVisible)
			.Requires<TransformComponent>()
			.Field("Kind", &NavModifierComponent::Kind, "Whether the box is not walkable or walkable at Cost.")
			.Field("Cost", &NavModifierComponent::Cost, "How many times the distance a path inside the box costs (Kind Cost).",
				{ .Min = 1.0, .Max = static_cast<double>(MaxNavAreaCost) })
			.Field("HalfExtents", &NavModifierComponent::HalfExtents,
				"Half the box's size along the entity's axes, in metres; the entity's scale applies and its rotation's yaw turns it.",
				{ .Min = 0.01, .Max = 10000.0, .Unit = "m" });
	}

}
