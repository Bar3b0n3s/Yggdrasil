#pragma once

#include "Engine/Core/Base.h"

#include <span>
#include <string_view>

// What SceneEdit and EditorContext's prefab updates share: putting the entities a ChangeTracker reports back into their
// state from before the tracked edit, through SceneEditCommand::ApplyChanges (the one scene-copy path, §1.3).

namespace Engine {

	class Scene;
	struct EntityChange;

	namespace Utils {

		// Restores the Before state of every entity `tracked` reports (removing the ones the edit created, restoring the ones
		// it changed or destroyed). An entity that existed before the edit but has no Before snapshot cannot be restored: it
		// is left out with an Error logged that names `label`. A failure of the restore itself is a bug (every Before state
		// was valid when it was captured): asserted, and logged in configurations without asserts.
		void RollBackTrackedChanges(Scene& scene, std::span<const EntityChange> tracked, std::string_view label);

	}

}
