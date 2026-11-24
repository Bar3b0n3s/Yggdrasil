#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>

namespace Engine {

	// Registry enum "StaticScope" (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3): the part of the hierarchy a
	// Static component covers.
	enum class StaticScope : uint8_t
	{
		Subtree,    // the entity and its descendants are static
		EntityOnly, // the entity alone is static; its descendants follow the Static components above it
		NotStatic   // the entity and its descendants are not static, even below a static ancestor
	};

	// Registry name "Static" (Architecture §5.2, §5.3): a promise that the entity's transform, renderer and colliders rarely
	// change, so the renderer keeps its meshes in the scene's static set (§8.3) instead of extracting them every frame.
	// Walking up from an entity, the first Static met decides: its own with any Scope but NotStatic makes it static; an
	// ancestor's Subtree makes it static and NotStatic makes it not, while EntityOnly is passed by. The scene keeps the
	// runtime-only HierarchyStaticTag on every effectively static entity. Writing to a static entity is allowed and correct:
	// it rebuilds its static cell and renders at its current pose, without interpolation.
	struct StaticComponent
	{
		StaticScope Scope = StaticScope::Subtree;
	};

}
