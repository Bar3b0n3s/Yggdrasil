#include "EditorPCH.h"
#include "EditorCore/Private/SceneEditRollback.h"

#include "EditorCore/Commands/SceneEditCommand.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/Scene.h"

#include <vector>

namespace Engine {

	namespace Utils {

		// The changes that bring the tracked entities back to their Before state from the scene's current state (After states
		// are left out: SceneEditCommand::ApplyChanges then reads the current state from the scene).
		static std::vector<SceneEntityChange> MakeRollbackChanges(std::span<const EntityChange> tracked, std::string_view label)
		{
			std::vector<SceneEntityChange> changes;
			changes.reserve(tracked.size());
			for (const EntityChange& change : tracked)
			{
				if (change.Kind != EntityChangeKind::Created && change.Before == nullptr)
				{
					ENGINE_ERROR("Rolling back '{}' cannot restore entity {}: its state before the edit could not be serialized", label, change.EntityID);
					continue;
				}
				changes.push_back(SceneEntityChange{ .EntityID = change.EntityID,
					.Before = change.Kind == EntityChangeKind::Created ? nullptr : change.Before,
					.After = nullptr,
					.ParentBefore = change.ParentBefore,
					.SiblingIndexBefore = change.SiblingIndexBefore,
					.ParentAfter = UUID(),
					.SiblingIndexAfter = 0 });
			}
			return changes;
		}

		void RollBackTrackedChanges(Scene& scene, std::span<const EntityChange> tracked, std::string_view label)
		{
			const std::vector<SceneEntityChange> changes = MakeRollbackChanges(tracked, label);
			const Status restored = SceneEditCommand::ApplyChanges(scene, changes, false);
			ENGINE_ASSERT(restored.has_value(), "rolling back '{}' failed: {}", label, restored ? std::string() : restored.error().ToString());
			if (!restored)
				ENGINE_ERROR("Rolling back '{}' failed: {}", label, restored.error().ToString());
		}

	}

}
