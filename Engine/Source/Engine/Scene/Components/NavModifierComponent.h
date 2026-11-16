#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// What a NavModifier does to the navigation mesh inside its box.
	enum class NavModifierKind : uint8_t
	{
		NotWalkable, // no agent walks there: the box cuts a hole in the mesh (a lava channel, a trap)
		Cost         // walkable at Cost times the length: paths avoid it unless there is no cheaper way
	};

	// Registry name "NavModifier", version 1, category Navigation (M30, Docs/Decisions/0045-m30-navigation.md decision 5): a box
	// of HalfExtents metres centred on the entity, scaled by its world scale and turned by its world rotation's yaw only (its
	// pitch and roll are ignored: the box's sides stay vertical), that marks the navigation mesh where the bake voxelizes it.
	// Requires Transform. Where boxes overlap, NotWalkable wins, then the highest Cost. Like the Navigation component it is
	// the bake's input only, so it is not script-visible.
	struct NavModifierComponent
	{
		NavModifierKind Kind = NavModifierKind::NotWalkable;
		float Cost = 4.0f;                       // [1, MaxNavAreaCost]; used when Kind is Cost
		glm::vec3 HalfExtents = glm::vec3(0.5f); // metres, each in [0.01, 10000]
	};

}
