#include "EditorPCH.h"
#include "EditorCore/Commands/SceneEdit.h"

#include "EditorCore/Commands/SceneEditCommand.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		// The parent's UUID, or the invalid UUID for a root.
		static UUID GetParentID(ConstEntity entity)
		{
			const ConstEntity parent = entity.GetParent();
			return parent.IsValid() ? parent.GetUUID() : UUID();
		}

		// The changes that bring the tracked entities back to their Before state from the scene's current state (After
		// states are left out: SceneEditCommand::ApplyChanges then reads the current state from the scene). An entity that
		// existed before the edit but has no Before snapshot cannot be restored and is left out, with an error logged.
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

		// Restores the Before state of `tracked`; a failure is a bug (every Before state was valid), asserted and logged.
		static void RollBackTrackedChanges(Scene& scene, std::span<const EntityChange> tracked, std::string_view label)
		{
			const std::vector<SceneEntityChange> changes = MakeRollbackChanges(tracked, label);
			const Status restored = SceneEditCommand::ApplyChanges(scene, changes, false);
			ENGINE_ASSERT(restored.has_value(), "rolling back '{}' failed: {}", label, restored ? std::string() : restored.error().ToString());
			if (!restored)
				ENGINE_ERROR("Rolling back '{}' failed: {}", label, restored.error().ToString());
		}

	}

	SceneEdit::SceneEdit(EditorContext& context, std::string label, std::string mergeKey)
		: m_Context(&context), m_Label(std::move(label)), m_MergeKey(std::move(mergeKey))
	{
		ENGINE_ASSERT(context.HasScene(), "SceneEdit '{}' needs an open scene", m_Label);
		Scene& scene = context.GetScene();
		ENGINE_ASSERT(!scene.GetChangeTracker().IsTracking(), "SceneEdit '{}': another SceneEdit of scene '{}' is active (edits do not nest)",
			m_Label, scene.GetName());
		m_RevisionBefore = context.GetRevision();
#if defined(ENGINE_DEBUG)
		// §12.3: Commit checks that the state of every entity the tracker does not report is unchanged. The state compared
		// is the canonical entity JSON, the input of the state hash, so the check is exact and needs no hashing.
		const Scene& constScene = scene;
		m_EntityStates.reserve(scene.GetEntityCount());
		constScene.ForEachCanonical([this](ConstEntity entity)
		{
			Result<Json> json = SceneSerializer::EntityToJson(entity);
			m_EntityStates.emplace_back(entity.GetUUID(), json ? std::move(*json) : Json());
		});
#endif
		scene.GetChangeTracker().Begin();
	}

	SceneEdit::~SceneEdit()
	{
		Cancel();
	}

	Result<uint64_t> SceneEdit::Commit()
	{
		ENGINE_ASSERT(m_IsActive, "SceneEdit '{}' committed after it ended", m_Label);
		if (!m_IsActive)
			return 0;
		m_IsActive = false;
		Scene& scene = GetScene();
		const std::vector<EntityChange> tracked = scene.GetChangeTracker().End();
#if defined(ENGINE_DEBUG)
		// §12.3: every entity the tracker did not report is unchanged (see the constructor).
		for (const auto& [id, state] : m_EntityStates)
		{
			// ChangeTracker::End sorts by UUID.
			const auto found = std::lower_bound(tracked.begin(), tracked.end(), id, [](const EntityChange& change, UUID value)
			{
				return change.EntityID < value;
			});
			if (found != tracked.end() && found->EntityID == id)
				continue;
			const ConstEntity entity = std::as_const(scene).FindEntityByID(id);
			ENGINE_ASSERT(entity.IsValid() && SceneSerializer::EntityToJson(entity) == state,
				"SceneEdit '{}' changed entity {} without the change tracker recording it", m_Label, id);
		}
		m_EntityStates.clear();
#endif

		if (tracked.empty())
			return 0;

		std::vector<SceneEntityChange> changes;
		changes.reserve(tracked.size());
		for (const EntityChange& change : tracked)
		{
			if (change.Kind != EntityChangeKind::Created && change.Before == nullptr)
			{
				Utils::RollBackTrackedChanges(scene, tracked, m_Label);
				return MakeError(ErrorCode::Validation, "'{}' cannot be recorded: the state of entity {} before the edit could not be serialized", m_Label,
					change.EntityID);
			}
			SceneEntityChange recorded{ .EntityID = change.EntityID,
				.Before = change.Kind == EntityChangeKind::Created ? nullptr : change.Before,
				.After = nullptr,
				.ParentBefore = change.ParentBefore,
				.SiblingIndexBefore = change.SiblingIndexBefore,
				.ParentAfter = UUID(),
				.SiblingIndexAfter = 0 };
			if (const ConstEntity entity = std::as_const(scene).FindEntityByID(change.EntityID); entity.IsValid())
			{
				Result<Json> after = SceneSerializer::EntityToJson(entity);
				if (!after)
				{
					Utils::RollBackTrackedChanges(scene, tracked, m_Label);
					return std::unexpected(std::move(after).error().WithContext(std::format("recording '{}'", m_Label)));
				}
				recorded.After = CreateRef<const Json>(std::move(*after));
				recorded.ParentAfter = Utils::GetParentID(entity);
				recorded.SiblingIndexAfter = entity.GetSiblingIndex();
			}
			if (recorded.Before == nullptr && recorded.After == nullptr)
				continue; // ChangeTracker never reports these, but such a change would have nothing to restore
			changes.push_back(std::move(recorded));
		}
		if (changes.empty())
			return 0;

		// Kept to roll back if the context refuses the command (a read-only project); the snapshots are shared.
		std::vector<SceneEntityChange> rollback = changes;
		m_Context->m_PendingRevisionBefore = m_RevisionBefore;
		Result<uint64_t> executed = m_Context->Execute(CreateScope<SceneEditCommand>(m_Label, std::move(changes), m_MergeKey));
		m_Context->m_PendingRevisionBefore.reset();
		if (!executed)
		{
			const Status restored = SceneEditCommand::ApplyChanges(scene, rollback, false);
			ENGINE_ASSERT(restored.has_value(), "rolling back '{}' failed: {}", m_Label, restored ? std::string() : restored.error().ToString());
			if (!restored)
				ENGINE_ERROR("Rolling back '{}' failed: {}", m_Label, restored.error().ToString());
			return std::unexpected(std::move(executed).error());
		}
		// The command was built applied, so its Execute appended nothing: the events of the edit are appended here, once it
		// is recorded (a dry run suppresses them).
		SceneEditCommand::AppendChangeEvents(*m_Context, rollback, true);
		return *executed;
	}

	void SceneEdit::Cancel()
	{
		if (!m_IsActive)
			return;
		m_IsActive = false;
		Scene& scene = GetScene();
		const std::vector<EntityChange> tracked = scene.GetChangeTracker().End();
		m_EntityStates.clear();
		if (!tracked.empty())
			Utils::RollBackTrackedChanges(scene, tracked, m_Label);
	}

	Scene& SceneEdit::GetScene() const
	{
		return m_Context->GetScene();
	}

}
